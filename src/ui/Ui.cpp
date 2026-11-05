#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/Ui.h"

#include "ui/Theme.h"
#include "util/Log.h"

#include <imgui.h>
#include <imgui_internal.h>  // ShadeVertsLinearColorGradientKeepAlpha for the wordmark sheen

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <numbers>

namespace ui {
namespace {

constexpr float kMinDist = 0.3f;
constexpr float kMaxDist = 5.0f;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr ImGuiSliderFlags kClamp = ImGuiSliderFlags_AlwaysClamp;

float rad(float deg) { return deg * kPi / 180.0f; }

float wrapDeg(float d) {
    d = std::fmod(d + 180.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}

// sqrt mapping gives the near field (where placement matters most) more room on screen.
float distToRadius(float d, float roomR) { return roomR * std::sqrt(std::clamp(d, kMinDist, kMaxDist) / kMaxDist); }

float radiusToDist(float r, float roomR) {
    const float t = r / roomR;
    return std::clamp(kMaxDist * t * t, kMinDist, kMaxDist);
}

// View angle in degrees: 0 = up (front), +90 = right.
ImVec2 polar(ImVec2 c, float r, float deg) { return {c.x + r * std::sin(rad(deg)), c.y - r * std::cos(rad(deg))}; }

// ImGui arc angles start at +x and grow clockwise on screen.
float arcAngle(float deg) { return rad(deg - 90.0f); }

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// RMS -> 0..1 on a 50 dB scale for orb pulsing and meters.
float levelNorm(float rms) {
    if (rms <= 1e-5f) return 0.0f;
    return std::clamp((20.0f * std::log10(rms) + 50.0f) / 50.0f, 0.0f, 1.0f);
}

float smoothLevel(float current, float target, float dt) {
    const float rate = target > current ? 30.0f : 4.0f;  // fast attack, slow release
    return current + (target - current) * std::min(1.0f, dt * rate);
}

// Which placement the user is hearing: while running the engine's live choice (Adaptive switches on its own),
// otherwise what the selected mode will start with.
bool renderingSpeakers(const app::Settings& s, const View& v) {
    if (v.status.running) return v.status.activeMode == engine::Mode::VirtualSpeakers;
    return s.mode == engine::Mode::VirtualSpeakers;
}

void tooltipEvenIfDisabled(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", text);
    }
}

// Lays out pills left to right, wrapping when the next one would not fit.
void flowBefore(bool first, const char* label) {
    if (first) return;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float w = ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f;
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
}

// Form row: secondary label in a fixed left column, control filling the rest.
float g_labelColumn = 0.0f;
void rowLabel(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(g_labelColumn);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool sliderRow(const char* label, float* v, float lo, float hi, const char* fmt, const char* tip = nullptr) {
    rowLabel(label);
    ImGui::PushID(label);
    const bool changed = ImGui::SliderFloat("##v", v, lo, hi, fmt, kClamp);
    ImGui::PopID();
    if (tip) ImGui::SetItemTooltip("%s", tip);
    return changed;
}

void thinMeter(float level) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 3.0f * ImGui::GetStyle().FontScaleDpi;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + ImVec2(w, h), col::kBorder, h);
    dl->AddRectFilled(p, p + ImVec2(w * std::clamp(level, 0.0f, 1.0f), h), col::kText, h);
    ImGui::Dummy(ImVec2(w, h));
}

struct Orb {
    int index;  // stem index, or 0/1 for the L/R speaker
    float az;
    float dist;
    float width;
    ImU32 col;
    const char* label;
    float level;
    bool silent;  // muted, or not soloed while another stem is
};

// Fixed capacity: the stage is rebuilt every frame, so keep it off the heap.
struct OrbList {
    std::array<Orb, engine::kMaxStems> items{};
    size_t count = 0;
    void push_back(const Orb& o) {
        if (count < items.size()) items[count++] = o;
    }
    size_t size() const { return count; }
    const Orb& operator[](size_t i) const { return items[i]; }
    const Orb* begin() const { return items.data(); }
    const Orb* end() const { return items.data() + count; }
};

}  // namespace

std::vector<std::string> defaultStemNames(const std::wstring& modelFile) {
    if (lower(logx::narrow(modelFile)).find("6s") != std::string::npos) {
        return {"drums", "bass", "other", "vocals", "guitar", "piano"};
    }
    return {"drums", "bass", "other", "vocals"};
}

const char* stemDisplayName(const std::string& stemName, const std::vector<std::string>& stemNames) {
    if (stemName == "drums") return "Drums";
    if (stemName == "bass") return "Bass";
    if (stemName == "other") {
        const bool split = std::any_of(stemNames.begin(), stemNames.end(),
                                       [](const std::string& n) { return n == "guitar" || n == "piano"; });
        return split ? "Other instruments" : "Melody & instruments";
    }
    if (stemName == "vocals") return "Vocals / voices";
    if (stemName == "guitar") return "Guitar";
    if (stemName == "piano") return "Piano / keys";
    return stemName.c_str();  // unknown model source: show its own name
}

