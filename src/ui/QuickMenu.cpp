#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/QuickMenu.h"

#include "ui/Theme.h"
#include "util/Log.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cstdio>

namespace ui {
namespace {

constexpr wchar_t kClass[] = L"Spatiall.QuickMenu";
constexpr float kWidth = 360.0f;     // unscaled
constexpr float kPill = 44.0f;
constexpr float kGap = 8.0f;
constexpr float kPad = 18.0f;
constexpr float kHeader = 46.0f;

bool keyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Swaps the current ImGui context for the scope; the main window's context is restored afterwards.
struct ContextScope {
    ImGuiContext* prev;
    explicit ContextScope(ImGuiContext* ctx) : prev(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(ctx); }
    ~ContextScope() { ImGui::SetCurrentContext(prev); }
};

}  // namespace

QuickMenu::~QuickMenu() { destroy(); }

LRESULT CALLBACK QuickMenu::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Clicking the overlay must not pull focus away from the game or app underneath.
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool QuickMenu::create(HINSTANCE instance, ID3D11Device* device, const std::wstring& exeDir) {
    instance_ = instance;
    device_ = device;
    device_->GetImmediateContext(&context_);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &QuickMenu::wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    // No redirection bitmap: DirectComposition supplies all pixels, including fully transparent ones.
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, kClass,
                            L"Spatiall layouts", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    if (!hwnd_) {
        logx::warn("quick menu: window creation failed");
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    HRESULT hr = device_.As(&dxgiDevice);
    if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 64;
        sd.Height = 64;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        sd.Scaling = DXGI_SCALING_STRETCH;
        hr = factory->CreateSwapChainForComposition(device_.Get(), &sd, nullptr, &swapChain_);
    }
    if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcomp_));
    if (SUCCEEDED(hr)) hr = dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &target_);
    if (SUCCEEDED(hr)) hr = dcomp_->CreateVisual(&visual_);
    if (SUCCEEDED(hr)) hr = visual_->SetContent(swapChain_.Get());
    if (SUCCEEDED(hr)) hr = target_->SetRoot(visual_.Get());
    if (SUCCEEDED(hr)) hr = dcomp_->Commit();
    if (FAILED(hr)) {
        logx::warn("quick menu: DirectComposition setup failed: " + logx::hr(hr));
        destroy();
        return false;
    }
    width_ = height_ = 64;

    // Own context and font atlas: sharing the main atlas would make both DX11 backends own its texture.
    ContextScope scope(nullptr);
    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    loadFonts(io.Fonts, exeDir);
    applyTheme(ImGui::GetStyle(), 1.0f);
    ImGui_ImplDX11_Init(device_.Get(), context_.Get());
    QueryPerformanceCounter(&lastFrame_);
    return true;
}

void QuickMenu::destroy() {
    if (imgui_) {
        ContextScope scope(imgui_);
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext(imgui_);
        imgui_ = nullptr;
    }
    rtv_.Reset();
    visual_.Reset();
    target_.Reset();
    dcomp_.Reset();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (instance_) UnregisterClassW(kClass, instance_);
    instance_ = nullptr;
    open_ = false;
}

bool QuickMenu::resize(int w, int h) {
    if (w == width_ && h == height_ && rtv_) return true;
    rtv_.Reset();
    if (FAILED(swapChain_->ResizeBuffers(0, static_cast<UINT>(w), static_cast<UINT>(h), DXGI_FORMAT_UNKNOWN, 0))) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    if (FAILED(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_))) return false;
    width_ = w;
    height_ = h;
    return true;
}

void QuickMenu::layout() {
    POINT cursor;
    GetCursorPos(&cursor);
    const HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    scale_ = ImGui_ImplWin32_GetDpiScaleForMonitor(mon);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT wa = mi.rcWork;

    const int w = static_cast<int>(kWidth * scale_);
    const float rows = static_cast<float>(items_.size());
    const int h = static_cast<int>((kPad * 2.0f + kHeader + rows * kPill + std::max(0.0f, rows - 1.0f) * kGap) * scale_);
    // Centred on the cursor, kept fully on the monitor's work area.
    int x = cursor.x - w / 2;
    int y = cursor.y - static_cast<int>((kPad + kHeader + kPill * 0.5f) * scale_);
    x = std::clamp(x, static_cast<int>(wa.left), std::max(static_cast<int>(wa.left), static_cast<int>(wa.right) - w));
    y = std::clamp(y, static_cast<int>(wa.top), std::max(static_cast<int>(wa.top), static_cast<int>(wa.bottom) - h));
    origin_ = {x, y};
    resize(w, h);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    ContextScope scope(imgui_);
    applyTheme(ImGui::GetStyle(), scale_);
}

