#include "audio/Routing.h"

#include "audio/Wasapi.h"
#include "util/Log.h"

#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <inspectable.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <roapi.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;

namespace audio {
namespace {

// Undocumented IPolicyConfig (Win7 .. Win11). Only SetDefaultEndpoint is called; the rest keeps the vtable exact.
constexpr GUID kClsidPolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
constexpr GUID kIidPolicyConfig = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

struct IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

// Undocumented Windows.Media.Internal.AudioPolicyConfig activation factory. Both IIDs share this layout; slots 6..24
// are unrelated volume-group/chat APIs we never call.
constexpr GUID kIidAudioPolicyConfigWin11 = {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}};
constexpr GUID kIidAudioPolicyConfigDownlevel = {
    0x2a59116d, 0x6c4f, 0x45e0, {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}};
constexpr uint32_t kAudioPolicyConfigWin11Build = 21390;

struct IAudioPolicyConfigFactory : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Slot06() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot07() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot08() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot09() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot10() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot11() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot12() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot13() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot14() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot15() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot16() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot17() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot18() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot19() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot20() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot21() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot22() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot23() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot24() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(UINT32 processId, EDataFlow flow, ERole role,
                                                                       HSTRING deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(UINT32 processId, EDataFlow flow, ERole role,
                                                                       HSTRING* deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

// The per-app policy store addresses endpoints by their SWD interface path, not the plain MMDevice id.
constexpr wchar_t kSwdPrefix[] = L"\\\\?\\SWD#MMDEVAPI#";
constexpr wchar_t kSwdRenderSuffix[] = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";

constexpr char kJournalHeader[] = "spatiall-routing-journal 2";
// Version 1 also journaled session mutes (removed: process loopback taps after session mute).
constexpr char kJournalHeaderV1[] = "spatiall-routing-journal 1";

// Callers may be on an STA UI thread, an MTA worker, or a thread without COM; RPC_E_CHANGED_MODE leaves the
// existing apartment in place and is not balanced with CoUninitialize.
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

std::mutex g_journalMutex;

std::string describeDevice(const std::wstring& id) {
    if (id.empty()) return "<system default>";
    const std::string name = deviceName(id);
    return name.empty() ? logx::narrow(id) : name + " [" + logx::narrow(id) + "]";
}

bool endsWithInsensitive(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + (s.size() - n), suffix) == 0;
}

ComPtr<IAudioPolicyConfigFactory> policyFactory() {
    const bool win11 = windowsBuild() >= kAudioPolicyConfigWin11Build;
    const GUID* order[] = {win11 ? &kIidAudioPolicyConfigWin11 : &kIidAudioPolicyConfigDownlevel,
                           win11 ? &kIidAudioPolicyConfigDownlevel : &kIidAudioPolicyConfigWin11};
    const HStringReference cls(L"Windows.Media.Internal.AudioPolicyConfig");
    HRESULT hr = E_FAIL;
    for (const GUID* iid : order) {
        ComPtr<IAudioPolicyConfigFactory> f;
        hr = RoGetActivationFactory(cls.Get(), *iid, reinterpret_cast<void**>(f.GetAddressOf()));
        if (SUCCEEDED(hr) && f) return f;
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) logx::warn("Routing: AudioPolicyConfig factory unavailable: " + logx::hr(hr));
    return nullptr;
}

std::wstring exePath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    std::wstring buf(1024, L'\0');
    DWORD size = static_cast<DWORD>(buf.size());
    const BOOL ok = QueryFullProcessImageNameW(h, 0, buf.data(), &size);
    CloseHandle(h);
    if (!ok) return {};
    buf.resize(size);
    return buf;
}

// Calls fn(pid, session) for every non-system render session on every active render endpoint.
template <typename Fn>
void forEachRenderSession(Fn&& fn) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) return;
    UINT deviceCount = 0;
    if (FAILED(devices->GetCount(&deviceCount))) return;
    for (UINT d = 0; d < deviceCount; ++d) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(d, &device))) continue;
        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(manager.GetAddressOf()))))
            continue;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager->GetSessionEnumerator(&sessions))) continue;
        int sessionCount = 0;
        if (FAILED(sessions->GetCount(&sessionCount))) continue;
        for (int i = 0; i < sessionCount; ++i) {
            ComPtr<IAudioSessionControl> control;
            if (FAILED(sessions->GetSession(i, &control))) continue;
            ComPtr<IAudioSessionControl2> control2;
            if (FAILED(control.As(&control2))) continue;
            if (control2->IsSystemSoundsSession() == S_OK) continue;
            // AUDCLNT_S_NO_SINGLE_PROCESS is a success code; pid is then the session's creating process.
            DWORD pid = 0;
            if (FAILED(control2->GetProcessId(&pid)) || pid == 0) continue;
            fn(pid, control2.Get());
        }
    }
}