Actions Ui::draw(app::Settings& s, const View& v) {
    Actions a;
    const float dt = ImGui::GetIO().DeltaTime;
    for (size_t i = 0; i < smoothedLevel_.size(); ++i) {
        const float target = v.status.running ? levelNorm(v.status.stemLevels[i]) : 0.0f;
        smoothedLevel_[i] = smoothLevel(smoothedLevel_[i], target, dt);
    }
    smoothedInput_ = smoothLevel(smoothedInput_, v.status.running ? levelNorm(v.status.inputPeak) : 0.0f, dt);
    const int stemCount = static_cast<int>(std::min<size_t>(v.stemNames.size(), engine::kMaxStems));
    selected_ = std::clamp(selected_, 0, std::max(0, stemCount - 1));
    g_labelColumn = ImGui::CalcTextSize("Prediction  ").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("Spatiall", nullptr, flags);

    drawTopBar(s, v, a);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float footerH = ImGui::GetFrameHeight() + style.ItemSpacing.y;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float bodyH = std::max(avail.y - footerH, 200.0f * v.dpiScale);
    const float leftW = std::floor(avail.x * 0.52f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f * v.dpiScale, 18.0f * v.dpiScale));
    ImGui::BeginChild("##stage", ImVec2(leftW, bodyH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawStage(s, v, a);
    ImGui::EndChild();
    ImGui::SameLine(0.0f, 18.0f * v.dpiScale);

    ImGui::BeginGroup();
    // Category chips, like the reference's filter row.
    struct TabDef {
        const char* label;
        Tab tab;
    };
    static const TabDef tabs[] = {{"Mix", Tab::Mix},         {"Layouts", Tab::Layouts}, {"Room", Tab::Room},
                                  {"Headphones", Tab::Headphones}, {"Sources", Tab::Sources},
                                  {"Advanced", Tab::Advanced}};
    bool first = true;
    for (const TabDef& t : tabs) {
        flowBefore(first, t.label);
        first = false;
        if (chip(t.label, tab_ == t.tab)) tab_ = t.tab;
    }
    const float panelH = bodyH - (ImGui::GetCursorScreenPos().y - ImGui::GetItemRectMin().y) +
                         ImGui::GetFrameHeight() - ImGui::GetFrameHeightWithSpacing() - style.ItemSpacing.y;
    ImGui::BeginChild("##panel", ImVec2(0.0f, std::max(panelH, 100.0f * v.dpiScale)), ImGuiChildFlags_Borders);
    switch (tab_) {
        case Tab::Mix: drawMix(s, v, a); break;
        case Tab::Layouts: drawLayouts(s, v, a); break;
        case Tab::Room: drawRoom(s, v, a); break;
        case Tab::Headphones: drawHeadphones(s, v, a); break;
        case Tab::Sources: drawSources(s, v, a); break;
        case Tab::Advanced: drawAdvanced(s, v, a); break;
    }
    ImGui::EndChild();
    ImGui::EndGroup();
    ImGui::PopStyleVar();

    // Footer: system facts left, window actions right.
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("spatiall %s  ·  windows build %u  ·  per-app routing %s", SPATIALL_VERSION, v.windowsBuild,
                        v.perAppRouting ? "available" : "unavailable");
    const float actionsW = ImGui::CalcTextSize("Hide to tray").x + ImGui::CalcTextSize("Exit").x +
                           style.FramePadding.x * 4.0f + style.ItemSpacing.x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - actionsW));
    if (pillButton("Hide to tray", false)) a.hide = true;
    ImGui::SameLine();
    if (pillButton("Exit", false)) a.exit = true;
    ImGui::End();

    if (a.sceneChanged || a.configChanged || a.trackingChanged) a.settingsChanged = true;
    return a;
}

void Ui::drawTopBar(app::Settings& s, const View& v, Actions& a) {
    const engine::Status& st = v.status;
    const float sc = v.dpiScale;
    const Fonts& f = fonts();

    // Wordmark + tagline.
    ImGui::BeginGroup();
    ImGui::PushFont(f.hero, kHeroSize);
    {
        // Wordmark with a soft diagonal sheen (bright top-left to grey bottom-right), like the reference.
        const char* word = "Spatiall";
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::CalcTextSize(word);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const int vtxStart = dl->VtxBuffer.Size;
        dl->AddText(p, col::kWhite, word);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, vtxStart, dl->VtxBuffer.Size, p, p + size,
                                                      IM_COL32(0xF7, 0xF7, 0xF9, 255), IM_COL32(0x8E, 0x8E, 0x96, 255));
        ImGui::Dummy(size);
    }
    ImGui::PopFont();
    ImGui::PushFont(f.label, kLabelSize);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col::kSecondary));
    ImGui::TextUnformatted("space for your sound");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::EndGroup();
    const float barTop = ImGui::GetItemRectMin().y;
    const float barBottom = ImGui::GetItemRectMax().y;

    // Right side: status pill, spatial toggle, power.
    const char* powerLabel = v.busy ? (v.powerWanted ? "Starting…###power" : "Stopping…###power")
                             : st.running ? "Power off###power"
                                          : "Power on###power";
    const char* spatialLabel = s.enabled ? "Spatial on###spatial" : "Spatial off###spatial";
    char status[256];
    ImU32 dot = col::kIdle;
    if (v.busy) {
        dot = col::kWarn;
        std::snprintf(status, sizeof(status), "%s", st.stage.empty() ? "working" : lower(st.stage).c_str());
    } else if (st.running) {
        dot = col::kOn;
        int n = std::snprintf(status, sizeof(status), "%.0f ms", st.latencySec * 1000.0);
        auto append = [&](const char* fmt, auto... args) {
            if (n > 0 && static_cast<size_t>(n) < sizeof(status))
                n += std::snprintf(status + n, sizeof(status) - static_cast<size_t>(n), fmt, args...);
        };
        if (s.mode == engine::Mode::Adaptive) append(" · auto");
        if (st.activeMode == engine::Mode::Stems) {
            append(" · stems · hop %.2f s · gpu %.0f%%", st.hopSec, st.gpuLoad * 100);
        } else {
            append(" · speakers");
            if (!st.adaptiveReason.empty()) append(" (%s)", st.adaptiveReason.c_str());
        }
        if (s.tracking.source != track::Source::None && n > 0) {
            std::snprintf(status + n, sizeof(status) - static_cast<size_t>(n), " · head %s",
                          v.headTracked ? "●" : "○");
        }
    } else if (v.powerWanted && !v.error.empty()) {
        dot = col::kError;
        std::snprintf(status, sizeof(status), "error");
    } else {
        std::snprintf(status, sizeof(status), "off");
    }
    // Placed explicitly on one row, vertically centred on the wordmark and flush with the panel's right edge.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float frameH = ImGui::GetFrameHeight();
    const float dotR = 3.5f * sc;
    const float statusW = ImGui::CalcTextSize(status).x + dotR * 2.0f + 8.0f * sc + style.FramePadding.x * 2.0f;
    const float spatialW = ImGui::CalcTextSize("Spatial off").x + style.FramePadding.x * 2.0f;
    const float powerW = std::max(ImGui::CalcTextSize("Power off").x, ImGui::CalcTextSize("Starting…").x) +
                         style.FramePadding.x * 2.0f + 24.0f * sc;
    const float gap = style.ItemSpacing.x;
    const float y = barTop + (barBottom - barTop - frameH) * 0.5f;
    float x = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - (statusW + spatialW + powerW + gap * 2.0f);

    // Status pill: outlined capsule with a coloured dot.
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    {
        const ImVec2 p(x, y);
        const ImVec2 sz(statusW, frameH);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRect(p, p + sz, col::kBorder, sz.y * 0.5f, 0, 1.0f * sc);
        dl->AddCircleFilled(ImVec2(p.x + style.FramePadding.x + dotR, p.y + sz.y * 0.5f), dotR, dot);
        dl->AddText(ImVec2(p.x + style.FramePadding.x + dotR * 2.0f + 8.0f * sc, p.y + style.FramePadding.y),
                    col::kSecondary, status);
        ImGui::Dummy(sz);
        if (st.running) {
            ImGui::SetItemTooltip("%s · %s · underruns %llu · %u → %u Hz", st.stage.c_str(),
                                  st.provider.empty() ? "no separator" : st.provider.c_str(),
                                  static_cast<unsigned long long>(st.underruns), st.captureRate, st.renderRate);
        }
    }
    x += statusW + gap;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (pillButton(spatialLabel, false, ImVec2(spatialW, 0.0f))) a.toggleSpatial = true;
    ImGui::SetItemTooltip("Spatial mix ↔ plain stereo, seamless (Ctrl+Alt+S%s)",
                          v.hotkeySpatialOk ? "" : " — taken by another app");
    x += spatialW + gap;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (pillButton(powerLabel, !st.running && !v.busy, ImVec2(powerW, 0.0f))) a.togglePower = true;
    ImGui::SetItemTooltip("Start or stop capturing (Ctrl+Alt+P%s)", v.hotkeyPowerOk ? "" : " — taken by another app");
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetCursorStartPos().x, barBottom + 10.0f * sc));

    if (!v.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(withAlpha(col::kError, 0.6f)));
        ImGui::BeginChild("##error", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        statusDot(col::kError);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 120.0f * sc);
        ImGui::TextUnformatted(v.error.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - 90.0f * sc));
        ImGui::BeginDisabled(v.busy);
        if (pillButton("Retry", true)) a.retry = true;
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

