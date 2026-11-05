#pragma once
// Spatiall's visual language: near-black surfaces, white as the only accent, monospace type, pill controls.
// Shared by the main panel and the quick-menu overlay.

#include <imgui.h>

#include <string>

namespace ui {

namespace col {
inline constexpr ImU32 kBg = IM_COL32(0x0B, 0x0B, 0x0D, 255);
inline constexpr ImU32 kCard = IM_COL32(0x12, 0x12, 0x14, 255);
inline constexpr ImU32 kBorder = IM_COL32(0x26, 0x26, 0x2A, 255);
inline constexpr ImU32 kOutline = IM_COL32(0x3A, 0x3A, 0x40, 255);
inline constexpr ImU32 kRing = IM_COL32(0x2A, 0x2A, 0x2F, 255);
inline constexpr ImU32 kText = IM_COL32(0xED, 0xED, 0xED, 255);
inline constexpr ImU32 kSecondary = IM_COL32(0x8A, 0x8A, 0x90, 255);
inline constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
inline constexpr ImU32 kOn = IM_COL32(0x3D, 0xD6, 0x8C, 255);
inline constexpr ImU32 kWarn = IM_COL32(0xD9, 0xA4, 0x41, 255);
inline constexpr ImU32 kError = IM_COL32(0xE5, 0x48, 0x4D, 255);
inline constexpr ImU32 kIdle = IM_COL32(0x5A, 0x5A, 0x60, 255);
}  // namespace col

// Unscaled sizes; style.FontScaleDpi applies the DPI factor.
inline constexpr float kBodySize = 15.0f;
inline constexpr float kTitleSize = 34.0f;
inline constexpr float kHeroSize = 44.0f;
inline constexpr float kLabelSize = 12.0f;

struct Fonts {
    ImFont* body = nullptr;   // Regular
    ImFont* light = nullptr;  // large titles
    ImFont* hero = nullptr;   // wordmark
    ImFont* label = nullptr;  // small uppercase section labels
};

// Loads <exeDir>\fonts\JetBrainsMono-*.ttf, falling back to Cascadia Mono, Consolas, then ImGui's default font for
// any missing face. Call once, before the first frame; the fonts live in `atlas`.
void loadFonts(ImFontAtlas* atlas, const std::wstring& exeDir);
const Fonts& fonts();

// Resets `style` to the Spatiall theme at `scale` (DPI factor).
void applyTheme(ImGuiStyle& style, float scale);

// Primary action = white filled pill with black text; secondary = outlined pill.
bool pillButton(const char* label, bool filled, const ImVec2& size = ImVec2(0.0f, 0.0f));
// Toggle-style chip: selected = white fill.
bool chip(const char* label, bool selected);
// Small uppercase label above a group of controls.
void sectionLabel(const char* text);
// A filled dot vertically centred on the current text line; advances the cursor like a small item.
void statusDot(ImU32 color);
// Wrapped text in `color`.
void wrappedText(ImU32 color, const char* text);

ImU32 withAlpha(ImU32 color, float alpha);
// Muted, distinguishable stem colours.
ImU32 stemColor(const std::string& name, size_t index);

}  // namespace ui
