#include "audio/Wasapi.h"

#include "util/Log.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <propkeydef.h>  // DEFINE_PROPERTYKEY, needed by the devpkey header under WIN32_LEAN_AND_MEAN
#include <functiondiscoverykeys_devpkey.h>
#include <cfgmgr32.h>
#include <devicetopology.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <string_view>

using Microsoft::WRL::ComPtr;

namespace audio {
namespace {

constexpr REFERENCE_TIME kHnsPerSec = 10'000'000;

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

struct MmcssScope {
    HANDLE h = nullptr;
    MmcssScope() {
        DWORD idx = 0;
        h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
    }
    ~MmcssScope() {
        if (h) AvRevertMmThreadCharacteristics(h);
    }
};

StreamFormat toStreamFormat(const WAVEFORMATEX* wf) {
    StreamFormat f;
    f.sampleRate = wf->nSamplesPerSec;
    f.channels = wf->nChannels;
    f.bits = wf->wBitsPerSample;
    f.validBits = wf->wBitsPerSample;
    f.isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    f.channelMask = wf->nChannels == 1 ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        f.isFloat = ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        f.validBits = ext->Samples.wValidBitsPerSample ? ext->Samples.wValidBitsPerSample : wf->wBitsPerSample;
        f.channelMask = ext->dwChannelMask;
    }
    return f;
}

float sampleAt(const BYTE* p, const StreamFormat& f) {
    if (f.isFloat) {
        if (f.bits == 32) return *reinterpret_cast<const float*>(p);
        return static_cast<float>(*reinterpret_cast<const double*>(p));
    }
    switch (f.bits) {
        case 16: return *reinterpret_cast<const int16_t*>(p) / 32768.0f;
        case 24: {
            const int32_t v = (static_cast<int32_t>(p[2]) << 24) | (p[1] << 16) | (p[0] << 8);
            return v / 2147483648.0f;
        }
        case 32: return *reinterpret_cast<const int32_t*>(p) / 2147483648.0f;
        case 8: return (static_cast<int>(p[0]) - 128) / 128.0f;
        default: return 0.0f;
    }
}

void storeSample(BYTE* p, const StreamFormat& f, float v) {
    v = std::clamp(v, -1.0f, 1.0f);
    if (f.isFloat) {
        if (f.bits == 32) *reinterpret_cast<float*>(p) = v;
        else *reinterpret_cast<double*>(p) = v;
        return;
    }
    switch (f.bits) {
        case 16: *reinterpret_cast<int16_t*>(p) = static_cast<int16_t>(std::lrint(v * 32767.0f)); break;
        case 24: {
            const int32_t s = static_cast<int32_t>(std::lrint(v * 8388607.0f));
            p[0] = static_cast<BYTE>(s & 0xFF);
            p[1] = static_cast<BYTE>((s >> 8) & 0xFF);
            p[2] = static_cast<BYTE>((s >> 16) & 0xFF);
            break;
        }
        case 32: *reinterpret_cast<int32_t*>(p) = static_cast<int32_t>(std::lrint(v * 2147483647.0)); break;
        case 8: p[0] = static_cast<BYTE>(std::lrint(v * 127.0f) + 128); break;
        default: break;
    }
}

// Per-channel stereo fold-down gains derived from the channel mask (ITU-style, LFE dropped).
struct Downmix {
    std::vector<float> gl, gr;
    explicit Downmix(const StreamFormat& f) {
        gl.assign(f.channels, 0.0f);
        gr.assign(f.channels, 0.0f);
        if (f.channels == 1) {
            gl[0] = gr[0] = 1.0f;
            return;
        }
        constexpr float k = 0.70710678f;
        int ch = 0;
        for (DWORD bit = 1; bit != 0 && ch < f.channels; bit <<= 1) {
            if (!(f.channelMask & bit)) continue;
            switch (bit) {
                case SPEAKER_FRONT_LEFT: gl[ch] = 1.0f; break;
                case SPEAKER_FRONT_RIGHT: gr[ch] = 1.0f; break;
                case SPEAKER_FRONT_CENTER: gl[ch] = gr[ch] = k; break;
                case SPEAKER_LOW_FREQUENCY: break;
                case SPEAKER_BACK_LEFT:
                case SPEAKER_SIDE_LEFT:
                case SPEAKER_FRONT_LEFT_OF_CENTER: gl[ch] = k; break;
                case SPEAKER_BACK_RIGHT:
                case SPEAKER_SIDE_RIGHT:
                case SPEAKER_FRONT_RIGHT_OF_CENTER: gr[ch] = k; break;
                case SPEAKER_BACK_CENTER: gl[ch] = gr[ch] = 0.5f; break;
                default: break;
            }
            ++ch;
        }
        // Mask missing or shorter than channel count: treat the first two channels as L/R.
        if (ch < f.channels && f.channels >= 2 && ch < 2) {
            std::fill(gl.begin(), gl.end(), 0.0f);
            std::fill(gr.begin(), gr.end(), 0.0f);
            gl[0] = 1.0f;
            gr[1] = 1.0f;
        }
    }
};

ComPtr<IMMDeviceEnumerator> enumerator() {
    ComPtr<IMMDeviceEnumerator> e;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
    return e;
}

std::string friendlyName(IMMDevice* dev) {
    ComPtr<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return {};
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string out;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) out = logx::narrow(v.pwszVal);
    PropVariantClear(&v);
    return out;
}