void Ui::drawStage(app::Settings& s, const View& v, Actions& a) {
    const float sc = v.dpiScale;
    const bool speakers = renderingSpeakers(s, v);
    const ImGuiStyle& style = ImGui::GetStyle();

    sectionLabel("Stage");
    if (v.layouts && v.activeLayout >= 0 && v.activeLayout < static_cast<int>(v.layouts->items.size())) {
        ImGui::SameLine();
        ImGui::PushFont(fonts().label, kLabelSize);
        ImGui::TextDisabled("·  %s", v.layouts->items[static_cast<size_t>(v.activeLayout)].name.c_str());
        ImGui::PopFont();
    }
    const float controlsH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float side = std::max(160.0f * sc, std::min(avail.x, avail.y - controlsH));
    ImVec2 origin = ImGui::GetCursorScreenPos();
    origin.x += std::max(0.0f, (avail.x - side) * 0.5f);
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##room", ImVec2(side, side));
    const bool activated = ImGui::IsItemActivated();
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c = origin + ImVec2(side * 0.5f, side * 0.5f);
    const float roomR = side * 0.5f - 6.0f * sc;
    const float yaw = s.scene.sceneYawDeg;

    // Soft radial glow toward the centre (stacked translucent discs), then hairline rings.
    for (int k = 0; k < 8; ++k) {
        dl->AddCircleFilled(c, roomR * (1.0f - static_cast<float>(k) * 0.11f), IM_COL32(255, 255, 255, 3));
    }
    dl->AddCircle(c, roomR, col::kRing, 0, 1.0f * sc);
    for (float axis : {0.0f, 90.0f}) {
        dl->AddLine(polar(c, roomR, axis + yaw), polar(c, roomR, axis + yaw + 180.0f), IM_COL32(255, 255, 255, 12),
                    1.0f * sc);
    }
    ImFont* labelFont = fonts().label;
    const float labelSize = kLabelSize * sc;
    for (float d : {1.0f, 2.0f, 4.0f}) {
        const float r = distToRadius(d, roomR);
        dl->AddCircle(c, r, col::kRing, 0, 1.0f * sc, ImDrawFlags_StrokeCenter);
        char label[8];
        std::snprintf(label, sizeof(label), "%.0fm", d);
        dl->AddText(labelFont, labelSize, polar(c, r, 135.0f) + ImVec2(4.0f * sc, 0.0f), withAlpha(col::kSecondary, 0.7f),
                    label);
    }
    const ImVec2 frontSize = labelFont->CalcTextSizeA(labelSize, FLT_MAX, 0.0f, "FRONT");
    dl->AddText(labelFont, labelSize, ImVec2(c.x - frontSize.x * 0.5f, c.y - roomR + 8.0f * sc), col::kSecondary,
                "FRONT");

    // Listener, line-art: with head tracking the head turns while the sources stay put — what you hear.
    const float headR = std::max(10.0f * sc, roomR * 0.065f);
    const float headRot = v.headTracked ? rad(v.head.yaw) : 0.0f;
    const float cosH = std::cos(headRot), sinH = std::sin(headRot);
    auto headPt = [&](float x, float y) { return c + ImVec2(x * cosH - y * sinH, x * sinH + y * cosH); };
    const ImU32 line = IM_COL32(0xD8, 0xD8, 0xDC, 255);
    dl->AddCircle(c, headR, line, 0, 1.5f * sc);
    dl->AddEllipse(headPt(-headR - 2.5f * sc, 0.0f), ImVec2(headR * 0.18f, headR * 0.38f), line, headRot, 0, 1.5f * sc);
    dl->AddEllipse(headPt(headR + 2.5f * sc, 0.0f), ImVec2(headR * 0.18f, headR * 0.38f), line, headRot, 0, 1.5f * sc);
    dl->AddLine(headPt(-headR * 0.3f, -headR * 0.95f), headPt(0.0f, -headR * 1.45f), line, 1.5f * sc);
    dl->AddLine(headPt(headR * 0.3f, -headR * 0.95f), headPt(0.0f, -headR * 1.45f), line, 1.5f * sc);

    const int stemCount = static_cast<int>(std::min<size_t>(v.stemNames.size(), engine::kMaxStems));
    bool anySolo = false;
    for (int i = 0; i < stemCount; ++i) anySolo = anySolo || s.scene.stems[static_cast<size_t>(i)].solo;

    auto buildOrbs = [&] {
        OrbList orbs;
        if (speakers) {
            const engine::StemParams& sp = s.scene.speakers;
            const ImU32 col = IM_COL32(0xD8, 0xD8, 0xDC, 255);
            const float half = sp.widthDeg * 0.5f;
            orbs.push_back({0, sp.azimuthDeg - half, sp.distance, 0.0f, col, "L", smoothedInput_, sp.mute});
            orbs.push_back({1, sp.azimuthDeg + half, sp.distance, 0.0f, col, "R", smoothedInput_, sp.mute});
        } else {
            for (int i = 0; i < stemCount; ++i) {
                const auto idx = static_cast<size_t>(i);
                const engine::StemParams& st = s.scene.stems[idx];
                orbs.push_back({i, st.azimuthDeg, st.distance, st.widthDeg, stemColor(v.stemNames[idx], idx),
                                stemDisplayName(v.stemNames[idx], v.stemNames), smoothedLevel_[idx],
                                st.mute || (anySolo && !st.solo)});
            }
        }
        return orbs;
    };
    auto orbRadius = [&](float level) { return (7.0f * sc + roomR * 0.018f) * (0.85f + 0.5f * level); };
    auto orbPos = [&](const Orb& o) { return polar(c, distToRadius(o.dist, roomR), o.az + yaw); };
    auto orbAt = [&](const OrbList& orbs, ImVec2 m) {
        int best = -1;
        float bestD = FLT_MAX;
        for (size_t k = 0; k < orbs.size(); ++k) {
            const ImVec2 d = m - orbPos(orbs[k]);
            const float len = std::sqrt(d.x * d.x + d.y * d.y);
            if (len < orbRadius(orbs[k].level) + 8.0f * sc && len < bestD) {
                bestD = len;
                best = static_cast<int>(k);
            }
        }
        return best;
    };

    const ImVec2 mouse = ImGui::GetMousePos();
    OrbList orbs = buildOrbs();
    if (activated) {
        const int hit = orbAt(orbs, mouse);
        dragging_ = hit >= 0 ? orbs[static_cast<size_t>(hit)].index : -1;
        if (!speakers && dragging_ >= 0) {
            selected_ = dragging_;
            tab_ = Tab::Mix;
        }
    }
    if (!active) dragging_ = -1;
    if (active && dragging_ >= 0) {
        const ImVec2 d = mouse - c;
        const float az = wrapDeg(std::atan2(d.x, -d.y) * 180.0f / kPi - yaw);
        const float dist = radiusToDist(std::sqrt(d.x * d.x + d.y * d.y), roomR);
        if (speakers) {
            engine::StemParams& sp = s.scene.speakers;
            sp.azimuthDeg = wrapDeg(dragging_ == 0 ? az + sp.widthDeg * 0.5f : az - sp.widthDeg * 0.5f);
            sp.distance = dist;
        } else {
            engine::StemParams& st = s.scene.stems[static_cast<size_t>(dragging_)];
            st.azimuthDeg = az;
            st.distance = dist;
        }
        a.sceneChanged = true;
        orbs = buildOrbs();
    }

    auto drawOrb = [&](const Orb& o, bool isSelected) {
        const float r = distToRadius(o.dist, roomR);
        const float view = o.az + yaw;
        const ImVec2 p = orbPos(o);
        const ImU32 col = o.silent ? col::kIdle : o.col;
        if (o.width > 0.5f) {
            // Arc ends = where the stem's left/right content sits; the orb holds its centred content.
            dl->PathArcTo(c, r, arcAngle(view - o.width * 0.5f), arcAngle(view + o.width * 0.5f));
            dl->PathStroke(withAlpha(col, 0.35f), 1.5f * sc, ImDrawFlags_AALineEnds);
            dl->AddCircleFilled(polar(c, r, view - o.width * 0.5f), 2.0f * sc, withAlpha(col, 0.7f));
            dl->AddCircleFilled(polar(c, r, view + o.width * 0.5f), 2.0f * sc, withAlpha(col, 0.7f));
        }
        const float orbR = orbRadius(o.level);
        if (!o.silent) {
            for (int k = 3; k >= 1; --k) {
                dl->AddCircleFilled(p, orbR * (1.0f + 0.45f * static_cast<float>(k)),
                                    withAlpha(col, (0.05f + 0.10f * o.level) / static_cast<float>(k)));
            }
        }
        dl->AddCircleFilled(p, orbR, o.silent ? withAlpha(col, 0.35f) : col);
        if (isSelected) dl->AddCircle(p, orbR + 4.0f * sc, col::kWhite, 0, 1.5f * sc, ImDrawFlags_StrokeOutside);
        const ImVec2 ts = labelFont->CalcTextSizeA(labelSize, FLT_MAX, 0.0f, o.label);
        dl->AddText(labelFont, labelSize, p + ImVec2(-ts.x * 0.5f, orbR + 7.0f * sc),
                    o.silent ? col::kIdle : withAlpha(col::kText, 0.85f), o.label);
    };
    const int highlighted = speakers ? dragging_ : selected_;
    for (const Orb& o : orbs) {
        if (o.index != highlighted) drawOrb(o, false);
    }
    for (const Orb& o : orbs) {
        if (o.index == highlighted) drawOrb(o, true);
    }

    if (hovered || active) {
        const int hit = active && dragging_ >= 0 ? -1 : orbAt(orbs, mouse);
        if (hit >= 0 || dragging_ >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (hit >= 0 && !active) {
            const Orb& o = orbs[static_cast<size_t>(hit)];
            ImGui::SetTooltip("%s\n%.0f°  ·  %.1f m%s", o.label, wrapDeg(o.az), o.dist, o.silent ? "  ·  silent" : "");
        }
    }

    const float faceW = ImGui::CalcTextSize("Face front").x + style.FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(std::max(100.0f * sc, ImGui::GetContentRegionAvail().x - faceW - style.ItemSpacing.x));
    if (ImGui::SliderFloat("##yaw", &s.scene.sceneYawDeg, -180.0f, 180.0f, "rotate %.0f°", kClamp)) {
        a.sceneChanged = true;
    }
    ImGui::SetItemTooltip("Turns the whole scene around you (positive = to the right)");
    ImGui::SameLine();
    if (pillButton("Face front", false)) {
        s.scene.sceneYawDeg = 0.0f;
        a.sceneChanged = true;
    }
    ImGui::TextDisabled(speakers ? "drag a speaker to move the pair" : "drag an orb to place it · click to select");
}

void Ui::drawMix(app::Settings& s, const View& v, Actions& a) {
    bool changed = false;
    if (renderingSpeakers(s, v)) {
        sectionLabel("Virtual speakers");
        engine::StemParams& p = s.scene.speakers;
        changed |= sliderRow("Direction", &p.azimuthDeg, -180.0f, 180.0f, "%.0f°");
        changed |= sliderRow("Elevation", &p.elevationDeg, -45.0f, 90.0f, "%.0f°");
        changed |= sliderRow("Distance", &p.distance, kMinDist, kMaxDist, "%.2f m",
                             "Farther = quieter and more room sound");
        changed |= sliderRow("Spread", &p.widthDeg, 0.0f, 300.0f, "%.0f°",
                             "Angle between the left and right virtual speakers; above 180° they wrap behind you");
        changed |= sliderRow("Gain", &p.gainDb, -24.0f, 12.0f, "%+.1f dB");
        changed |= ImGui::Checkbox("Mute", &p.mute);
        ImGui::Spacing();
        ImGui::TextDisabled("switch to Stems under Sources to place instruments separately");
    } else {
        sectionLabel("Instruments");
        const int stemCount = static_cast<int>(std::min<size_t>(v.stemNames.size(), engine::kMaxStems));
        for (int i = 0; i < stemCount; ++i) {
            const auto idx = static_cast<size_t>(i);
            const char* name = stemDisplayName(v.stemNames[idx], v.stemNames);
            flowBefore(i == 0, name);
            ImGui::PushID(i);
            if (chip(name, i == selected_)) selected_ = i;
            ImGui::PopID();
        }
        if (stemCount == 0) return;
        ImGui::Spacing();

        const auto sel = static_cast<size_t>(selected_);
        engine::StemParams& p = s.scene.stems[sel];
        statusDot(stemColor(v.stemNames[sel], sel));
        ImGui::SameLine();
        ImGui::PushFont(fonts().light, kTitleSize * 0.7f);
        ImGui::TextUnformatted(stemDisplayName(v.stemNames[sel], v.stemNames));
        ImGui::PopFont();
        thinMeter(smoothedLevel_[sel]);
        ImGui::Spacing();
        changed |= sliderRow("Azimuth", &p.azimuthDeg, -180.0f, 180.0f, "%.0f°", "0° ahead, +90° right, ±180° behind");
        changed |= sliderRow("Elevation", &p.elevationDeg, -45.0f, 90.0f, "%.0f°");
        changed |= sliderRow("Distance", &p.distance, kMinDist, kMaxDist, "%.2f m",
                             "Farther = quieter and more room sound");
        changed |= sliderRow("Width", &p.widthDeg, 0.0f, 300.0f, "%.0f°",
                             "Spread of the instrument's left/right content; centred content stays at the orb");
        changed |= sliderRow("Gain", &p.gainDb, -24.0f, 12.0f, "%+.1f dB");
        changed |= ImGui::Checkbox("Mute", &p.mute);
        ImGui::SameLine(0.0f, 24.0f * v.dpiScale);
        changed |= ImGui::Checkbox("Solo", &p.solo);
    }
    if (changed) a.sceneChanged = true;
}

void Ui::hotkeyButton(const char* id, const app::Hotkey& hk, bool ok, int target, const View& v, Actions& a) {
    ImGui::PushID(id);
    const bool capturing = v.capturingHotkey == target;
    std::string label = capturing ? "press keys…" : hk.empty() ? "set hotkey" : app::hotkeyText(hk);
    label += "###hk";
    if (capturing) {
        if (pillButton(label.c_str(), true)) a.cancelCapture = true;
        ImGui::SetItemTooltip("Press a combination with Ctrl, Alt, Shift or Win. Esc cancels, Backspace clears.");
    } else {
        if (!ok && !hk.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col::kWarn));
        if (pillButton(label.c_str(), false)) a.captureHotkey = target;
        if (!ok && !hk.empty()) {
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Already used by another app — pick another combination");
        }
    }
    if (!hk.empty() && !capturing) {
        ImGui::SameLine(0.0f, 4.0f * v.dpiScale);
        if (pillButton("×", false)) a.clearHotkey = target;
        ImGui::SetItemTooltip("Remove hotkey");
    }
    ImGui::PopID();
}

