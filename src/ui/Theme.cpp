#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/Theme.h"

#include "util/Log.h"

#include <imgui_internal.h>

#include <windows.h>

#include <algorithm>
#include <iterator>
#include <map>

namespace ui {
namespace {

// Fonts belong to an atlas, and each ImGui context (main window, quick menu) has its own.
std::map<const ImFontAtlas*, Fonts>& fontTable() {
    static std::map<const ImFontAtlas*, Fonts> table;
    return table;
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring windowsFonts() {
    wchar_t dir[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring(dir, n) + L"\\Fonts\\" : std::wstring();
}

ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

}  // namespace

void loadFonts(ImFontAtlas* atlas, const std::wstring& exeDir) {
    const std::wstring own = exeDir + L"\\fonts\\JetBrainsMono-";
    const std::wstring sys = windowsFonts();
    // Same family for every face when bundled; otherwise the best monospace Windows has.
    auto load = [&](const wchar_t* weight, float size) -> ImFont* {
        for (const std::wstring& path : {own + weight + L".ttf", sys + L"CascadiaMono.ttf", sys + L"consola.ttf"}) {
            if (!exists(path)) continue;
            if (ImFont* f = atlas->AddFontFromFileTTF(logx::narrow(path).c_str(), size)) return f;
        }
        return nullptr;
    };
    Fonts f;
    f.body = load(L"Regular", kBodySize);
    if (!f.body) {
        logx::warn("No monospace UI font found; using ImGui's default font");
        f.body = atlas->AddFontDefault();
    }
    f.light = load(L"Light", kTitleSize);
    f.hero = load(L"ExtraLight", kHeroSize);
    f.label = load(L"Medium", kLabelSize);
    if (!f.light) f.light = f.body;
    if (!f.hero) f.hero = f.light;
    if (!f.label) f.label = f.body;
    fontTable()[atlas] = f;
}

const Fonts& fonts() {
    static const Fonts none;
    const auto& table = fontTable();
    const auto it = table.find(ImGui::GetIO().Fonts);
    return it == table.end() ? none : it->second;
}

void applyTheme(ImGuiStyle& style, float scale) {
    style = ImGuiStyle();
    ImGui::StyleColorsDark(&style);
    style.FontSizeBase = kBodySize;
    style.WindowPadding = ImVec2(28.0f, 22.0f);
    style.FramePadding = ImVec2(14.0f, 7.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    style.ItemInnerSpacing = ImVec2(10.0f, 6.0f);
    style.CellPadding = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 8.0f;
    style.GrabMinSize = 10.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 14.0f;
    style.FrameRounding = 999.0f;  // pills
    style.GrabRounding = 999.0f;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 999.0f;
    style.TabRounding = 999.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextPadding = ImVec2(0.0f, 4.0f);
    style.DisabledAlpha = 0.4f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = v4(col::kText);
    c[ImGuiCol_TextDisabled] = v4(col::kSecondary);
    c[ImGuiCol_WindowBg] = v4(col::kBg);
    c[ImGuiCol_ChildBg] = v4(col::kCard);
    c[ImGuiCol_PopupBg] = v4(IM_COL32(0x14, 0x14, 0x16, 250));
    c[ImGuiCol_Border] = v4(col::kBorder);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = v4(IM_COL32(0x17, 0x17, 0x1A, 255));
    c[ImGuiCol_FrameBgHovered] = v4(IM_COL32(0x1E, 0x1E, 0x22, 255));
    c[ImGuiCol_FrameBgActive] = v4(IM_COL32(0x24, 0x24, 0x29, 255));
    c[ImGuiCol_Button] = v4(IM_COL32(0x17, 0x17, 0x1A, 255));
    c[ImGuiCol_ButtonHovered] = v4(IM_COL32(0x24, 0x24, 0x29, 255));
    c[ImGuiCol_ButtonActive] = v4(IM_COL32(0x2E, 0x2E, 0x34, 255));
    c[ImGuiCol_Header] = v4(IM_COL32(0x22, 0x22, 0x26, 255));
    c[ImGuiCol_HeaderHovered] = v4(IM_COL32(0x2A, 0x2A, 0x2F, 255));
    c[ImGuiCol_HeaderActive] = v4(IM_COL32(0x33, 0x33, 0x39, 255));
    c[ImGuiCol_SliderGrab] = v4(col::kWhite);
    c[ImGuiCol_SliderGrabActive] = v4(IM_COL32(0xD0, 0xD0, 0xD4, 255));
    c[ImGuiCol_CheckMark] = v4(col::kWhite);
    c[ImGuiCol_Separator] = v4(col::kBorder);
    c[ImGuiCol_SeparatorHovered] = v4(col::kOutline);
    c[ImGuiCol_SeparatorActive] = v4(col::kSecondary);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = v4(IM_COL32(0x2A, 0x2A, 0x2F, 255));
    c[ImGuiCol_ScrollbarGrabHovered] = v4(IM_COL32(0x3A, 0x3A, 0x40, 255));
    c[ImGuiCol_ScrollbarGrabActive] = v4(IM_COL32(0x4A, 0x4A, 0x50, 255));
    c[ImGuiCol_PlotHistogram] = v4(col::kText);
    c[ImGuiCol_TextSelectedBg] = v4(IM_COL32(255, 255, 255, 50));
    c[ImGuiCol_NavCursor] = v4(col::kWhite);
    c[ImGuiCol_ModalWindowDimBg] = v4(IM_COL32(0, 0, 0, 150));

    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

bool pillButton(const char* label, bool filled, const ImVec2& size) {
    if (filled) {
        ImGui::PushStyleColor(ImGuiCol_Button, v4(col::kWhite));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(IM_COL32(0xDC, 0xDC, 0xE0, 255)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, v4(IM_COL32(0xC0, 0xC0, 0xC6, 255)));
        ImGui::PushStyleColor(ImGuiCol_Text, v4(IM_COL32(0, 0, 0, 255)));
        ImGui::PushStyleColor(ImGuiCol_Border, v4(col::kWhite));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(IM_COL32(255, 255, 255, 18)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, v4(IM_COL32(255, 255, 255, 32)));
        ImGui::PushStyleColor(ImGuiCol_Text, v4(col::kText));
        ImGui::PushStyleColor(ImGuiCol_Border, v4(col::kOutline));
    }
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    return pressed;
}

bool chip(const char* label, bool selected) { return pillButton(label, selected); }

void sectionLabel(const char* text) {
    ImGui::PushFont(fonts().label, kLabelSize);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(col::kSecondary));
    std::string upper(text);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) { return std::toupper(ch); });
    ImGui::TextUnformatted(upper.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void statusDot(ImU32 color) {
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = h * 0.22f;
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + h * 0.5f), r, color);
    ImGui::Dummy(ImVec2(r * 2.0f, h));
}

void wrappedText(ImU32 color, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, v4(color));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

ImU32 withAlpha(ImU32 color, float alpha) {
    const float current = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF);
    const auto a = static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * current);
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

ImU32 stemColor(const std::string& name, size_t index) {
    struct Entry {
        const char* name;
        ImU32 col;
    };
    // Muted pastel set: distinguishable on near-black without competing with the white accent.
    static const Entry table[] = {
        {"drums", IM_COL32(0xE0, 0x9F, 0x6E, 255)},  {"bass", IM_COL32(0xA8, 0x93, 0xE0, 255)},
        {"other", IM_COL32(0x7C, 0xC4, 0xB4, 255)},  {"vocals", IM_COL32(0xE0, 0x8A, 0xA8, 255)},
        {"guitar", IM_COL32(0xD9, 0xC6, 0x7A, 255)}, {"piano", IM_COL32(0x84, 0xA9, 0xE0, 255)},
    };
    for (const auto& e : table) {
        if (name == e.name) return e.col;
    }
    return table[index % std::size(table)].col;
}

}  // namespace ui