// PKEY_AudioEndpoint_FormFactor, spelled out: mmdeviceapi.h only declares it and no SDK import library defines it.
const PROPERTYKEY kFormFactorKey = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};

// Bus an endpoint's audio device hangs off, as far as connection classification cares.
enum class Bus { Unresolved, Bluetooth, BluetoothHandsFree, Usb, Internal, Virtual };

std::string asciiLower(std::string s) {
    for (auto& ch : s)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return s;
}

// Walks from a device node towards the root; the first enumerator that names a bus decides (a Bluetooth device
// sits below its USB/PCI radio, so Bluetooth must win at the lower level).
Bus busOf(std::wstring instanceId) {
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, instanceId.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return Bus::Unresolved;
    wchar_t id[MAX_DEVICE_ID_LEN];
    for (int depth = 0; depth < 8; ++depth) {
        if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) break;
        const std::wstring_view full(id);
        std::wstring en(full.substr(0, full.find(L'\\')));
        for (auto& ch : en)
            if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - L'a' + L'A');
        if (en == L"BTHHFENUM") return Bus::BluetoothHandsFree;
        if (en.starts_with(L"BTH") || en.find(L"BLUETOOTH") != std::wstring::npos) return Bus::Bluetooth;
        if (en == L"USB") return Bus::Usb;
        if (en == L"HDAUDIO" || en == L"PCI" || en == L"INTELAUDIO" || en == L"ACPI") return Bus::Internal;
        if (en == L"ROOT") return Bus::Virtual;
        if (CM_Get_Parent(&node, node, 0) != CR_SUCCESS) break;
    }
    return Bus::Unresolved;
}

// Instance id of the adapter KS filter behind the endpoint's topology connector:
// "{2}.\\?\hdaudio#func_01&ven_...#4&...&0001#{6994ad04-...}\ehpspeakertopo" -> "hdaudio\func_01&ven_...\4&...&0001".
std::wstring topologyInstanceId(IMMDevice* dev) {
    ComPtr<IDeviceTopology> topology;
    ComPtr<IConnector> connector;
    LPWSTR raw = nullptr;
    if (FAILED(dev->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, &topology)) ||
        FAILED(topology->GetConnector(0, &connector)) || FAILED(connector->GetDeviceIdConnectedTo(&raw)) || !raw)
        return {};
    const std::wstring path = raw;
    CoTaskMemFree(raw);
    const size_t begin = path.find(L"\\\\?\\");
    const size_t end = path.rfind(L"#{");  // last: Bluetooth service ids contain "#{" themselves
    if (begin == std::wstring::npos || end == std::wstring::npos || end <= begin + 4) return {};
    std::wstring id = path.substr(begin + 4, end - begin - 4);
    std::replace(id.begin(), id.end(), L'#', L'\\');
    return id;
}

}  // namespace

