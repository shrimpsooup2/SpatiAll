#pragma once
// Hold-to-open layout switcher: a see-through, always-on-top overlay near the mouse that never takes focus.
// Rendered with its own ImGui context into a DirectComposition swap chain with per-pixel alpha.
//
// It stays open while the hotkey's modifiers are held. Click a pill, press 1-9, or release the keys while
// hovering a pill to pick it. Exclusive-fullscreen games may draw over it; per-layout hotkeys still work there.

#include "app/Hotkey.h"

#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <string>
#include <vector>

struct ImGuiContext;

namespace ui {

class QuickMenu {
public:
    QuickMenu() = default;
    ~QuickMenu();
    QuickMenu(const QuickMenu&) = delete;
    QuickMenu& operator=(const QuickMenu&) = delete;

    bool create(HINSTANCE instance, ID3D11Device* device, const std::wstring& exeDir);
    void destroy();

    void open(std::vector<std::string> items, int active, const app::Hotkey& held);
    void close();
    bool isOpen() const { return open_; }

    // Drive every ~15 ms while open (WM_TIMER): polls mouse/keys and renders. Returns the chosen item, else -1.
    int frame();

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool resize(int w, int h);
    void layout();

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> visual_;
    ImGuiContext* imgui_ = nullptr;

    bool open_ = false;
    std::vector<std::string> items_;
    int active_ = -1;
    int hovered_ = -1;
    app::Hotkey held_;
    float scale_ = 1.0f;
    int width_ = 0, height_ = 0;
    POINT origin_{};
    LARGE_INTEGER lastFrame_{};
    std::array<bool, 9> digitDown_{};
};

}  // namespace ui