std::wstring g_journalName = L"routing-journal.txt";  // see RoutingJournal::useFile

std::filesystem::path journalPath() {
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) return {};
    std::filesystem::path dir = std::filesystem::path(base) / L"Spatiall";
    CoTaskMemFree(base);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / g_journalName;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find('\t', start);
        out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) return out;
        start = tab + 1;
    }
}

bool parseUInt(const std::string& s, uint32_t& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 10);
    if (*end != '\0') return false;
    out = static_cast<uint32_t>(v);
    return true;
}

void restoreAppEndpoint(uint32_t pid, const std::wstring& originalEndpoint) {
    if (!setAppRenderEndpoint(pid, originalEndpoint))
        logx::warn("Routing: could not restore endpoint of pid " + std::to_string(pid) + " (process may have exited)");
}

}  // namespace

std::vector<AppSession> listAppSessions() {
    const ComScope com;
    struct Entry {
        AppSession session;
        bool namedBySession = false;
    };
    std::map<uint32_t, Entry> byPid;
    const DWORD self = GetCurrentProcessId();

    forEachRenderSession([&](DWORD pid, IAudioSessionControl2* control) {
        if (pid == self) return;
        AudioSessionState state = AudioSessionStateInactive;
        const bool active = SUCCEEDED(control->GetState(&state)) && state == AudioSessionStateActive;

        std::string sessionName;
        LPWSTR name = nullptr;
        if (SUCCEEDED(control->GetDisplayName(&name)) && name) {
            // "@dll,-id" names are indirect resource strings; the exe name is more useful than resolving them.
            if (name[0] != L'\0' && name[0] != L'@') sessionName = logx::narrow(name);
            CoTaskMemFree(name);
        }

        auto it = byPid.find(pid);
        if (it == byPid.end()) {
            Entry e;
            e.session.pid = pid;
            const std::wstring path = exePath(pid);
            e.session.exe = logx::narrow(path.substr(path.find_last_of(L"\\/") + 1));
            if (!sessionName.empty()) {
                e.session.displayName = sessionName;
                e.namedBySession = true;
            } else if (!e.session.exe.empty()) {
                std::string base = e.session.exe;
                if (base.size() > 4 && _stricmp(base.c_str() + base.size() - 4, ".exe") == 0)
                    base.resize(base.size() - 4);
                e.session.displayName = base;
            } else {
                e.session.displayName = "PID " + std::to_string(pid);
            }
            it = byPid.emplace(pid, std::move(e)).first;
        } else if (!it->second.namedBySession && !sessionName.empty()) {
            it->second.session.displayName = sessionName;
            it->second.namedBySession = true;
        }
        it->second.session.active = it->second.session.active || active;
    });

    std::vector<AppSession> out;
    out.reserve(byPid.size());
    for (auto& entry : byPid) out.push_back(std::move(entry.second.session));
    std::sort(out.begin(), out.end(), [](const AppSession& a, const AppSession& b) {
        if (a.active != b.active) return a.active;
        const int c = _stricmp(a.displayName.c_str(), b.displayName.c_str());
        return c != 0 ? c < 0 : a.pid < b.pid;
    });
    return out;
}

bool setDefaultRenderEndpoint(const std::wstring& deviceId) {
    const ComScope com;
    ComPtr<IPolicyConfig> config;
    HRESULT hr = CoCreateInstance(kClsidPolicyConfigClient, nullptr, CLSCTX_ALL, kIidPolicyConfig,
                                  reinterpret_cast<void**>(config.GetAddressOf()));
    if (FAILED(hr)) {
        logx::error("Routing: IPolicyConfig unavailable: " + logx::hr(hr));
        return false;
    }
    bool ok = true;
    for (ERole role : {eConsole, eMultimedia, eCommunications}) {
        hr = config->SetDefaultEndpoint(deviceId.c_str(), role);
        if (FAILED(hr)) {
            logx::error("Routing: SetDefaultEndpoint(role " + std::to_string(role) + ") failed: " + logx::hr(hr));
            ok = false;
        }
    }
    if (ok) logx::info("Routing: system default render endpoint -> " + describeDevice(deviceId));
    return ok;
}

bool perAppRoutingSupported() {
    const ComScope com;
    static const bool supported = policyFactory() != nullptr;
    return supported;
}

