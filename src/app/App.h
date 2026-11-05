#pragma once
// Application shell: window + D3D11 + ImGui, tray, hotkeys, settings, and the engine control thread.

#include "app/Layouts.h"
#include "app/Settings.h"
#include "app/Tray.h"
#include "engine/Engine.h"
#include "track/HeadTracker.h"
#include "ui/QuickMenu.h"
#include "ui/Ui.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace app {

inline constexpr wchar_t kWindowClass[] = L"Spatiall.MainWindow";
// Posted by a second instance to bring this one to the front.
inline constexpr wchar_t kActivateMessage[] = L"Spatiall.Activate";

class App {
public:
    App() = default;
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool init(HINSTANCE instance, int showCmd);
    int run();

private:
    using Clock = std::chrono::steady_clock;

    // What the control thread should converge to. appPids are resolved from exes at start time.
    struct Request {
        uint64_t generation = 0;
        bool power = false;
        engine::Config cfg;
        std::vector<std::string> exes;
    };

    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    bool createDevice();
    void destroyDevice();
    void createRenderTarget();
    void renderFrame();
    void shutdown();

    void tick(bool visible);
    void updateTray();
    void showWindow();
    void hideWindow();
    void exitApp();
    void onTrayCommand(UINT cmd);
    void onEndSession();

    // Layouts: animated switching, persistence, global hotkeys and the hold-to-open quick menu.
    void applyLayout(int index);
    void animateTo(const engine::SceneParams& target);
    void stepTransition();
    void handleLayoutActions(const ui::Actions& a);
    void openQuickMenu();
    void registerHotkeys();
    void unregisterHotkeys();
    void beginHotkeyCapture(int target);
    void endHotkeyCapture();
    bool onCaptureKey(UINT vk);
    void setHotkey(int target, const Hotkey& hk);

    void togglePower();
    void toggleSpatial();
    void handle(const ui::Actions& a);
    void pushScene();

    void refreshDevices();
    void refreshSessions();
    void scanModels();
    // Model stem list changed: indices now mean other instruments, so the stems are laid out like Stage.
    void setStemNames(const std::vector<std::string>& names);
    // Headphone selection changed (or first run): refresh traits and switch to that device's tracking profile.
    void syncHeadphones();
    void restartTracker();
    void autoPickDevices();
    void markDirty();
    void saveNow();

    engine::Config buildConfig() const;
    std::string validate() const;
    void reconcile(bool force);
    void controlLoop();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT activateMsg_ = 0;
    std::wstring modelsDir_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    UINT resizeW_ = 0;
    UINT resizeH_ = 0;
    bool occluded_ = false;
    bool imguiReady_ = false;
    float dpiScale_ = 1.0f;
    bool shutDown_ = false;
    // Declared before engine_ so it outlives it; shutdown() also detaches it explicitly.
    track::HeadTracker tracker_;
    engine::Engine engine_;
    Settings settings_;
    bool profileLoaded_ = false;  // settings_.tracking matches settings_.headphoneDeviceId
    Tray tray_;
    ui::Ui ui_;
    ui::View view_;  // also the App's store for devices, sessions, models and stem names
    Layouts layouts_;
    ui::QuickMenu quickMenu_;
    std::wstring exeDir_;
    int captureTarget_ = ui::kCaptureNone;
    bool transitioning_ = false;
    engine::SceneParams transitionFrom_, transitionTo_;
    Clock::time_point transitionStart_{};
    Clock::time_point lastFullscreenCheck_{};

    bool powerWanted_ = false;
    std::string validationError_;
    std::wstring lastModelFile_;
    bool dirty_ = false;
    Clock::time_point dirtyAt_{};
    Clock::time_point lastDeviceRefresh_{};
    Tray::State trayState_ = Tray::State::Count;  // forces the first update
    std::string trayError_;
    bool trayEnabled_ = true;

    // Engine start blocks for seconds (model load), so start/stop run serialized on this thread.
    std::thread control_;
    std::mutex ctlMu_;
    std::condition_variable ctlCv_;
    Request desired_;              // guarded by ctlMu_
    uint64_t applied_ = 0;         // guarded by ctlMu_
    bool ctlQuit_ = false;         // guarded by ctlMu_
    std::string ctlError_;         // guarded by ctlMu_
    engine::SceneParams ctlScene_;  // guarded by ctlMu_; applied right before each start
    bool ctlEnabled_ = true;       // guarded by ctlMu_
    std::atomic<bool> busy_{false};
};

}  // namespace app
