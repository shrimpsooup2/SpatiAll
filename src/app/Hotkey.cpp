#include "app/Hotkey.h"

#include "util/Log.h"

#include <windows.h>

#include <cstdlib>

namespace app {
namespace {

static_assert(kModAlt == MOD_ALT && kModControl == MOD_CONTROL && kModShift == MOD_SHIFT && kModWin == MOD_WIN);

std::string keyName(uint32_t vk) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num" + std::to_string(vk - VK_NUMPAD0);
    switch (vk) {
        case VK_SPACE: return "Space";
        case VK_TAB: return "Tab";
        case VK_RETURN: return "Enter";
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "PageUp";
        case VK_NEXT: return "PageDown";
        case VK_LEFT: return "Left";
        case VK_RIGHT: return "Right";
        case VK_UP: return "Up";
        case VK_DOWN: return "Down";
        case VK_PAUSE: return "Pause";
        default: break;
    }
    // Layout-dependent keys (punctuation, OEM): ask Windows for the label of the key on this keyboard.
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t name[64];
    if (scan != 0 && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) return logx::narrow(name);
    return "Key " + std::to_string(vk);
}

}  // namespace

std::string hotkeyText(const Hotkey& h) {
    if (h.empty()) return {};
    std::string s;
    if (h.mods & kModControl) s += "Ctrl+";
    if (h.mods & kModAlt) s += "Alt+";
    if (h.mods & kModShift) s += "Shift+";
    if (h.mods & kModWin) s += "Win+";
    return s + keyName(h.vk);
}

std::wstring hotkeyToString(const Hotkey& h) {
    if (h.empty()) return {};
    return std::to_wstring(h.mods) + L"," + std::to_wstring(h.vk);
}

Hotkey hotkeyFromString(const std::wstring& s) {
    wchar_t* end = nullptr;
    const unsigned long mods = std::wcstoul(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != L',') return {};
    const wchar_t* vkStart = end + 1;
    const unsigned long vk = std::wcstoul(vkStart, &end, 10);
    if (end == vkStart || vk == 0 || vk > 0xFE) return {};
    const uint32_t allMods = kModAlt | kModControl | kModShift | kModWin;
    if ((mods & allMods) == 0) return {};  // a bare key would hijack normal typing everywhere
    return {static_cast<uint32_t>(mods) & allMods, static_cast<uint32_t>(vk)};
}

bool hotkeyModifiersHeld(const Hotkey& h) {
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if ((h.mods & kModControl) && !down(VK_CONTROL)) return false;
    if ((h.mods & kModAlt) && !down(VK_MENU)) return false;
    if ((h.mods & kModShift) && !down(VK_SHIFT)) return false;
    if ((h.mods & kModWin) && !down(VK_LWIN) && !down(VK_RWIN)) return false;
    return true;
}

}  // namespace app
