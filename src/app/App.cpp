#include "app/App.h"

#include "app/VideoDetector.h"
#include "audio/Routing.h"
#include "audio/Wasapi.h"
#include "ui/Theme.h"
#include "util/Log.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <winsock2.h>
#include <iphlpapi.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <iterator>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace app {
namespace {

constexpr int kHotkeySpatial = 1;
constexpr int kHotkeyPower = 2;
constexpr int kHotkeyRecenter = 3;
constexpr int kHotkeyQuickMenu = 4;
constexpr int kHotkeyLayoutBase = 100;  // + layout index
constexpr UINT_PTR kTimerTransition = 1;
constexpr UINT_PTR kTimerQuickMenu = 2;
constexpr float kTransitionMs = 350.0f;
constexpr auto kFrameInterval = std::chrono::microseconds(16667);  // ~60 fps
constexpr auto kSaveDebounce = std::chrono::seconds(1);
constexpr auto kDeviceRefresh = std::chrono::seconds(3);
constexpr DWORD kHiddenWaitMs = 250;  // status/tray polling while no frames are drawn

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool sameConfig(const engine::Config& a, const engine::Config& b) {
    return a.headphoneDeviceId == b.headphoneDeviceId && a.sinkDeviceId == b.sinkDeviceId && a.capture == b.capture &&
           a.mode == b.mode && a.modelPath == b.modelPath && a.lookaheadSec == b.lookaheadSec && a.hopSec == b.hopSec &&
           a.adaptiveGpuFallback == b.adaptiveGpuFallback;
}

bool listed(const std::vector<audio::DeviceInfo>& devices, const std::wstring& id) {
    return std::any_of(devices.begin(), devices.end(), [&](const audio::DeviceInfo& d) { return d.id == id; });
}

// A device nobody listens to: virtual cables first, then digital/display outputs that are usually unconnected.
std::wstring pickSilentSink(const std::vector<audio::DeviceInfo>& devices, const std::wstring& headphones,
                            const std::wstring& systemDefault) {
    static const std::vector<std::vector<const char*>> tiers = {
        {"cable input", "virtual"},
        {"digital audio (s/pdif)"},
        {"nvidia high definition audio", "amd high definition audio", "intel(r) display audio", "hdmi", "displayport"},
    };
    for (const auto& tier : tiers) {
        for (const audio::DeviceInfo& d : devices) {
            if (d.id == headphones || d.id == systemDefault) continue;
            const std::string name = lower(d.name);
            for (const char* needle : tier) {
                if (name.find(needle) != std::string::npos) return d.id;
            }
        }
    }
    return {};
}

std::vector<uint32_t> resolvePids(const std::vector<std::string>& exes) {
    std::vector<uint32_t> pids;
    const DWORD self = GetCurrentProcessId();
    for (const audio::AppSession& s : audio::listAppSessions()) {
        if (s.pid == self) continue;
        if (std::find(exes.begin(), exes.end(), lower(s.exe)) != exes.end()) pids.push_back(s.pid);
    }
    return pids;
}

std::wstring exeDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return L".";
        if (n < path.size()) {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);
    }
    return std::filesystem::path(path).parent_path().wstring();
}

// This PC's IPv4 addresses on connected adapters, for the "send UDP here" hint.
std::vector<std::string> localIPv4Addresses() {
    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    std::vector<unsigned char> buf;
    ULONG size = 16 * 1024;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    // The table can grow between the size query and the read; a few retries cover that.
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()),
                                  &size);
    }
    std::vector<std::string> out;
    if (rc != NO_ERROR) return out;
    for (auto* ad = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buf.data()); ad; ad = ad->Next) {
        if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (const IP_ADAPTER_UNICAST_ADDRESS* ua = ad->FirstUnicastAddress; ua; ua = ua->Next) {
            const sockaddr* sa = ua->Address.lpSockaddr;
            if (!sa || sa->sa_family != AF_INET) continue;
            const auto* b =
                reinterpret_cast<const unsigned char*>(&reinterpret_cast<const sockaddr_in*>(sa)->sin_addr);
            if (b[0] == 169 && b[1] == 254) continue;  // link-local (no DHCP): a phone cannot reach it
            char text[16];
            std::snprintf(text, sizeof(text), "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
            out.emplace_back(text);
        }
    }
    return out;
}

// New headphones: Bluetooth adds ~100+ ms of output delay that prediction hides, and headphones with their own
// head tracker (Bluetooth HID) are the obvious source.
TrackingProfile defaultProfile(const audio::EndpointTraits& t,
                               const std::vector<track::HidHeadSensor::Info>& sensors) {
    TrackingProfile p;
    const bool bluetooth = t.connection == audio::Connection::Bluetooth;
    p.predictionMs = bluetooth ? 100.0f : 0.0f;
    if (bluetooth && !sensors.empty()) {
        p.source = track::Source::HeadphoneSensor;
        p.sensorPath = sensors.front().path;
    }
    return p;
}

}  // namespace

App::~App() { shutdown(); }