bool setAppRenderEndpoint(uint32_t pid, const std::wstring& deviceId) {
    const ComScope com;
    const ComPtr<IAudioPolicyConfigFactory> factory = policyFactory();
    if (!factory) return false;

    // A null HSTRING clears the app's persisted endpoint so it follows the system default again.
    const std::wstring path = deviceId.empty() ? std::wstring() : kSwdPrefix + deviceId + kSwdRenderSuffix;
    const HStringReference ref(path.c_str(), static_cast<unsigned int>(path.size()));
    const HSTRING value = deviceId.empty() ? nullptr : ref.Get();

    bool ok = true;
    for (ERole role : {eMultimedia, eConsole}) {
        const HRESULT hr = factory->SetPersistedDefaultAudioEndpoint(pid, eRender, role, value);
        if (FAILED(hr)) {
            logx::warn("Routing: SetPersistedDefaultAudioEndpoint(pid " + std::to_string(pid) + ", role " +
                       std::to_string(role) + ") failed: " + logx::hr(hr));
            ok = false;
        }
    }
    if (ok) logx::info("Routing: pid " + std::to_string(pid) + " render endpoint -> " + describeDevice(deviceId));
    return ok;
}

std::wstring getAppRenderEndpoint(uint32_t pid) {
    const ComScope com;
    const ComPtr<IAudioPolicyConfigFactory> factory = policyFactory();
    if (!factory) return {};
    HString value;
    const HRESULT hr = factory->GetPersistedDefaultAudioEndpoint(pid, eRender, eMultimedia, value.GetAddressOf());
    if (FAILED(hr)) {
        logx::warn("Routing: GetPersistedDefaultAudioEndpoint(pid " + std::to_string(pid) + ") failed: " +
                   logx::hr(hr));
        return {};
    }
    unsigned int len = 0;
    const wchar_t* raw = value.GetRawBuffer(&len);
    const std::wstring path(raw ? raw : L"", raw ? len : 0);
    if (path.empty()) return {};

    const size_t prefixLen = wcslen(kSwdPrefix);
    const size_t suffixLen = wcslen(kSwdRenderSuffix);
    if (path.size() <= prefixLen + suffixLen || _wcsnicmp(path.c_str(), kSwdPrefix, prefixLen) != 0 ||
        !endsWithInsensitive(path, kSwdRenderSuffix)) {
        logx::warn("Routing: unexpected persisted endpoint format for pid " + std::to_string(pid) + ": " +
                   logx::narrow(path));
        return {};
    }
    return path.substr(prefixLen, path.size() - prefixLen - suffixLen);
}

RoutingJournal& RoutingJournal::instance() {
    static RoutingJournal journal;
    return journal;
}

void RoutingJournal::useFile(const std::wstring& fileName) {
    std::lock_guard lock(g_journalMutex);
    g_journalName = fileName;
}

bool RoutingJournal::redirectSystemDefault(const std::wstring& toDeviceId) {
    const ComScope com;  // defaultRenderDeviceId/deviceName need COM on this thread
    std::lock_guard lock(g_journalMutex);
    const bool firstRedirect = originalDefault_.empty();
    if (firstRedirect) {
        const std::wstring current = defaultRenderDeviceId();
        if (current.empty()) {
            logx::error("Routing: cannot redirect system default: no current default render endpoint");
            return false;
        }
        // Journal before changing anything so a crash mid-change is still recoverable.
        originalDefault_ = current;
        save();
        logx::info("Routing journal: original system default " + describeDevice(current));
    }
    if (setDefaultRenderEndpoint(toDeviceId)) return true;
    if (firstRedirect) {
        originalDefault_.clear();
        save();
    }
    return false;
}

bool RoutingJournal::redirectApp(uint32_t pid, const std::wstring& toDeviceId) {
    const ComScope com;
    std::lock_guard lock(g_journalMutex);
    const bool newEntry = std::none_of(apps_.begin(), apps_.end(), [&](const AppEntry& e) { return e.pid == pid; });
    if (newEntry) {
        const std::wstring original = getAppRenderEndpoint(pid);
        apps_.push_back(AppEntry{pid, original});
        save();
        logx::info("Routing journal: pid " + std::to_string(pid) + " original endpoint " + describeDevice(original));
    }
    if (setAppRenderEndpoint(pid, toDeviceId)) return true;
    if (newEntry) {
        apps_.pop_back();
        save();
    }
    return false;
}