void QuickMenu::open(std::vector<std::string> items, int active, const app::Hotkey& held) {
    if (!hwnd_ || items.empty()) return;
    items_ = std::move(items);
    active_ = active;
    held_ = held;
    hovered_ = -1;
    for (int d = 0; d < 9; ++d) digitDown_[d] = keyDown('1' + d) || keyDown(VK_NUMPAD1 + d);
    open_ = true;
    layout();
    frame();
}

void QuickMenu::close() {
    if (!open_) return;
    open_ = false;
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

int QuickMenu::frame() {
    if (!open_ || !imgui_) return -1;
    int chosen = -1;

    // Released the combo: take the pill under the mouse, if any.
    if (!app::hotkeyModifiersHeld(held_)) {
        chosen = hovered_;
        close();
        return chosen;
    }
    for (int d = 0; d < 9; ++d) {
        const bool down = keyDown('1' + d) || keyDown(VK_NUMPAD1 + d);
        if (down && !digitDown_[static_cast<size_t>(d)] && d < static_cast<int>(items_.size())) chosen = d;
        digitDown_[static_cast<size_t>(d)] = down;
    }

    ContextScope scope(imgui_);
    ImGuiIO& io = ImGui::GetIO();
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    io.DeltaTime = std::max(1e-4f, static_cast<float>(now.QuadPart - lastFrame_.QuadPart) / freq.QuadPart);
    lastFrame_ = now;
    io.DisplaySize = ImVec2(static_cast<float>(width_), static_cast<float>(height_));
    // The window never has focus, so read the global mouse instead of relying on window messages.
    POINT cursor;
    GetCursorPos(&cursor);
    io.AddMousePosEvent(static_cast<float>(cursor.x - origin_.x), static_cast<float>(cursor.y - origin_.y));
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    io.AddMouseButtonEvent(0, keyDown(swapped ? VK_RBUTTON : VK_LBUTTON));

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##quick", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float s = scale_;
    const ImVec2 size = io.DisplaySize;
    // Translucent glass: the desktop/game stays visible through everything but the pills' outlines and text.
    dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(11, 11, 13, 150), 18.0f * s);
    dl->AddRect(ImVec2(0, 0), size, IM_COL32(255, 255, 255, 26), 18.0f * s, 0, 1.0f * s);

    const Fonts& f = fonts();
    const float pad = kPad * s;
    dl->AddText(f.label, kLabelSize * s, ImVec2(pad + 4.0f * s, pad + 4.0f * s), col::kSecondary, "LAYOUTS");
    const char* hint = "release to choose";
    const ImVec2 hintSize = f.label->CalcTextSizeA(kLabelSize * s, FLT_MAX, 0.0f, hint);
    dl->AddText(f.label, kLabelSize * s, ImVec2(size.x - pad - 4.0f * s - hintSize.x, pad + 4.0f * s),
                withAlpha(col::kSecondary, 0.7f), hint);

    hovered_ = -1;
    float y = pad + kHeader * s;
    for (size_t i = 0; i < items_.size(); ++i) {
        const ImVec2 p0(pad, y), p1(size.x - pad, y + kPill * s);
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::InvisibleButton("##pill", p1 - p0)) chosen = static_cast<int>(i);
        const bool hot = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hot) hovered_ = static_cast<int>(i);
        const bool isActive = static_cast<int>(i) == active_;
        const float r = (kPill * s) * 0.5f;
        if (hot) {
            dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 235), r);
        } else {
            dl->AddRectFilled(p0, p1, IM_COL32(14, 14, 16, 175), r);
            dl->AddRect(p0, p1, IM_COL32(255, 255, 255, isActive ? 200 : 70), r, 0, 1.0f * s);
        }
        const ImU32 text = hot ? IM_COL32(0, 0, 0, 255) : col::kText;
        const ImU32 sub = hot ? IM_COL32(0, 0, 0, 150) : withAlpha(col::kSecondary, 0.9f);
        const float fs = kBodySize * s;
        const float ty = y + (kPill * s - fs) * 0.5f;
        if (i < 9) {
            char num[4];
            std::snprintf(num, sizeof(num), "%zu", i + 1);
            dl->AddText(f.body, fs, ImVec2(p0.x + 18.0f * s, ty), sub, num);
        }
        dl->PushClipRect(p0, ImVec2(p1.x - 40.0f * s, p1.y), true);
        dl->AddText(f.body, fs, ImVec2(p0.x + 46.0f * s, ty), text, items_[i].c_str());
        dl->PopClipRect();
        if (isActive) dl->AddCircleFilled(ImVec2(p1.x - 22.0f * s, y + kPill * s * 0.5f), 3.5f * s, hot ? text : col::kOn);
        y += (kPill + kGap) * s;
    }
    ImGui::End();
    ImGui::Render();

    // Cleared to transparent; ImGui's blending over transparent black yields premultiplied alpha, which is what
    // the composition swap chain expects.
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ID3D11RenderTargetView* rtv = rtv_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->ClearRenderTargetView(rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(0, 0);

    if (chosen >= 0) close();
    return chosen;
}

}  // namespace ui