uint32_t windowsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    static const uint32_t build = [] {
        RTL_OSVERSIONINFOW vi{};
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
            fn(&vi);
        return static_cast<uint32_t>(vi.dwBuildNumber);
    }();
    return build;
}

std::vector<DeviceInfo> listRenderDevices() {
    std::vector<DeviceInfo> out;
    auto e = enumerator();
    if (!e) return out;
    const std::wstring def = defaultRenderDeviceId();
    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) return out;
    UINT n = 0;
    coll->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev))) continue;
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id))) continue;
        DeviceInfo d;
        d.id = id;
        CoTaskMemFree(id);
        d.name = friendlyName(dev.Get());
        d.isDefault = d.id == def;
        out.push_back(std::move(d));
    }
    return out;
}

std::wstring defaultRenderDeviceId() {
    auto e = enumerator();
    if (!e) return {};
    ComPtr<IMMDevice> dev;
    if (FAILED(e->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return {};
    LPWSTR id = nullptr;
    if (FAILED(dev->GetId(&id))) return {};
    std::wstring out = id;
    CoTaskMemFree(id);
    return out;
}

std::string deviceName(const std::wstring& id) {
    auto e = enumerator();
    ComPtr<IMMDevice> dev;
    if (!e || FAILED(e->GetDevice(id.c_str(), &dev))) return {};
    return friendlyName(dev.Get());
}

EndpointTraits endpointTraits(const std::wstring& id) {
    EndpointTraits t;
    try {
        auto e = enumerator();
        ComPtr<IMMDevice> dev;
        if (!e || FAILED(e->GetDevice(id.c_str(), &dev))) return t;
        const std::string name = asciiLower(friendlyName(dev.Get()));

        ComPtr<IPropertyStore> props;
        ULONG formFactor = UnknownFormFactor;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(kFormFactorKey, &v)) && v.vt == VT_UI4) formFactor = v.ulVal;
            PropVariantClear(&v);
        }
        t.headphones = formFactor == static_cast<ULONG>(Headphones) || formFactor == static_cast<ULONG>(Headset);

        ComPtr<IAudioClient> client;
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) &&
            SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
            t.sampleRate = mix->nSamplesPerSec;
            t.channels = mix->nChannels;
            CoTaskMemFree(mix);
        }

        // The endpoint is a software device (SWD\MMDEVAPI\<id>) parented to its audio adapter; if that walk is
        // inconclusive, the adapter filter behind the topology connector names the bus instead.
        Bus bus = busOf(L"SWD\\MMDEVAPI\\" + id);
        if (bus == Bus::Unresolved)
            if (std::wstring adapter = topologyInstanceId(dev.Get()); !adapter.empty()) bus = busOf(std::move(adapter));
        const bool hfName = name.find("hands-free") != std::string::npos;
        switch (bus) {
        case Bus::Bluetooth:
        case Bus::BluetoothHandsFree: t.connection = Connection::Bluetooth; break;
        case Bus::Usb: t.connection = Connection::Usb; break;
        case Bus::Internal: {
            // Onboard codecs drive analog jacks ("Speakers" is often the headphone jack too); their digital
            // outputs (S/PDIF, HDMI/DisplayPort audio) are not headphones.
            const bool digital = formFactor == static_cast<ULONG>(SPDIF) ||
                                 formFactor == static_cast<ULONG>(DigitalAudioDisplayDevice) ||
                                 formFactor == static_cast<ULONG>(UnknownDigitalPassthrough);
            t.connection = digital ? Connection::Unknown : Connection::Wired;
            break;
        }
        case Bus::Virtual: break;
        case Bus::Unresolved:
            if (hfName || name.find("bluetooth") != std::string::npos) t.connection = Connection::Bluetooth;
            break;
        }
        // Hands-free (HFP/HSP) carries mono narrow/wide-band voice; binaural rendering can't survive it.
        t.handsFree = t.connection == Connection::Bluetooth &&
                      (bus == Bus::BluetoothHandsFree || hfName ||
                       (t.channels == 1 && t.sampleRate > 0 && t.sampleRate <= 16000));

        // Most specific first: "AirPods Pro" must not match as "AirPods".
        static constexpr const char* kModels[] = {"AirPods Pro", "AirPods Max", "AirPods",   "Beats", "WH-1000XM",
                                                  "WF-1000XM",   "LinkBuds",    "Galaxy Buds", "Bose"};
        for (const char* model : kModels) {
            if (name.find(asciiLower(model)) == std::string::npos) continue;
            t.model = model;
            break;
        }
    } catch (...) {
        // Allocation failure only; return what was learned.
    }
    return t;
}