void RoutingJournal::restoreAll() {
    const ComScope com;
    std::lock_guard lock(g_journalMutex);
    if (originalDefault_.empty() && apps_.empty()) {
        save();  // removes a stale journal file, if any
        return;
    }
    logx::info("Routing journal: restoring " + std::to_string(apps_.size()) + " app(s)" +
               (originalDefault_.empty() ? std::string() : " and the system default"));
    for (auto it = apps_.rbegin(); it != apps_.rend(); ++it)
        restoreAppEndpoint(it->pid, it->originalEndpoint);
    if (!originalDefault_.empty() && !setDefaultRenderEndpoint(originalDefault_))
        logx::warn("Routing: could not restore system default " + describeDevice(originalDefault_));
    apps_.clear();
    originalDefault_.clear();
    save();
}

void RoutingJournal::restoreApp(uint32_t pid) {
    std::lock_guard lock(g_journalMutex);
    auto it = std::find_if(apps_.begin(), apps_.end(), [&](const AppEntry& e) { return e.pid == pid; });
    if (it == apps_.end()) return;
    logx::info("Routing journal: restoring pid " + std::to_string(pid));
    restoreAppEndpoint(it->pid, it->originalEndpoint);
    apps_.erase(it);
    save();
}

void RoutingJournal::recoverFromCrash() {
    {
        std::lock_guard lock(g_journalMutex);
        const std::filesystem::path path = journalPath();
        std::error_code ec;
        if (path.empty() || !std::filesystem::exists(path, ec)) return;

        std::ifstream in(path, std::ios::binary);
        std::string line;
        if (!in || !std::getline(in, line)) {
            logx::warn("Routing journal: unreadable journal left by a previous run; discarding");
        } else {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const bool v1 = line == kJournalHeaderV1;
            if (!v1 && line != kJournalHeader) {
                logx::warn("Routing journal: unknown journal format; discarding");
            } else {
                while (std::getline(in, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    const std::vector<std::string> f = splitTabs(line);
                    if (f.size() == 2 && f[0] == "default") {
                        if (originalDefault_.empty()) originalDefault_ = logx::widen(f[1]);
                        continue;
                    }
                    // v2: app <pid> <original>; v1: app <pid> <routed> <muted> <original> (mute column ignored).
                    uint32_t pid = 0;
                    if (f.size() == (v1 ? 5u : 3u) && f[0] == "app" && parseUInt(f[1], pid)) {
                        const bool known = std::any_of(apps_.begin(), apps_.end(),
                                                       [&](const AppEntry& e) { return e.pid == pid; });
                        if (!known && (!v1 || f[2] == "1")) apps_.push_back(AppEntry{pid, logx::widen(f.back())});
                        continue;
                    }
                    if (!line.empty()) logx::warn("Routing journal: ignoring malformed line: " + line);
                }
            }
        }
        in.close();
        logx::warn("Routing journal: previous run left routing changes behind (" + std::to_string(apps_.size()) +
                   " app(s)" + (originalDefault_.empty() ? std::string() : ", system default") + "); reverting");
    }
    restoreAll();
}

void RoutingJournal::save() const {
    const std::filesystem::path path = journalPath();
    if (path.empty()) {
        logx::error("Routing journal: cannot resolve %LOCALAPPDATA%");
        return;
    }
    if (originalDefault_.empty() && apps_.empty()) {
        if (!DeleteFileW(path.c_str())) {
            const DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
                logx::error("Routing journal: delete failed: " + logx::hr(HRESULT_FROM_WIN32(err)));
        }
        return;
    }

    std::ostringstream out;
    out << kJournalHeader << '\n';
    if (!originalDefault_.empty()) out << "default\t" << logx::narrow(originalDefault_) << '\n';
    for (const AppEntry& e : apps_)
        out << "app\t" << e.pid << '\t' << logx::narrow(e.originalEndpoint) << '\n';
    const std::string data = out.str();

    // Write a temp file and swap it in so a crash mid-write never leaves a truncated journal.
    std::filesystem::path tmp = path;
    tmp += L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        logx::error("Routing journal: create failed: " + logx::hr(HRESULT_FROM_WIN32(GetLastError())));
        return;
    }
    DWORD written = 0;
    const bool wrote = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                       written == data.size() && FlushFileBuffers(h);
    const DWORD writeErr = GetLastError();
    CloseHandle(h);
    if (!wrote) {
        logx::error("Routing journal: write failed: " + logx::hr(HRESULT_FROM_WIN32(writeErr)));
        DeleteFileW(tmp.c_str());
        return;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        logx::error("Routing journal: replace failed: " + logx::hr(HRESULT_FROM_WIN32(GetLastError())));
        DeleteFileW(tmp.c_str());
    }
}

}  // namespace audio