bool App::init(HINSTANCE instance, int showCmd) {
    instance_ = instance;
    exeDir_ = exeDirectory();
    modelsDir_ = exeDir_ + L"\\models";

    const bool existed = settings_.load();
    for (std::string& exe : settings_.appExes) exe = lower(exe);

    view_.windowsBuild = audio::windowsBuild();
    view_.perAppRouting = audio::perAppRoutingSupported();
    view_.modelsDir = modelsDir_;
    scanModels();
    refreshDevices();
    refreshSessions();
    autoPickDevices();
    syncHeadphones();
    view_.stemNames = ui::defaultStemNames(settings_.modelFile);
    lastModelFile_ = settings_.modelFile;
    if (!existed) {
        settings_.scene = presetScene(Preset::Stage, view_.stemNames, settings_.scene);
        settings_.activeLayout = "Stage";
        markDirty();
    }
    layouts_.load();
    view_.layouts = &layouts_;
    logx::info("Windows build " + std::to_string(view_.windowsBuild) +
               ", per-app routing " + (view_.perAppRouting ? "available" : "unavailable"));

    ctlScene_ = settings_.scene;
    ctlEnabled_ = settings_.enabled;
    engine_.setScene(settings_.scene);
    engine_.setEnabled(settings_.enabled);
    engine_.setHeadTracker(&tracker_);  // tracking runs on its own, independent of engine power

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = &App::wndProcThunk;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        logx::error("RegisterClassExW failed: " + logx::hr(HRESULT_FROM_WIN32(GetLastError())));
        return false;
    }

    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const float primaryScale = ImGui_ImplWin32_GetDpiScaleForMonitor(primary);
    hwnd_ = CreateWindowExW(0, kWindowClass, L"Spatiall", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            static_cast<int>(1180.0f * primaryScale), static_cast<int>(820.0f * primaryScale), nullptr,
                            nullptr, instance, this);
    if (!hwnd_) {
        logx::error("CreateWindowExW failed: " + logx::hr(HRESULT_FROM_WIN32(GetLastError())));
        return false;
    }
    dpiScale_ = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd_);

    // Dark title bar to match the UI (Windows 10 20H1+ / 11; ignored elsewhere).
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    activateMsg_ = RegisterWindowMessageW(kActivateMessage);
    if (activateMsg_) ChangeWindowMessageFilterEx(hwnd_, activateMsg_, MSGFLT_ALLOW, nullptr);

    if (!createDevice()) {
        MessageBoxW(nullptr, L"Spatiall could not initialise Direct3D 11.", L"Spatiall", MB_ICONERROR | MB_OK);
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // single fixed panel; nothing worth persisting
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ui::applyTheme(ImGui::GetStyle(), dpiScale_);
    view_.dpiScale = dpiScale_;
    // Sizes come from style.FontSizeBase * FontScaleDpi (dynamic fonts), so nothing is baked per DPI.
    ui::loadFonts(io.Fonts, exeDir_);
    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_.Get(), context_.Get());
    imguiReady_ = true;

    view_.quickMenuAvailable = quickMenu_.create(instance_, device_.Get(), exeDir_);
    tray_.create(hwnd_);
    registerHotkeys();

    control_ = std::thread(&App::controlLoop, this);

    ShowWindow(hwnd_, showCmd);
    UpdateWindow(hwnd_);

    if (settings_.autostartPower) {
        powerWanted_ = true;
        reconcile(false);
    }
    updateTray();
    return true;
}