void Ui::drawLayouts(app::Settings& s, const View& v, Actions& a) {
    if (!v.layouts) return;
    const float sc = v.dpiScale;
    const auto& items = v.layouts->items;

    sectionLabel("Quick menu");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("hold the combo anywhere to open a see-through layout picker; click a layout, press 1-9, "
                        "or let go while pointing at one");
    ImGui::PopTextWrapPos();
    hotkeyButton("quick", s.quickMenuHotkey, v.quickMenuHotkeyOk, kCaptureQuickMenu, v, a);
    if (!v.quickMenuAvailable) wrappedText(col::kWarn, "The overlay could not be created on this system.");
    ImGui::SetItemTooltip("Games in exclusive fullscreen may hide the overlay; per-layout hotkeys still work there.");
    ImGui::Spacing();
    ImGui::Spacing();

    sectionLabel("Layouts");
    const float applyW = ImGui::CalcTextSize("Apply").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable("##layouts", 2, tableFlags)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed);
        for (size_t i = 0; i < items.size(); ++i) {
            const app::Layout& l = items[i];
            const int idx = static_cast<int>(i);
            ImGui::PushID(idx);
            const bool active = idx == v.activeLayout;
            ImGui::TableNextRow(0, ImGui::GetFrameHeight() + 10.0f * sc);

            ImGui::TableSetColumnIndex(0);
            ImGui::Dummy(ImVec2(0.0f, 2.0f * sc));
            if (renaming_ == idx) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##rename", renameBuf_, sizeof(renameBuf_),
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) &&
                    renameBuf_[0] != '\0') {
                    a.renameLayout = idx;
                    a.renameTo = renameBuf_;
                    renaming_ = -1;
                }
                if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) renaming_ = -1;
            } else {
                ImGui::AlignTextToFramePadding();
                if (i < 9) {
                    ImGui::TextDisabled("%zu", i + 1);
                    ImGui::SameLine();
                }
                ImGui::TextUnformatted(l.name.c_str());
                if (active) {
                    ImGui::SameLine();
                    statusDot(col::kOn);
                }
                if (l.builtIn()) ImGui::SetItemTooltip("Built-in: placement adapts to the model's instruments");
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::Dummy(ImVec2(0.0f, 2.0f * sc));
            hotkeyButton("hk", l.hotkey, i < v.layoutHotkeyOk.size() ? v.layoutHotkeyOk[i] : true, idx, v, a);
            ImGui::SameLine();
            if (pillButton("Apply", active, ImVec2(applyW, 0.0f))) a.applyLayout = idx;
            ImGui::SameLine();
            ImGui::BeginDisabled(l.builtIn());
            if (pillButton("···", false)) ImGui::OpenPopup("more");
            ImGui::EndDisabled();
            if (ImGui::BeginPopup("more")) {
                if (ImGui::MenuItem("Update from current placement")) a.updateLayout = idx;
                if (ImGui::MenuItem("Rename")) {
                    renaming_ = idx;
                    std::snprintf(renameBuf_, sizeof(renameBuf_), "%s", l.name.c_str());
                }
                if (ImGui::MenuItem("Delete")) a.deleteLayout = idx;
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();

    sectionLabel("Save current placement");
    const float saveW = ImGui::CalcTextSize("Save layout").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(std::max(120.0f * sc, ImGui::GetContentRegionAvail().x - saveW -
                                                      ImGui::GetStyle().ItemSpacing.x));
    const bool enter = ImGui::InputTextWithHint("##newname", "name, e.g. Movie night", newLayoutName_,
                                                sizeof(newLayoutName_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(newLayoutName_[0] == '\0');
    if (pillButton("Save layout", true) || (enter && newLayoutName_[0] != '\0')) {
        a.saveLayoutAs = newLayoutName_;
        newLayoutName_[0] = '\0';
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (pillButton("Reset placement", false)) a.resetScene = true;
    ImGui::SetItemTooltip("Everything centred, 1 m away, gains at 0 dB");
}

void Ui::drawRoom(app::Settings& s, const View& v, Actions& a) {
    sectionLabel("Room");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("reflections and reverb that place sounds outside your head; distance sets how much "
                        "room each instrument gets");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    struct RoomDef {
        const char* label;
        engine::RoomPreset room;
        const char* tip;
    };
    static const RoomDef rooms[] = {
        {"Off", engine::RoomPreset::Off, "Dry: direction only. Sounds may feel inside your head."},
        {"Small room", engine::RoomPreset::SmallRoom, "Tight, close walls."},
        {"Studio", engine::RoomPreset::Studio, "Controlled, short ambience. A good default."},
        {"Living room", engine::RoomPreset::LivingRoom, "Warmer and a bit longer."},
        {"Concert hall", engine::RoomPreset::Hall, "Big and spacious; best for live and orchestral music."},
    };
    bool first = true;
    for (const RoomDef& r : rooms) {
        flowBefore(first, r.label);
        first = false;
        if (chip(r.label, s.scene.room == r.room) && s.scene.room != r.room) {
            s.scene.room = r.room;
            a.sceneChanged = true;
        }
        ImGui::SetItemTooltip("%s", r.tip);
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(s.scene.room == engine::RoomPreset::Off);
    if (sliderRow("Amount", &s.scene.roomWetDb, -30.0f, 0.0f, "%.0f dB")) a.sceneChanged = true;
    ImGui::EndDisabled();
    (void)v;
}

void Ui::drawHeadphones(app::Settings& s, const View& v, Actions& a) {
    auto nameOf = [&](const std::wstring& id) -> std::string {
        if (id.empty()) return "(none)";
        for (const audio::DeviceInfo& d : v.devices) {
            if (d.id == id) return d.name;
        }
        return "(unavailable)";
    };

    sectionLabel("Devices");
    rowLabel("Headphones");
    if (ImGui::BeginCombo("##hp", nameOf(s.headphoneDeviceId).c_str())) {
        for (const audio::DeviceInfo& d : v.devices) {
            ImGui::PushID(logx::narrow(d.id).c_str());
            const std::string label = d.name + (d.isDefault ? "  (default)" : "");
            const bool isSel = d.id == s.headphoneDeviceId;
            if (ImGui::Selectable(label.c_str(), isSel) && !isSel) {
                s.headphoneDeviceId = d.id;
                a.configChanged = true;
                a.headphonesChanged = true;
            }
            if (isSel) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Where you listen. The spatial mix is played here.");
    rowLabel("Silent");
    if (ImGui::BeginCombo("##sink", nameOf(s.sinkDeviceId).c_str())) {
        for (const audio::DeviceInfo& d : v.devices) {
            ImGui::PushID(logx::narrow(d.id).c_str());
            const bool isHeadphones = d.id == s.headphoneDeviceId;  // sink == headphones would feed back
            const std::string label = d.name + (isHeadphones ? "  (your headphones)" : "");
            const bool isSel = d.id == s.sinkDeviceId;
            if (ImGui::Selectable(label.c_str(), isSel, isHeadphones ? ImGuiSelectableFlags_Disabled : 0) && !isSel) {
                s.sinkDeviceId = d.id;
                a.configChanged = true;
            }
            if (isSel) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Apps are routed into this device and captured from it.");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("the silent device must not be connected to speakers you can hear");
    ImGui::PopTextWrapPos();
    if (s.sinkDeviceId.empty()) {
        wrappedText(col::kError, "No silent device available. Install a virtual audio cable (e.g. VB-CABLE) or use a "
                                 "monitor/HDMI output with nothing connected to it.");
    }
    if (pillButton("Refresh devices", false)) a.refreshDevices = true;

    ImGui::Spacing();
    ImGui::Spacing();
    sectionLabel("Your headphones");
    const audio::EndpointTraits& t = v.headphoneTraits;
    if (!s.headphoneDeviceId.empty()) {
        const char* conn = "unknown connection";
        switch (t.connection) {
            case audio::Connection::Wired: conn = "wired"; break;
            case audio::Connection::Usb: conn = "usb"; break;
            case audio::Connection::Bluetooth: conn = "bluetooth"; break;
            case audio::Connection::Unknown: break;
        }
        std::string line = conn;
        if (!t.model.empty()) line += "  ·  " + t.model;
        if (t.sampleRate != 0) line += "  ·  " + std::to_string(t.sampleRate) + " Hz " + std::to_string(t.channels) + " ch";
        ImGui::TextUnformatted(line.c_str());
    }
    if (t.handsFree) {
        wrappedText(col::kError, "This is the Bluetooth hands-free (headset/mic) mode: mono, low quality, spatial "
                                 "audio cannot work. Choose the “… Stereo” device, or close apps using this "
                                 "headset's microphone.");
    }
    if (t.model.starts_with("AirPods")) {
        wrappedText(col::kWarn, "Windows can't read AirPods' motion sensors directly. Head tracking needs a relay that "
                                "sends OpenTrack UDP to this PC — see README “AirPods head tracking”.");
    }

    ImGui::Spacing();
    sectionLabel("Head tracking");
    app::TrackingProfile& p = s.tracking;
    static constexpr const char* kUdpLabel = "Phone, webcam or OpenTrack (UDP)";
    auto sensorLabel = [](const track::HidHeadSensor::Info& info) { return "Headphone sensor: " + info.name; };
    // An empty path means "first sensor found", matching HeadTracker::Config::sensorPath.
    auto isProfileSensor = [&](const track::HidHeadSensor::Info& info) {
        return p.sensorPath.empty() ? &info == &v.sensors.front() : info.path == p.sensorPath;
    };
    std::string current = "Off";
    if (p.source == track::Source::OpenTrackUdp) {
        current = kUdpLabel;
    } else if (p.source == track::Source::HeadphoneSensor) {
        current = "Headphone sensor (not connected)";
        for (const track::HidHeadSensor::Info& info : v.sensors) {
            if (isProfileSensor(info)) {
                current = sensorLabel(info);
                break;
            }
        }
    }
    rowLabel("Source");
    if (ImGui::BeginCombo("##track", current.c_str())) {
        if (ImGui::Selectable("Off", p.source == track::Source::None) && p.source != track::Source::None) {
            p.source = track::Source::None;
            a.trackingChanged = true;
        }
        if (ImGui::Selectable(kUdpLabel, p.source == track::Source::OpenTrackUdp) &&
            p.source != track::Source::OpenTrackUdp) {
            p.source = track::Source::OpenTrackUdp;
            a.trackingChanged = true;
        }
        for (size_t i = 0; i < v.sensors.size(); ++i) {
            const track::HidHeadSensor::Info& info = v.sensors[i];
            ImGui::PushID(static_cast<int>(i));
            const bool isSel = p.source == track::Source::HeadphoneSensor && isProfileSensor(info);
            if (ImGui::Selectable(sensorLabel(info).c_str(), isSel) && !isSel) {
                p.source = track::Source::HeadphoneSensor;
                p.sensorPath = info.path;
                a.trackingChanged = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Keeps sounds fixed in the room when you turn your head. Saved per headphones.");

    if (p.source == track::Source::OpenTrackUdp) {
        int port = p.udpPort;
        rowLabel("UDP port");
        if (ImGui::InputInt("##port", &port, 0, 0)) {
            p.udpPort = static_cast<uint16_t>(std::clamp(port, 1, 65535));
            a.settingsChanged = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) a.trackingChanged = true;  // rebind once typing is done
        std::string hint;
        if (v.localIPv4.empty()) {
            hint = "no network connection found: only a relay on this PC can reach port " + std::to_string(p.udpPort);
        } else {
            hint = "send OpenTrack UDP to ";
            for (size_t i = 0; i < v.localIPv4.size(); ++i) hint += (i ? " or " : "") + v.localIPv4[i];
            hint += " : " + std::to_string(p.udpPort);
        }
        wrappedText(col::kSecondary, hint.c_str());
    }

    if (p.source != track::Source::None) {
        bool invert = false;
        invert |= ImGui::Checkbox("Invert yaw", &p.invertYaw);
        ImGui::SameLine();
        invert |= ImGui::Checkbox("pitch", &p.invertPitch);
        ImGui::SameLine();
        invert |= ImGui::Checkbox("roll", &p.invertRoll);
        if (invert) a.trackingChanged = true;

        // Restart only when the drag ends; the tracker reads its config at start.
        if (sliderRow("Prediction", &p.predictionMs, 0.0f, 250.0f, "%.0f ms",
                      "Turns the sound a little ahead of your head to hide Bluetooth/relay delay")) {
            a.settingsChanged = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) a.trackingChanged = true;

        if (pillButton("Recenter", true)) a.recenter = true;
        ImGui::SetItemTooltip("Makes the direction you are facing now the front");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Ctrl+Alt+C%s", v.hotkeyRecenterOk ? "" : "  (taken by another app)");

        const track::TrackerStatus& ts = v.tracker;
        const track::Orientation& o = v.headTracked ? v.head : ts.raw;
        if (!ts.error.empty()) {
            wrappedText(col::kError, ts.error.c_str());
        } else if (ts.receiving) {
            statusDot(col::kOn);
            ImGui::SameLine();
            ImGui::Text("%.0f Hz  ·  yaw %+.0f°  pitch %+.0f°  roll %+.0f°", ts.rateHz, o.yaw, o.pitch, o.roll);
        } else {
            statusDot(col::kWarn);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", ts.running ? "waiting for data" : "not running");
        }
    }
}

void Ui::drawSources(app::Settings& s, const View& v, Actions& a) {
    sectionLabel("Mode");
    struct ModeDef {
        const char* label;
        engine::Mode mode;
        const char* tip;
    };
    static const ModeDef modes[] = {
        {"Adaptive", engine::Mode::Adaptive,
         "Stems for music; switches itself to low-latency virtual speakers while a game or video plays (voices "
         "stay in front), or when the GPU is too busy to separate in time."},
        {"Stems (ML)", engine::Mode::Stems,
         "Always separates instruments and places each in space. About 1 s delay."},
        {"Virtual speakers", engine::Mode::VirtualSpeakers,
         "Never separates: the mix through two virtual speakers, centred voices in front. About 75 ms."},
    };
    bool first = true;
    for (const ModeDef& m : modes) {
        flowBefore(first, m.label);
        first = false;
        if (chip(m.label, s.mode == m.mode) && s.mode != m.mode) {
            s.mode = m.mode;
            a.configChanged = true;
        }
        ImGui::SetItemTooltip("%s", m.tip);
    }
    if (s.mode == engine::Mode::Adaptive) {
        ImGui::Spacing();
        ImGui::TextDisabled("prefer low latency over separation while…");
        // Read every second by the App; no engine restart needed.
        if (ImGui::Checkbox("a game or fullscreen app is in front", &s.adaptiveFullscreen)) a.settingsChanged = true;
        if (ImGui::Checkbox("a video is playing (video apps, YouTube, Netflix, Twitch…)", &s.adaptiveVideo)) {
            a.settingsChanged = true;
        }
        ImGui::SetItemTooltip("Lip sync needs under ~100 ms, which separation can't reach. Voices are mixed to the "
                              "centre, so the low-latency path still keeps dialogue in front. Edit the list of "
                              "video apps and sites under Advanced.");
        if (ImGui::Checkbox("the GPU can't keep up", &s.adaptiveGpuFallback)) a.configChanged = true;

        const engine::Status& st = v.status;
        std::string now;
        if (!st.running) {
            now = "power is off";
        } else if (st.activeMode == engine::Mode::Stems) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "now: stems · hop %.2f s · gpu %.0f%%", st.hopSec, st.gpuLoad * 100);
            now = buf;
        } else {
            now = "now: low latency" + (st.adaptiveReason.empty() ? std::string() : " · " + st.adaptiveReason);
        }
        ImGui::TextDisabled("%s", now.c_str());
    }

    ImGui::Spacing();
    ImGui::Spacing();
    sectionLabel("Capture");
    if (chip("Whole system", s.capture == engine::CaptureMode::WholeSystem) &&
        s.capture != engine::CaptureMode::WholeSystem) {
        s.capture = engine::CaptureMode::WholeSystem;
        a.configChanged = true;
    }
    ImGui::SetItemTooltip("Everything you hear: the default output is pointed at the silent device.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.perAppRouting);
    if (chip("Selected apps", s.capture == engine::CaptureMode::SelectedApps) &&
        s.capture != engine::CaptureMode::SelectedApps) {
        s.capture = engine::CaptureMode::SelectedApps;
        a.configChanged = true;
        a.refreshApps = true;
    }
    ImGui::EndDisabled();
    tooltipEvenIfDisabled(v.perAppRouting ? "Only the checked apps go through Spatiall; everything else plays normally."
                                          : "Per-app output routing is not available on this Windows build.");
    if (s.capture == engine::CaptureMode::WholeSystem) return;

    ImGui::Spacing();
    struct Row {
        std::string exe;  // lower-case key
        std::string label;
        bool active;
    };
    std::vector<Row> rows;
    for (const audio::AppSession& ses : v.sessions) {
        std::string exe = lower(ses.exe);
        if (exe.empty()) continue;
        auto it = std::find_if(rows.begin(), rows.end(), [&](const Row& r) { return r.exe == exe; });
        if (it != rows.end()) {
            it->active = it->active || ses.active;
            continue;
        }
        std::string label = ses.displayName.empty() || lower(ses.displayName) == exe
                                ? ses.exe
                                : ses.displayName + "  (" + ses.exe + ")";
        rows.push_back({std::move(exe), std::move(label), ses.active});
    }
    for (const std::string& exe : s.appExes) {
        if (std::none_of(rows.begin(), rows.end(), [&](const Row& r) { return r.exe == exe; })) {
            rows.push_back({exe, exe + "  (not running)", false});
        }
    }
    if (rows.empty()) ImGui::TextDisabled("no app is playing audio · start playback, then refresh");
    for (const Row& r : rows) {
        ImGui::PushID(r.exe.c_str());
        auto it = std::find(s.appExes.begin(), s.appExes.end(), r.exe);
        bool on = it != s.appExes.end();
        if (ImGui::Checkbox(r.label.c_str(), &on)) {
            if (on) {
                s.appExes.push_back(r.exe);
            } else {
                s.appExes.erase(it);
            }
            a.configChanged = true;
        }
        if (r.active) {
            ImGui::SameLine();
            statusDot(col::kOn);
        }
        ImGui::PopID();
    }
    if (pillButton("Refresh apps", false)) a.refreshApps = true;
    if (s.appExes.empty()) wrappedText(col::kWarn, "Check at least one app.");
}

void Ui::drawAdvanced(app::Settings& s, const View& v, Actions& a) {
    const float sc = v.dpiScale;
    const bool stems = s.mode != engine::Mode::VirtualSpeakers;

    sectionLabel("Separation");
    ImGui::BeginDisabled(!stems);
    const std::string current = s.modelFile.empty() ? std::string("(none)") : logx::narrow(s.modelFile);
    rowLabel("Model");
    if (ImGui::BeginCombo("##model", current.c_str())) {
        for (const std::wstring& m : v.models) {
            const std::string name = logx::narrow(m);
            const bool isSel = m == s.modelFile;
            if (ImGui::Selectable(name.c_str(), isSel) && !isSel) {
                s.modelFile = m;
                a.configChanged = true;
            }
            if (isSel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("*_6s = 6 stems (adds guitar and piano) · *_4s = 4-second live window (fast)");
    if (v.models.empty()) {
        const std::string msg = "No .onnx models found in " + logx::narrow(v.modelsDir);
        wrappedText(col::kError, msg.c_str());
    }
    // Restart only when the drag ends; dragging would otherwise reload the model every frame.
    if (sliderRow("Look-ahead", &s.lookaheadSec, 0.1f, 2.0f, "%.2f s",
                  "Future audio the separator may look at. More = cleaner separation but more delay.")) {
        a.settingsChanged = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) a.configChanged = true;
    if (sliderRow("Hop", &s.hopSec, 0.2f, 1.0f, "%.2f s",
                  "Shortest separator step = lowest latency. It grows automatically (up to 0.8 s) when the GPU is "
                  "busy, and shrinks back when it isn't.")) {
        a.settingsChanged = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) a.configChanged = true;
    const double inferMs = v.status.inferenceMs;
    const double hopNow = v.status.running && v.status.hopSec > 0.0 ? v.status.hopSec : s.hopSec;
    ImGui::TextDisabled("hop now %.2f s · estimated delay %.0f ms (look-ahead + hop + inference %.0f ms)", hopNow,
                        (s.lookaheadSec + hopNow) * 1000.0 + inferMs, inferMs);
    ImGui::EndDisabled();

    ImGui::Spacing();
    sectionLabel("Output");
    if (sliderRow("Master", &s.scene.masterGainDb, -24.0f, 12.0f, "%+.1f dB")) a.sceneChanged = true;
    if (ImGui::Checkbox("Turn power on when Spatiall starts", &s.autostartPower)) a.settingsChanged = true;

    ImGui::Spacing();
    sectionLabel("Video detection (adaptive)");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("comma-separated: player.exe, text in a browser tab title (e.g. - YouTube), or !text to "
                        "never count as video (e.g. !YouTube Music)");
    ImGui::PopTextWrapPos();
    // Mirror the setting into the edit buffer except while the user is typing in it.
    static char markers[1024];
    if (ImGui::GetActiveID() != ImGui::GetID("##videomarkers")) {
        std::snprintf(markers, sizeof(markers), "%s", s.videoMarkers.c_str());
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f * sc);
    if (ImGui::InputTextMultiline("##videomarkers", markers, sizeof(markers),
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f))) {
        s.videoMarkers = markers;
        a.settingsChanged = true;
    }
    ImGui::PopStyleVar();
    if (pillButton("Reset list", false)) {
        s.videoMarkers = app::kDefaultVideoMarkers;
        a.settingsChanged = true;
    }

    ImGui::Spacing();
    sectionLabel("Log");
    if (pillButton("Open log folder", false)) a.openLogFolder = true;
    const std::vector<std::string> lines = logx::recent(200);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(col::kBg));
    ImGui::BeginChild("##log", ImVec2(0.0f, 200.0f * sc), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(fonts().label, kLabelSize);
    for (const std::string& l : lines) ImGui::TextDisabled("%s", l.c_str());
    ImGui::PopFont();
    // Follow the tail unless the user scrolled up.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

}  // namespace ui
