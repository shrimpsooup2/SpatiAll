#pragma once
// A global key combination for RegisterHotKey: modifier flags (MOD_ALT/CONTROL/SHIFT/WIN values) + virtual key.

#include <cstdint>
#include <string>

namespace app {

inline constexpr uint32_t kModAlt = 0x0001;      // MOD_ALT
inline constexpr uint32_t kModControl = 0x0002;  // MOD_CONTROL
inline constexpr uint32_t kModShift = 0x0004;    // MOD_SHIFT
inline constexpr uint32_t kModWin = 0x0008;      // MOD_WIN

struct Hotkey {
    uint32_t mods = 0;
    uint32_t vk = 0;  // 0 = none

    bool empty() const { return vk == 0; }
    bool operator==(const Hotkey&) const = default;
};

// "Ctrl+Alt+Space"; empty when unset.
std::string hotkeyText(const Hotkey& h);
// Stored as "mods,vk" (empty = none).
std::wstring hotkeyToString(const Hotkey& h);
Hotkey hotkeyFromString(const std::wstring& s);
// True while every modifier of `h` is physically held (GetAsyncKeyState).
bool hotkeyModifiersHeld(const Hotkey& h);

}  // namespace app