// ---------------------------------------------------------------------------------------------
// Capture

CaptureStream::~CaptureStream() { stop(); }

bool CaptureStream::start(const Config& cfg, Sink sink) {
    stop();
    cfg_ = cfg;
    sink_ = std::move(sink);
    error_.clear();
    failed_ = false;
    stop_ = false;
    startOk_ = false;
    startedEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    running_ = true;
    thread_ = std::thread(&CaptureStream::threadMain, this);
    WaitForSingleObject(startedEvent_, 10000);
    CloseHandle(startedEvent_);
    startedEvent_ = nullptr;
    if (!startOk_) stop();
    return startOk_;
}

void CaptureStream::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

void CaptureStream::threadMain() {
    ComScope com;
    auto fail = [&](const std::string& what, HRESULT hr) {
        error_ = what + ": " + logx::hr(hr);
        logx::error("capture " + error_);
        failed_ = true;
        running_ = false;
        if (startedEvent_) SetEvent(startedEvent_);
    };

    auto e = enumerator();
    ComPtr<IMMDevice> dev;
    HRESULT hr = e ? e->GetDevice(cfg_.deviceId.c_str(), &dev) : E_FAIL;
    if (FAILED(hr)) return fail("open device", hr);
    ComPtr<IAudioClient> client;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) return fail("activate", hr);
    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) return fail("mix format", hr);
    format_ = toStreamFormat(mix);
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            kHnsPerSec / 50, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) return fail("initialize", hr);

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = client->SetEventHandle(ev);
    if (FAILED(hr)) {
        CloseHandle(ev);
        return fail("event handle", hr);
    }
    ComPtr<IAudioCaptureClient> cap;
    hr = client->GetService(IID_PPV_ARGS(&cap));
    if (FAILED(hr)) {
        CloseHandle(ev);
        return fail("capture service", hr);
    }
    hr = client->Start();
    if (FAILED(hr)) {
        CloseHandle(ev);
        return fail("start", hr);
    }

    logx::info("capture started: " + std::to_string(format_.sampleRate) + " Hz, " + std::to_string(format_.channels) +
               " ch, " + std::to_string(format_.bits) + (format_.isFloat ? "-bit float" : "-bit int"));
    startOk_ = true;
    SetEvent(startedEvent_);

    MmcssScope mmcss;
    const Downmix dm(format_);
    const size_t bytesPerSample = format_.bits / 8;
    std::vector<float> stereo;

    while (!stop_) {
        // Endpoint loopback signals nothing while the endpoint is silent; the timeout keeps us responsive.
        WaitForSingleObject(ev, 20);
        for (;;) {
            UINT32 packet = 0;
            hr = cap->GetNextPacketSize(&packet);
            if (FAILED(hr) || packet == 0) break;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD bufFlags = 0;
            hr = cap->GetBuffer(&data, &frames, &bufFlags, nullptr, nullptr);
            if (FAILED(hr)) break;
            stereo.resize(static_cast<size_t>(frames) * 2);
            if (bufFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::fill(stereo.begin(), stereo.end(), 0.0f);
            } else {
                const BYTE* p = data;
                for (UINT32 i = 0; i < frames; ++i) {
                    float l = 0.0f, r = 0.0f;
                    for (uint16_t c = 0; c < format_.channels; ++c, p += bytesPerSample) {
                        const float s = sampleAt(p, format_);
                        l += s * dm.gl[c];
                        r += s * dm.gr[c];
                    }
                    stereo[2 * i] = l;
                    stereo[2 * i + 1] = r;
                }
            }
            cap->ReleaseBuffer(frames);
            sink_(stereo.data(), frames);
        }
        if (FAILED(hr)) {
            error_ = "capture: " + logx::hr(hr);
            logx::error(error_);
            failed_ = true;
            break;
        }
    }
    client->Stop();
    CloseHandle(ev);
    running_ = false;
}