int App::run() {
    Clock::time_point nextFrame = Clock::now();
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                shutdown();
                return static_cast<int>(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (shutDown_) continue;

        const bool visible = IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
        tick(visible);

        DWORD waitMs = kHiddenWaitMs;
        if (visible) {
            const Clock::time_point now = Clock::now();
            if (now >= nextFrame) {
                if (occluded_ && swapChain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
                    nextFrame = now + std::chrono::milliseconds(100);  // screen locked / fully covered
                } else {
                    occluded_ = false;
                    nextFrame = now + kFrameInterval;
                    renderFrame();
                }
            }
            const auto remaining =
                std::chrono::ceil<std::chrono::milliseconds>(nextFrame - Clock::now()).count();
            waitMs = static_cast<DWORD>(std::clamp<long long>(remaining, 0, kHiddenWaitMs));
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Window / D3D

LRESULT CALLBACK App::wndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->wndProc(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // While capturing a hotkey the key presses belong to the capture, not to the UI.
    if (captureTarget_ != ui::kCaptureNone && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
        if (onCaptureKey(static_cast<UINT>(wp))) return 0;
    }
    if (imguiReady_ && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return TRUE;

    if (activateMsg_ && msg == activateMsg_) {
        showWindow();
        return 0;
    }
    if (tray_.taskbarCreatedMessage() && msg == tray_.taskbarCreatedMessage()) {
        tray_.readd();
        return 0;
    }
    if (msg == Tray::kCallbackMessage) {
        switch (LOWORD(lp)) {
            case NIN_SELECT:
            case NIN_KEYSELECT:
                showWindow();
                break;
            case WM_CONTEXTMENU: {
                // NOTIFYICON_VERSION_4 passes the anchor point in wParam.
                const POINT anchor{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)};
                Tray::MenuState ms;
                ms.spatialOn = settings_.enabled;
                ms.powerOn = powerWanted_;
                ms.trackingOn = settings_.tracking.source != track::Source::None;
                for (const Layout& l : layouts_.items) ms.layouts.push_back(l.name);
                ms.activeLayout = layouts_.find(settings_.activeLayout);
                onTrayCommand(tray_.showMenu(anchor, ms));
                break;
            }
            default:
                break;
        }
        return 0;
    }

    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                resizeW_ = LOWORD(lp);  // applied before the next frame, not inside the size loop
                resizeH_ = HIWORD(lp);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = static_cast<LONG>(820.0f * dpiScale_);
            mmi->ptMinTrackSize.y = static_cast<LONG>(600.0f * dpiScale_);
            return 0;
        }
        case WM_DPICHANGED: {
            dpiScale_ = static_cast<float>(HIWORD(wp)) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
            if (imguiReady_) {
                ui::applyTheme(ImGui::GetStyle(), dpiScale_);
                view_.dpiScale = dpiScale_;
            }
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_HOTKEY: {
            const int id = static_cast<int>(wp);
            if (id == kHotkeySpatial) toggleSpatial();
            if (id == kHotkeyPower) togglePower();
            if (id == kHotkeyRecenter) tracker_.recenter();
            if (id == kHotkeyQuickMenu) openQuickMenu();
            if (id >= kHotkeyLayoutBase) applyLayout(id - kHotkeyLayoutBase);
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimerTransition) stepTransition();
            if (wp == kTimerQuickMenu) {
                const int chosen = quickMenu_.frame();
                if (chosen >= 0) applyLayout(chosen);
                if (!quickMenu_.isOpen()) KillTimer(hwnd_, kTimerQuickMenu);
            }
            return 0;
        case WM_CLOSE:
            hideWindow();  // the window closes to the tray; Exit lives in the tray menu and the footer
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // no Alt menu
            break;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) onEndSession();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool App::createDevice() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd_;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                               static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &sd,
                                               &swapChain_, &device_, &level, &context_);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels,
                                           static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &sd, &swapChain_,
                                           &device_, &level, &context_);
    }
    if (FAILED(hr)) {
        logx::error("D3D11CreateDeviceAndSwapChain failed: " + logx::hr(hr));
        return false;
    }
    createRenderTarget();
    return rtv_ != nullptr;
}

void App::destroyDevice() {
    rtv_.Reset();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
}

void App::createRenderTarget() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_);
    }
}

