#include "app/Tray.h"

#include "util/Log.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <vector>

namespace app {
namespace {

constexpr UINT kIconId = 1;

// Ring + centre dot in the state colour, drawn with anti-aliased coverage so it reads at 16 px.
HICON makeIcon(int size, BYTE r, BYTE g, BYTE b) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;

    auto* px = static_cast<uint32_t*>(bits);
    const float c = (static_cast<float>(size) - 1.0f) * 0.5f;
    const float outer = static_cast<float>(size) * 0.5f - 0.5f;
    const float inner = outer - std::max(1.5f, static_cast<float>(size) * 0.14f);
    const float dot = static_cast<float>(size) * 0.2f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float d = std::hypot(static_cast<float>(x) - c, static_cast<float>(y) - c);
            const float ring = std::clamp(outer + 0.5f - d, 0.0f, 1.0f) * std::clamp(d - inner + 0.5f, 0.0f, 1.0f);
            const float centre = std::clamp(dot + 0.5f - d, 0.0f, 1.0f);
            const auto a = static_cast<uint32_t>(std::max(ring, centre) * 255.0f + 0.5f);
            px[y * size + x] = (a << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
        }
    }

    // The 32-bit colour bitmap carries alpha; the monochrome mask just has to exist (rows are WORD aligned).
    std::vector<BYTE> maskBits(static_cast<size_t>((size + 15) / 16 * 2 * size), 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = mask ? CreateIconIndirect(&ii) : nullptr;
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

}  // namespace

Tray::~Tray() { destroy(); }

bool Tray::create(HWND hwnd) {
    hwnd_ = hwnd;
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    // Explorer's broadcast would be dropped by UIPI if we ever run elevated.
    if (taskbarCreatedMsg_) ChangeWindowMessageFilterEx(hwnd, taskbarCreatedMsg_, MSGFLT_ALLOW, nullptr);

    const int size = GetSystemMetrics(SM_CXSMICON);
    struct Rgb {
        BYTE r, g, b;
    };
    constexpr std::array<Rgb, static_cast<size_t>(State::Count)> colors{{
        {150, 150, 150},  // Off
        {235, 170, 40},   // Starting
        {60, 205, 115},   // Spatial
        {80, 150, 235},   // Stereo (running, spatial off)
        {225, 70, 60},    // Error
    }};
    for (size_t i = 0; i < icons_.size(); ++i) {
        icons_[i] = makeIcon(size, colors[i].r, colors[i].g, colors[i].b);
        if (!icons_[i]) icons_[i] = LoadIconW(nullptr, IDI_APPLICATION);
    }
    return add();
}

bool Tray::add() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kCallbackMessage;
    nid.hIcon = icons_[static_cast<size_t>(state_)];
    wcsncpy_s(nid.szTip, tooltip_.c_str(), _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        // Explorer may not be up yet (early autostart); TaskbarCreated will retry.
        logx::warn("Tray icon could not be added yet");
        added_ = false;
        return false;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    added_ = true;
    return true;
}

void Tray::readd() {
    added_ = false;
    add();
}

void Tray::destroy() {
    if (added_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        added_ = false;
    }
    const HICON stock = LoadIconW(nullptr, IDI_APPLICATION);  // shared fallback; must not be destroyed
    for (HICON& icon : icons_) {
        if (icon && icon != stock) DestroyIcon(icon);
        icon = nullptr;
    }
}

void Tray::update(State state, const std::wstring& tooltip) {
    if (state == state_ && tooltip == tooltip_ && added_) return;
    state_ = state;
    tooltip_ = tooltip;
    if (!hwnd_) return;
    if (!added_) {
        add();
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = icons_[static_cast<size_t>(state_)];
    wcsncpy_s(nid.szTip, tooltip_.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

UINT Tray::showMenu(POINT anchor, const MenuState& state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return CmdNone;
    AppendMenuW(menu, MF_STRING | (state.spatialOn ? MF_CHECKED : MF_UNCHECKED), CmdToggleSpatial,
                state.spatialOn ? L"Spatial audio: On\tCtrl+Alt+S" : L"Spatial audio: Off\tCtrl+Alt+S");
    AppendMenuW(menu, MF_STRING | (state.powerOn ? MF_CHECKED : MF_UNCHECKED), CmdTogglePower,
                state.powerOn ? L"Power: On\tCtrl+Alt+P" : L"Power: Off\tCtrl+Alt+P");
    AppendMenuW(menu, MF_STRING | (state.trackingOn ? MF_ENABLED : MF_GRAYED), CmdRecenter,
                L"Recenter head tracking\tCtrl+Alt+C");

    const UINT layoutCount = std::min(static_cast<UINT>(state.layouts.size()), kMaxMenuLayouts);
    HMENU layouts = layoutCount > 0 ? CreatePopupMenu() : nullptr;
    if (layouts) {
        for (UINT i = 0; i < layoutCount; ++i) {
            std::wstring label = logx::widen(state.layouts[i]);
            // '&' would turn the next letter into a mnemonic.
            for (size_t pos = label.find(L'&'); pos != std::wstring::npos; pos = label.find(L'&', pos + 2)) {
                label.insert(pos, 1, L'&');
            }
            const bool active = static_cast<int>(i) == state.activeLayout;
            AppendMenuW(layouts, MF_STRING | (active ? MF_CHECKED : MF_UNCHECKED), kCmdLayoutBase + i, label.c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layouts), L"Layouts");  // owned by `menu` now
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdShow, L"Show");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdExit, L"Exit");
    SetMenuDefaultItem(menu, CmdShow, FALSE);

    // Required so the menu closes when the user clicks elsewhere (see TrackPopupMenu remarks).
    SetForegroundWindow(hwnd_);
    const UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const auto cmd = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | align,
                                                        anchor.x, anchor.y, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);  // also destroys the submenu
    switch (cmd) {
        case CmdToggleSpatial:
        case CmdTogglePower:
        case CmdShow:
        case CmdExit:
        case CmdRecenter:
            return cmd;
        default:
            return cmd >= kCmdLayoutBase && cmd < kCmdLayoutBase + layoutCount ? cmd : CmdNone;
    }
}

}  // namespace app