// ---------------------------------------------------------------------------------------------
// Render

RenderStream::~RenderStream() { stop(); }

bool RenderStream::start(const std::wstring& deviceId, Source source) {
    stop();
    deviceId_ = deviceId;
    source_ = std::move(source);
    error_.clear();
    failed_ = false;
    stop_ = false;
    startOk_ = false;
    startedEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    running_ = true;
    thread_ = std::thread(&RenderStream::threadMain, this);
    WaitForSingleObject(startedEvent_, 10000);
    CloseHandle(startedEvent_);
    startedEvent_ = nullptr;
    if (!startOk_) stop();
    return startOk_;
}

void RenderStream::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

void RenderStream::threadMain() {
    ComScope com;
    auto fail = [&](const std::string& what, HRESULT hr) {
        error_ = what + ": " + logx::hr(hr);
        logx::error("render " + error_);
        failed_ = true;
        running_ = false;
        if (startedEvent_) SetEvent(startedEvent_);
    };

    auto e = enumerator();
    ComPtr<IMMDevice> dev;
    HRESULT hr = e ? e->GetDevice(deviceId_.c_str(), &dev) : E_FAIL;
    if (FAILED(hr)) return fail("open device", hr);
    ComPtr<IAudioClient> client;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) return fail("activate", hr);
    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) return fail("mix format", hr);
    format_ = toStreamFormat(mix);
    // 10 ms requested buffer; shared mode rounds up to the engine period.
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kHnsPerSec / 100, 0, mix,
                            nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) return fail("initialize", hr);

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    client->SetEventHandle(ev);
    ComPtr<IAudioRenderClient> rc;
    hr = client->GetService(IID_PPV_ARGS(&rc));
    if (FAILED(hr)) {
        CloseHandle(ev);
        return fail("render service", hr);
    }
    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);
    REFERENCE_TIME streamLatency = 0;
    client->GetStreamLatency(&streamLatency);
    latency_ = static_cast<double>(streamLatency) / kHnsPerSec + static_cast<double>(bufferFrames) / format_.sampleRate;

    // Pre-fill with silence so the first period doesn't glitch.
    BYTE* data = nullptr;
    if (SUCCEEDED(rc->GetBuffer(bufferFrames, &data))) rc->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);

    hr = client->Start();
    if (FAILED(hr)) {
        CloseHandle(ev);
        return fail("start", hr);
    }
    logx::info("render started: " + std::to_string(format_.sampleRate) + " Hz, " + std::to_string(format_.channels) +
               " ch, buffer " + std::to_string(bufferFrames) + " frames");
    startOk_ = true;
    SetEvent(startedEvent_);

    MmcssScope mmcss;
    const size_t bytesPerSample = format_.bits / 8;
    std::vector<float> stereo(static_cast<size_t>(bufferFrames) * 2);

    while (!stop_) {
        if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
        UINT32 padding = 0;
        hr = client->GetCurrentPadding(&padding);
        if (FAILED(hr)) break;
        const UINT32 frames = bufferFrames - padding;
        if (frames == 0) continue;
        hr = rc->GetBuffer(frames, &data);
        if (FAILED(hr)) break;
        source_(stereo.data(), frames);
        BYTE* p = data;
        for (UINT32 i = 0; i < frames; ++i) {
            const float l = stereo[2 * i], r = stereo[2 * i + 1];
            if (format_.channels == 1) {
                storeSample(p, format_, 0.5f * (l + r));
                p += bytesPerSample;
                continue;
            }
            for (uint16_t c = 0; c < format_.channels; ++c, p += bytesPerSample)
                storeSample(p, format_, c == 0 ? l : c == 1 ? r : 0.0f);
        }
        hr = rc->ReleaseBuffer(frames, 0);
        if (FAILED(hr)) break;
    }
    if (FAILED(hr)) {
        error_ = "render: " + logx::hr(hr);
        logx::error(error_);
        failed_ = true;
    }
    client->Stop();
    CloseHandle(ev);
    running_ = false;
}

}  // namespace audio