void App::renderFrame() {
    if (resizeW_ != 0 && resizeH_ != 0) {
        rtv_.Reset();
        swapChain_->ResizeBuffers(0, resizeW_, resizeH_, DXGI_FORMAT_UNKNOWN, 0);
        resizeW_ = resizeH_ = 0;
        createRenderTarget();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    const ui::Actions actions = ui_.draw(settings_, view_);
    ImGui::Render();

    const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(ui::col::kBg);
    const float clear[4] = {bg.x, bg.y, bg.z, 1.0f};
    ID3D11RenderTargetView* rtv = rtv_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->ClearRenderTargetView(rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    const HRESULT hr = swapChain_->Present(1, 0);
    occluded_ = hr == DXGI_STATUS_OCCLUDED;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        // Driver update / TDR: rebuild the device and the backend's GPU objects.
        logx::warn("Display device lost (" + logx::hr(hr) + "), recreating");
        ImGui_ImplDX11_Shutdown();
        destroyDevice();
        if (!createDevice()) {
            logx::error("Direct3D could not be recreated; exiting");
            imguiReady_ = false;  // the DX11 backend is already shut down
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            exitApp();
            return;
        }
        ImGui_ImplDX11_Init(device_.Get(), context_.Get());
    }

    // Acted on after the frame so blocking work (shell, menus) never happens mid-frame.
    handle(actions);
}

void App::shutdown() {
    if (shutDown_) return;
    shutDown_ = true;
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);

    if (control_.joinable()) {
        {
            std::lock_guard lk(ctlMu_);
            ctlQuit_ = true;
        }
        ctlCv_.notify_one();
        control_.join();  // its last act is Engine::stop(), which restores routing
    }
    engine_.setHeadTracker(nullptr);
    tracker_.stop();
    audio::RoutingJournal::instance().restoreAll();
    if (dirty_) saveNow();

    if (hwnd_) {
        KillTimer(hwnd_, kTimerTransition);
        KillTimer(hwnd_, kTimerQuickMenu);
        unregisterHotkeys();
    }
    quickMenu_.destroy();  // uses the D3D device; must go before destroyDevice()
    tray_.destroy();
    if (imguiReady_) {
        imguiReady_ = false;
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    destroyDevice();
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (instance_) UnregisterClassW(kWindowClass, instance_);
}

// ---------------------------------------------------------------------------------------------------------------
// App state

void App::tick(bool visible) {
    view_.status = engine_.status();
    view_.busy = busy_.load();
    view_.powerWanted = powerWanted_;
    if (view_.status.running && !view_.busy && !view_.status.stemNames.empty()) {
        setStemNames(view_.status.stemNames);
    }
    view_.tracker = tracker_.status();
    view_.activeLayout = layouts_.find(settings_.activeLayout);
    view_.capturingHotkey = captureTarget_;
    if (view_.status.running && view_.status.headTracked) {
        view_.headTracked = true;  // the smoothed orientation the engine actually renders with
        view_.head = {view_.status.headYawDeg, view_.status.headPitchDeg, view_.status.headRollDeg};
    } else {
        view_.headTracked = tracker_.current(view_.head);
    }

    if (!validationError_.empty()) {
        view_.error = validationError_;
    } else {
        std::lock_guard lk(ctlMu_);
        view_.error = ctlError_;
    }
    if (view_.error.empty() && powerWanted_ && !view_.busy && !view_.status.running) view_.error = view_.status.error;

    const Clock::time_point now = Clock::now();
    // Adaptive mode: games and videos want low latency more than separation. Fullscreen (exclusive D3D or
    // borderless) comes from the shell's notification state; windowed video from what is producing sound.
    if (now - lastFullscreenCheck_ >= std::chrono::seconds(1)) {
        lastFullscreenCheck_ = now;
        QUERY_USER_NOTIFICATION_STATE state{};
        const bool fullscreen = SUCCEEDED(SHQueryUserNotificationState(&state)) &&
                                (state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_BUSY);
        // Only Adaptive acts on this; elsewhere skip the work and the log lines.
        const bool adaptive = settings_.mode == engine::Mode::Adaptive && view_.status.running;
        std::string reason;
        if (adaptive && settings_.adaptiveFullscreen && fullscreen) {
            reason = "fullscreen app";
        } else if (adaptive && settings_.adaptiveVideo) {
            reason = detectVideo(audio::listAppSessions(), settings_.videoMarkers);
        }
        if (reason != view_.lowLatencyReason) {
            logx::info(reason.empty() ? "Low latency no longer needed" : "Low latency wanted: " + reason);
        }
        view_.fullscreenApp = fullscreen;
        view_.lowLatencyReason = reason;
        engine_.setLowLatencyHint(!reason.empty(), reason);
    }
    if (visible && now - lastDeviceRefresh_ >= kDeviceRefresh) refreshDevices();
    if (dirty_ && now - dirtyAt_ >= kSaveDebounce) saveNow();
    updateTray();
}

void App::updateTray() {
    Tray::State state;
    if (view_.busy) {
        state = Tray::State::Starting;
    } else if (powerWanted_ && !view_.error.empty()) {
        state = Tray::State::Error;
    } else if (view_.status.running) {
        state = settings_.enabled ? Tray::State::Spatial : Tray::State::Stereo;
    } else {
        state = Tray::State::Off;
    }
    if (state == trayState_ && settings_.enabled == trayEnabled_ && view_.error == trayError_) return;
    trayState_ = state;
    trayEnabled_ = settings_.enabled;
    trayError_ = view_.error;

    std::wstring status;
    switch (state) {
        case Tray::State::Starting: status = powerWanted_ ? L"starting…" : L"stopping…"; break;
        case Tray::State::Error: status = L"error: " + logx::widen(view_.error); break;
        case Tray::State::Spatial: status = L"spatial audio on"; break;
        case Tray::State::Stereo: status = L"on, spatial off (stereo)"; break;
        case Tray::State::Off:
        case Tray::State::Count: status = L"off"; break;
    }
    tray_.update(state, L"Spatiall — " + status);
    if (hwnd_) SetWindowTextW(hwnd_, (L"Spatiall — " + status).c_str());
}

void App::showWindow() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
    refreshDevices();
    refreshSessions();
}

void App::hideWindow() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void App::exitApp() {
    logx::info("Exit requested");
    shutdown();  // destroys the window -> WM_DESTROY -> WM_QUIT ends run()
}

void App::onTrayCommand(UINT cmd) {
    if (cmd >= Tray::kCmdLayoutBase) {
        applyLayout(static_cast<int>(cmd - Tray::kCmdLayoutBase));
        return;
    }
    switch (cmd) {
        case Tray::CmdToggleSpatial: toggleSpatial(); break;
        case Tray::CmdTogglePower: togglePower(); break;
        case Tray::CmdShow: showWindow(); break;
        case Tray::CmdExit: exitApp(); break;
        case Tray::CmdRecenter: tracker_.recenter(); break;
        default: break;
    }
}

void App::onEndSession() {
    // The process may be killed as soon as this returns: undo routing first, without waiting for the engine.
    logx::info("Windows session ending");
    audio::RoutingJournal::instance().restoreAll();
    saveNow();
}

void App::togglePower() {
    powerWanted_ = !powerWanted_;
    logx::info(powerWanted_ ? "Power on requested" : "Power off requested");
    if (powerWanted_) {
        refreshDevices();
        refreshSessions();
    }
    reconcile(false);
    updateTray();
}

void App::toggleSpatial() {
    settings_.enabled = !settings_.enabled;
    engine_.setEnabled(settings_.enabled);
    {
        std::lock_guard lk(ctlMu_);
        ctlEnabled_ = settings_.enabled;
    }
    markDirty();
    updateTray();
}

void App::pushScene() {
    engine_.setScene(settings_.scene);
    std::lock_guard lk(ctlMu_);
    ctlScene_ = settings_.scene;
}

void App::handle(const ui::Actions& a) {
    if (a.togglePower) togglePower();
    if (a.toggleSpatial) toggleSpatial();
    if (a.refreshDevices) refreshDevices();
    if (a.refreshApps) refreshSessions();
    if (a.headphonesChanged) {
        if (settings_.sinkDeviceId == settings_.headphoneDeviceId) settings_.sinkDeviceId.clear();
        autoPickDevices();
        syncHeadphones();
    }
    if (a.trackingChanged) restartTracker();
    if (a.recenter) tracker_.recenter();
    if (a.sceneChanged) {
        // A manual edit wins over a running layout animation.
        if (transitioning_) {
            KillTimer(hwnd_, kTimerTransition);
            transitioning_ = false;
        }
        pushScene();
    }
    handleLayoutActions(a);
    if (a.configChanged) {
        if (settings_.modelFile != lastModelFile_) {
            lastModelFile_ = settings_.modelFile;
            setStemNames(ui::defaultStemNames(settings_.modelFile));
        }
        if (powerWanted_) reconcile(false);
    }
    if (a.retry) {
        powerWanted_ = true;
        refreshDevices();
        refreshSessions();
        reconcile(true);
    }
    if (a.settingsChanged) markDirty();
    if (a.openLogFolder) {
        ShellExecuteW(hwnd_, L"open", Settings::directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (a.hide) hideWindow();
    if (a.exit) exitApp();
}

void App::refreshDevices() {
    std::vector<audio::DeviceInfo> devices = audio::listRenderDevices();
    const auto sameId = [](const audio::DeviceInfo& x, const audio::DeviceInfo& y) { return x.id == y.id; };
    const bool changed =
        !std::equal(devices.begin(), devices.end(), view_.devices.begin(), view_.devices.end(), sameId);
    view_.devices = std::move(devices);
    view_.sensors = track::HidHeadSensor::enumerate();
    view_.localIPv4 = localIPv4Addresses();
    // A reconnect can bring the headphones back in another mode (e.g. Bluetooth hands-free).
    if (changed && !settings_.headphoneDeviceId.empty()) {
        view_.headphoneTraits = audio::endpointTraits(settings_.headphoneDeviceId);
    }
    lastDeviceRefresh_ = Clock::now();
}

void App::refreshSessions() {
    view_.sessions = audio::listAppSessions();
    const DWORD self = GetCurrentProcessId();  // our own headphone stream is not a capture candidate
    std::erase_if(view_.sessions, [&](const audio::AppSession& s) { return s.pid == self; });
}

void App::scanModels() {
    view_.models.clear();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(modelsDir_, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (_wcsicmp(entry.path().extension().c_str(), L".onnx") == 0) {
            view_.models.push_back(entry.path().filename().wstring());
        }
    }
    std::sort(view_.models.begin(), view_.models.end());
    const auto has = [&](const std::wstring& name) {
        return std::find(view_.models.begin(), view_.models.end(), name) != view_.models.end();
    };
    if (view_.models.empty() || has(settings_.modelFile)) return;
    settings_.modelFile = view_.models.front();
    for (const wchar_t* preferred : {L"htdemucs_6s_4s.onnx", L"htdemucs_4s.onnx", L"htdemucs.onnx"}) {
        if (has(preferred)) {
            settings_.modelFile = preferred;
            break;
        }
    }
    markDirty();
}

void App::setStemNames(const std::vector<std::string>& names) {
    if (names == view_.stemNames) return;
    view_.stemNames = names;
    // Only the stems move; the virtual speaker pair and scene rotation belong to the user.
    const engine::SceneParams placed = presetScene(Preset::Stage, view_.stemNames, settings_.scene);
    settings_.scene.stems = placed.stems;
    pushScene();
    markDirty();
}

void App::syncHeadphones() {
    const std::wstring& id = settings_.headphoneDeviceId;
    view_.headphoneTraits = id.empty() ? audio::EndpointTraits{} : audio::endpointTraits(id);
    if (profileLoaded_ && settings_.trackingDeviceId == id) return;
    settings_.saveTracking();  // the previous headphones keep their profile
    if (!settings_.loadTracking(id)) {
        settings_.tracking = defaultProfile(view_.headphoneTraits, view_.sensors);
        settings_.trackingDeviceId = id;
        markDirty();
    }
    profileLoaded_ = true;
    restartTracker();
}

void App::restartTracker() {
    const TrackingProfile& p = settings_.tracking;
    track::HeadTracker::Config cfg;
    cfg.source = p.source;  // None stops tracking
    cfg.udpPort = p.udpPort;
    cfg.sensorPath = p.sensorPath;
    cfg.invertYaw = p.invertYaw;
    cfg.invertPitch = p.invertPitch;
    cfg.invertRoll = p.invertRoll;
    cfg.predictionMs = p.predictionMs;
    if (!tracker_.start(cfg) && cfg.source != track::Source::None) {
        logx::warn("Head tracking could not start: " + tracker_.status().error);
    }
}

void App::autoPickDevices() {
    const std::wstring systemDefault = audio::defaultRenderDeviceId();
    if (settings_.headphoneDeviceId.empty() && !systemDefault.empty()) {
        settings_.headphoneDeviceId = systemDefault;
        logx::info("Headphones: " + audio::deviceName(systemDefault) + " (system default)");
        markDirty();
    }
    if (settings_.sinkDeviceId.empty() || settings_.sinkDeviceId == settings_.headphoneDeviceId) {
        settings_.sinkDeviceId = pickSilentSink(view_.devices, settings_.headphoneDeviceId, systemDefault);
        if (!settings_.sinkDeviceId.empty()) {
            logx::info("Silent device: " + audio::deviceName(settings_.sinkDeviceId) + " (automatic)");
        } else {
            logx::warn("No suitable silent device found");
        }
        markDirty();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Layouts, hotkeys, quick menu

void App::applyLayout(int index) {
    if (index < 0 || index >= static_cast<int>(layouts_.items.size())) return;
    const Layout& l = layouts_.items[static_cast<size_t>(index)];
    logx::info("Layout: " + l.name);
    settings_.activeLayout = l.name;
    animateTo(layoutScene(l, settings_.scene, view_.stemNames));
}

void App::animateTo(const engine::SceneParams& target) {
    transitionFrom_ = settings_.scene;
    transitionTo_ = target;
    transitionStart_ = Clock::now();
    transitioning_ = true;
    // A timer, not the frame loop: switching from a hotkey or the quick menu happens with the window hidden.
    SetTimer(hwnd_, kTimerTransition, 16, nullptr);
    stepTransition();
}

void App::stepTransition() {
    if (!transitioning_) {
        KillTimer(hwnd_, kTimerTransition);
        return;
    }
    const float elapsed = std::chrono::duration<float, std::milli>(Clock::now() - transitionStart_).count();
    const float t = std::clamp(elapsed / kTransitionMs, 0.0f, 1.0f);
    const float eased = t * t * (3.0f - 2.0f * t);  // smoothstep: no jolt at either end
    settings_.scene = t >= 1.0f ? transitionTo_ : lerpScene(transitionFrom_, transitionTo_, eased);
    pushScene();
    if (t >= 1.0f) {
        transitioning_ = false;
        KillTimer(hwnd_, kTimerTransition);
        markDirty();
    }
}

void App::handleLayoutActions(const ui::Actions& a) {
    auto uniqueName = [&](std::string name) {
        const std::string base = name;
        for (int n = 2; layouts_.find(name) >= 0; ++n) name = base + " (" + std::to_string(n) + ")";
        return name;
    };
    bool layoutsChanged = false;
    if (a.applyLayout >= 0) applyLayout(a.applyLayout);
    if (a.resetScene) {
        settings_.activeLayout.clear();
        animateTo(resetScene(settings_.scene));
    }
    if (!a.saveLayoutAs.empty()) {
        const int existing = layouts_.find(a.saveLayoutAs);
        if (existing >= 0 && !layouts_.items[static_cast<size_t>(existing)].builtIn()) {
            Layout& l = layouts_.items[static_cast<size_t>(existing)];
            const Hotkey hk = l.hotkey;
            l = captureLayout(l.name, settings_.scene, view_.stemNames);
            l.hotkey = hk;
        } else {
            layouts_.items.push_back(captureLayout(uniqueName(a.saveLayoutAs), settings_.scene, view_.stemNames));
        }
        settings_.activeLayout = existing >= 0 && !layouts_.items[static_cast<size_t>(existing)].builtIn()
                                     ? a.saveLayoutAs
                                     : layouts_.items.back().name;
        layoutsChanged = true;
    }
    auto custom = [&](int i) {
        return i >= 0 && i < static_cast<int>(layouts_.items.size()) && !layouts_.items[static_cast<size_t>(i)].builtIn();
    };
    if (custom(a.updateLayout)) {
        Layout& l = layouts_.items[static_cast<size_t>(a.updateLayout)];
        const Hotkey hk = l.hotkey;
        l = captureLayout(l.name, settings_.scene, view_.stemNames);
        l.hotkey = hk;
        settings_.activeLayout = l.name;
        layoutsChanged = true;
    }
    if (custom(a.renameLayout) && !a.renameTo.empty()) {
        Layout& l = layouts_.items[static_cast<size_t>(a.renameLayout)];
        if (a.renameTo != l.name) {
            const std::string name = uniqueName(a.renameTo);
            if (settings_.activeLayout == l.name) settings_.activeLayout = name;
            l.name = name;
            layoutsChanged = true;
        }
    }
    if (custom(a.deleteLayout)) {
        if (settings_.activeLayout == layouts_.items[static_cast<size_t>(a.deleteLayout)].name) {
            settings_.activeLayout.clear();
        }
        layouts_.items.erase(layouts_.items.begin() + a.deleteLayout);
        layoutsChanged = true;
    }
    if (a.cancelCapture) endHotkeyCapture();
    if (a.captureHotkey != ui::kCaptureNone) beginHotkeyCapture(a.captureHotkey);
    if (a.clearHotkey != ui::kCaptureNone) setHotkey(a.clearHotkey, Hotkey{});

    if (layoutsChanged) {
        layouts_.save();
        registerHotkeys();  // indices may have shifted
        markDirty();
    }
}

void App::openQuickMenu() {
    if (quickMenu_.isOpen() || layouts_.items.empty()) return;
    std::vector<std::string> names;
    for (const Layout& l : layouts_.items) names.push_back(l.name);
    quickMenu_.open(std::move(names), layouts_.find(settings_.activeLayout), settings_.quickMenuHotkey);
    if (quickMenu_.isOpen()) SetTimer(hwnd_, kTimerQuickMenu, 15, nullptr);
}

void App::unregisterHotkeys() {
    for (int id : {kHotkeySpatial, kHotkeyPower, kHotkeyRecenter, kHotkeyQuickMenu}) UnregisterHotKey(hwnd_, id);
    for (size_t i = 0; i < Tray::kMaxMenuLayouts; ++i) UnregisterHotKey(hwnd_, kHotkeyLayoutBase + static_cast<int>(i));
}

void App::registerHotkeys() {
    unregisterHotkeys();
    auto reg = [&](int id, const Hotkey& hk) {
        return hk.empty() || RegisterHotKey(hwnd_, id, hk.mods | MOD_NOREPEAT, hk.vk) != FALSE;
    };
    view_.hotkeySpatialOk = reg(kHotkeySpatial, Hotkey{kModControl | kModAlt, 'S'});
    view_.hotkeyPowerOk = reg(kHotkeyPower, Hotkey{kModControl | kModAlt, 'P'});
    view_.hotkeyRecenterOk = reg(kHotkeyRecenter, Hotkey{kModControl | kModAlt, 'C'});
    view_.quickMenuHotkeyOk = reg(kHotkeyQuickMenu, settings_.quickMenuHotkey);
    view_.layoutHotkeyOk.assign(layouts_.items.size(), true);
    for (size_t i = 0; i < layouts_.items.size() && i < Tray::kMaxMenuLayouts; ++i) {
        view_.layoutHotkeyOk[i] = reg(kHotkeyLayoutBase + static_cast<int>(i), layouts_.items[i].hotkey);
        if (!view_.layoutHotkeyOk[i]) {
            logx::warn("Hotkey " + hotkeyText(layouts_.items[i].hotkey) + " for layout \"" + layouts_.items[i].name +
                       "\" is already in use");
        }
    }
    if (!view_.hotkeySpatialOk) logx::warn("Ctrl+Alt+S is registered by another application");
    if (!view_.hotkeyPowerOk) logx::warn("Ctrl+Alt+P is registered by another application");
    if (!view_.hotkeyRecenterOk) logx::warn("Ctrl+Alt+C is registered by another application");
    if (!view_.quickMenuHotkeyOk) logx::warn("Quick menu hotkey is registered by another application");
}

void App::beginHotkeyCapture(int target) {
    captureTarget_ = target;
    // Our own combos must reach the capture as plain key presses, not fire their actions.
    unregisterHotkeys();
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
}

void App::endHotkeyCapture() {
    if (captureTarget_ == ui::kCaptureNone) return;
    captureTarget_ = ui::kCaptureNone;
    registerHotkeys();
}

bool App::onCaptureKey(UINT vk) {
    switch (vk) {
        case VK_ESCAPE:
            endHotkeyCapture();
            return true;
        case VK_BACK: {
            const int target = captureTarget_;
            captureTarget_ = ui::kCaptureNone;
            setHotkey(target, Hotkey{});
            return true;
        }
        case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LWIN: case VK_RWIN:
        case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
            return true;  // wait for the actual key
        default:
            break;
    }
    auto down = [](int k) { return (GetKeyState(k) & 0x8000) != 0; };
    Hotkey hk;
    hk.vk = vk;
    if (down(VK_CONTROL)) hk.mods |= kModControl;
    if (down(VK_MENU)) hk.mods |= kModAlt;
    if (down(VK_SHIFT)) hk.mods |= kModShift;
    if (down(VK_LWIN) || down(VK_RWIN)) hk.mods |= kModWin;
    if (hk.mods == 0) return true;  // a bare key would hijack typing everywhere; keep waiting
    const int target = captureTarget_;
    captureTarget_ = ui::kCaptureNone;
    setHotkey(target, hk);
    return true;
}

void App::setHotkey(int target, const Hotkey& hk) {
    if (target == ui::kCaptureQuickMenu) {
        settings_.quickMenuHotkey = hk;
        markDirty();
    } else if (target >= 0 && target < static_cast<int>(layouts_.items.size())) {
        layouts_.items[static_cast<size_t>(target)].hotkey = hk;
        layouts_.save();
    }
    if (!hk.empty()) logx::info("Hotkey set: " + hotkeyText(hk));
    endHotkeyCapture();
    registerHotkeys();
}

void App::markDirty() {
    dirty_ = true;
    dirtyAt_ = Clock::now();
}

void App::saveNow() {
    settings_.save();
    dirty_ = false;
}

// ---------------------------------------------------------------------------------------------------------------
// Engine control

engine::Config App::buildConfig() const {
    engine::Config c;
    c.headphoneDeviceId = settings_.headphoneDeviceId;
    c.sinkDeviceId = settings_.sinkDeviceId;
    c.capture = settings_.capture;
    c.mode = settings_.mode;
    if (!settings_.modelFile.empty()) c.modelPath = modelsDir_ + L"\\" + settings_.modelFile;
    c.lookaheadSec = settings_.lookaheadSec;
    c.hopSec = settings_.hopSec;
    c.adaptiveGpuFallback = settings_.adaptiveGpuFallback;
    return c;
}

std::string App::validate() const {
    const Settings& s = settings_;
    if (s.headphoneDeviceId.empty()) return "Choose your headphones under Devices.";
    if (!listed(view_.devices, s.headphoneDeviceId)) {
        return "The selected headphones are not connected. Plug them in or choose another device, then press Retry.";
    }
    if (s.sinkDeviceId.empty()) {
        return "Choose a silent device under Devices (a virtual cable or an unused HDMI / S/PDIF output).";
    }
    if (s.sinkDeviceId == s.headphoneDeviceId) return "The silent device and the headphones must be different devices.";
    if (!listed(view_.devices, s.sinkDeviceId)) {
        return "The silent device is not available. Choose another one under Devices, then press Retry.";
    }
    if (s.capture == engine::CaptureMode::SelectedApps) {
        if (!view_.perAppRouting) return "Per-app routing is not available on this Windows build; use Whole system.";
        if (s.appExes.empty()) return "Check at least one app under Source.";
    }
    if (s.mode == engine::Mode::Stems) {
        const std::wstring path = modelsDir_ + L"\\" + s.modelFile;
        if (s.modelFile.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return "No separation model found in " + logx::narrow(modelsDir_) +
                   ". Put htdemucs.onnx there, or switch to Virtual speakers.";
        }
    }
    return {};
}

void App::reconcile(bool force) {
    Request r;
    r.power = powerWanted_;
    validationError_.clear();
    if (r.power) {
        validationError_ = validate();
        if (!validationError_.empty()) {
            logx::warn(validationError_);
            r.power = false;  // cannot run like this; make sure nothing stays half-started
        }
    }
    r.cfg = buildConfig();
    r.exes = settings_.appExes;

    std::lock_guard lk(ctlMu_);
    const bool unchanged = !force && r.power == desired_.power &&
                           (!r.power || (sameConfig(r.cfg, desired_.cfg) && r.exes == desired_.exes));
    if (unchanged) return;
    r.generation = desired_.generation + 1;
    desired_ = std::move(r);
    ctlError_.clear();
    busy_ = true;
    ctlCv_.notify_one();
}

void App::controlLoop() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Request r;
        engine::SceneParams scene;
        bool enabled = true;
        {
            std::unique_lock lk(ctlMu_);
            ctlCv_.wait(lk, [&] { return ctlQuit_ || desired_.generation != applied_; });
            if (ctlQuit_) break;
            r = desired_;
            applied_ = r.generation;
            scene = ctlScene_;
            enabled = ctlEnabled_;
            busy_ = true;
        }

        // Every new generation is a real change (power or restart-worthy config), so always stop first;
        // Engine::stop() also restores routing and is a no-op when idle.
        engine_.stop();
        std::string error;
        if (r.power) {
            if (r.cfg.capture == engine::CaptureMode::SelectedApps) {
                r.cfg.appPids = resolvePids(r.exes);
                if (r.cfg.appPids.empty()) {
                    error = "None of the selected apps is running. Start playback in one of them, then press Retry.";
                }
            }
            if (error.empty()) {
                logx::info("Starting engine");
                engine_.setScene(scene);
                engine_.setEnabled(enabled);
                if (!engine_.start(r.cfg)) {
                    error = engine_.status().error;
                    if (error.empty()) error = "The audio engine failed to start. See the log for details.";
                    engine_.stop();
                }
            }
            if (!error.empty()) logx::error(error);
        } else {
            logx::info("Engine stopped");
        }

        std::lock_guard lk(ctlMu_);
        if (applied_ == desired_.generation) {
            ctlError_ = std::move(error);
            busy_ = false;
        }
    }
    engine_.stop();
    if (SUCCEEDED(co)) CoUninitialize();
}

}  // namespace app
