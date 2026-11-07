#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/DiligentHud.hpp"

#include <cmath>
#include <iterator>
#include <string>
#include "ArchViz/HudShell.hpp"
#include "ArchViz/SunStudyPreview.hpp"

#include <imgui.h>

namespace geomsrv::archviz {
namespace {
ImU32 Colour (unsigned hex)
{
    return IM_COL32 ((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, 255);
}
// The tint's colour at `hours`: SunStudyRampColour, the shader's SunWarm.
ImU32 SunColour (float hours, std::span<const overlaylayers::ColourStop> ramp)
{
    const uint32_t rgba = SunStudyRampColour (ramp, hours);
    return IM_COL32 ((rgba >> 24) & 255, (rgba >> 16) & 255, (rgba >> 8) & 255, 255);
}
float Snapped (float value)
{
    return std::round (value * 4.0f) / 4.0f;
}
} // namespace

double SunStudyClockMinute (double x, double y)
{
    double angle = std::atan2 (y, x) + 1.57079632679489661923;
    if (angle < 0.0)
        angle += 6.28318530717958647692;
    return angle * (1440.0 / 6.28318530717958647692);
}

bool DrawSunStudyRange (const char* id, float& from, float& to, float minimum, float maximum, bool duration)
{
    const ImVec2 start = ImGui::GetCursorScreenPos ();
    const float height = ImGui::GetFrameHeight () + 4.0f;
    const float width = ImGui::GetContentRegionAvail ().x;
    const float inset = 7.0f;
    const float span = std::max (maximum - minimum, 0.25f);
    from = std::clamp (from, minimum, maximum);
    to = std::clamp (to, from, maximum);
    const auto position = [&] (float value) {
        return start.x + inset + (width - 2 * inset) * (value - minimum) / span;
    };
    ImGui::InvisibleButton (id, ImVec2 (width, height));
    const ImGuiID handleId = ImGui::GetID (id);
    if (ImGui::IsItemActivated ()) {
        const float mouse = ImGui::GetIO ().MousePos.x;
        ImGui::GetStateStorage ()->SetInt (
            handleId, std::abs (mouse - position (from)) <= std::abs (mouse - position (to)) ? 0 : 1);
    }
    bool changed = false;
    if (ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left)) {
        const float value = std::clamp (Snapped (minimum + (ImGui::GetIO ().MousePos.x - start.x - inset) /
                                                               std::max (width - 2 * inset, 1.0f) * span),
                                        minimum, maximum);
        float& chosen = ImGui::GetStateStorage ()->GetInt (handleId) == 0 ? from : to;
        const float next = &chosen == &from ? std::min (value, to) : std::max (value, from);
        changed = chosen != next;
        chosen = next;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    draw->AddRectFilled (start, ImVec2 (start.x + width, start.y + height), IM_COL32 (76, 76, 76, 255), 4.0f);
    const float left = position (from), right = position (to);
    draw->AddRectFilled (ImVec2 (left, start.y), ImVec2 (right, start.y + height), IM_COL32 (109, 102, 90, 255), 4.0f);
    for (const float x : { left, right })
        draw->AddRectFilled (ImVec2 (x - 3, start.y + 3), ImVec2 (x + 3, start.y + height - 3),
                             IM_COL32 (226, 226, 226, 255), 3.0f);
    const std::string text = SunStudyValueText (from, duration) + " to " + SunStudyValueText (to, duration);
    const ImVec2 size = ImGui::CalcTextSize (text.c_str ());
    draw->AddText (ImVec2 (start.x + (width - size.x) * 0.5f, start.y + (height - size.y) * 0.5f),
                   IM_COL32 (255, 216, 158, 255), text.c_str ());
    return changed;
}

void DrawSunStudyRampBar (ImDrawList* draw, ImVec2 start, ImVec2 size, int bands, const float* edges, float blueBelow,
                          unsigned lowSunHex, bool labels, std::span<const overlaylayers::ColourStop> ramp)
{
    const float top = kSunRampTop;
    const bool stepped = edges != nullptr && bands >= kSunBandsMin && bands <= kSunBandsMax;
    const auto position = [&] (float hours) { return start.x + size.x * hours / top; };
    if (stepped) {
        for (int band = 0; band < bands; ++band) {
            const float lo = band > 0 ? edges[band - 1] : 0.0f;
            const float hi = band < bands - 1 ? edges[band] : top;
            draw->AddRectFilled (ImVec2 (position (lo), start.y), ImVec2 (position (hi), start.y + size.y),
                                 SunColour (SunStudyBandCentre (band, bands, edges), ramp));
        }
        for (int i = 0; i < bands - 1; ++i)
            draw->AddLine (ImVec2 (position (edges[i]), start.y), ImVec2 (position (edges[i]), start.y + size.y),
                           IM_COL32 (40, 40, 40, 200), 1.0f);
    }
    else {
        for (int segment = 0; segment < 40; ++segment) {
            const ImVec2 lo (start.x + size.x * segment / 40.0f, start.y);
            const ImVec2 hi (start.x + size.x * (segment + 1) / 40.0f, start.y + size.y);
            const ImU32 a = SunColour (float (segment) * 0.25f, ramp),
                        b = SunColour (float (segment + 1) * 0.25f, ramp);
            draw->AddRectFilledMultiColor (lo, hi, a, b, b, a);
        }
    }
    // Solid, as the surface draws it: below the cutoff the warm ramp is not shown.
    if (blueBelow > 0.0f)
        draw->AddRectFilled (start, ImVec2 (position ((std::min) (blueBelow, top)), start.y + size.y),
                             Colour (lowSunHex));
    if (!stepped || !labels)
        return;
    // Each cell's start hour, centred in it: dark ink on a light cell, white on
    // a dark one. Text wider than its cell is left out, never clipped.
    for (int band = 0; band < bands; ++band) {
        const float lo = band > 0 ? edges[band - 1] : 0.0f;
        const float hi = band < bands - 1 ? edges[band] : top;
        const std::string text = SunStudyBandLabel (band, bands, edges);
        const ImVec2 extent = ImGui::CalcTextSize (text.c_str ());
        const float left = position (lo), right = position (hi);
        if (extent.x + 4.0f > right - left || extent.y > size.y + 2.0f)
            continue;
        const bool blue = blueBelow > 0.0f && 0.5f * (lo + hi) < blueBelow;
        const ImVec4 fill = ImGui::ColorConvertU32ToFloat4 (
            blue ? Colour (lowSunHex) : SunColour (SunStudyBandCentre (band, bands, edges), ramp));
        const float luminance = 0.2126f * fill.x + 0.7152f * fill.y + 0.0722f * fill.z;
        draw->AddText (ImVec2 (0.5f * (left + right - extent.x), start.y + 0.5f * (size.y - extent.y)),
                       luminance > 0.55f ? IM_COL32 (30, 30, 30, 255) : IM_COL32_WHITE, text.c_str ());
    }
}

void DrawSunStudyGradient (bool blue, float& threshold, int bands, float* edges, unsigned lowSunHex,
                           std::span<const overlaylayers::ColourStop> ramp)
{
    constexpr float top = kSunRampTop;
    const bool stepped = edges != nullptr && bands >= kSunBandsMin && bands <= kSunBandsMax;
    threshold = std::clamp (threshold, 0.0f, top);
    // The grips stand ABOVE the bar, the blue box below it, so the two never
    // sit on the same pixels.
    const float gripRoom = stepped ? 10.0f : 0.0f;
    const ImVec2 widget = ImGui::GetCursorScreenPos ();
    const ImVec2 start (widget.x, widget.y + gripRoom);
    const float width = ImGui::GetContentRegionAvail ().x;
    const float height = ImGui::GetFrameHeight ();
    const auto position = [&] (float hours) { return start.x + width * hours / top; };
    const auto hoursAt = [&] (float x) {
        return std::clamp (Snapped ((x - start.x) / std::max (width, 1.0f) * top), 0.0f, top);
    };
    ImGui::InvisibleButton ("##sun-gradient", ImVec2 (width, gripRoom + height + (blue ? 12.0f : 0.0f)));
    // Which handle the press took: -1 the blue box, 0.. an edge, -2 nothing.
    const ImGuiID handleId = ImGui::GetID ("##sun-gradient-handle");
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    if (ImGui::IsItemActivated ()) {
        int handle = blue ? -1 : -2;
        if (stepped && (mouse.y < start.y + height || !blue)) {
            float best = 1.0e9f;
            for (int i = 0; i < bands - 1; ++i)
                if (std::abs (mouse.x - position (edges[i])) < best) {
                    best = std::abs (mouse.x - position (edges[i]));
                    handle = i;
                }
        }
        ImGui::GetStateStorage ()->SetInt (handleId, handle);
    }
    if (ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left)) {
        const int handle = ImGui::GetStateStorage ()->GetInt (handleId, -2);
        if (handle == -1)
            threshold = hoursAt (mouse.x);
        else if (stepped && handle >= 0 && handle < bands - 1) {
            // Held between its neighbours, never crossing them.
            const float lo = handle > 0 ? edges[handle - 1] + kSunBandGap : kSunBandGap;
            const float hi = handle < bands - 2 ? edges[handle + 1] - kSunBandGap : top - kSunBandGap;
            edges[handle] = std::clamp (hoursAt (mouse.x), lo, hi);
        }
    }
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    DrawSunStudyRampBar (draw, start, ImVec2 (width, height), stepped ? bands : 0, edges, blue ? threshold : -1.0f,
                         lowSunHex, true, ramp);
    if (stepped)
        for (int i = 0; i < bands - 1; ++i) {
            const float x = position (edges[i]);
            draw->AddTriangleFilled (ImVec2 (x - 5, widget.y + 1), ImVec2 (x + 5, widget.y + 1),
                                     ImVec2 (x, start.y - 1), IM_COL32 (226, 226, 226, 255));
        }
    if (blue) {
        const float edge = position (threshold);
        draw->AddRectFilled (ImVec2 (edge - 5, start.y + height - 3), ImVec2 (edge + 5, start.y + height + 9),
                             Colour (lowSunHex), 2.0f);
        draw->AddRect (ImVec2 (edge - 5, start.y + height - 3), ImVec2 (edge + 5, start.y + height + 9), IM_COL32_WHITE,
                       2.0f);
    }
    if (ImGui::IsItemHovered ()) {
        std::string tip;
        if (stepped) {
            // The bands' hour ranges: the words for cells too narrow to label.
            for (int band = 0; band < bands; ++band) {
                const float lo = band > 0 ? edges[band - 1] : 0.0f;
                tip += (band > 0 ? "  " : "") + SunStudyValueText (lo) +
                       (band < bands - 1 ? "-" + SunStudyValueText (edges[band]) : "+");
            }
            tip += std::string (SunLegendUnit () == SunValueUnit::Hours ? " h" : "") +
                   "\nDrag a grip above the bar to move a step's edge";
        }
        if (blue)
            tip += std::string (tip.empty () ? "" : "; ") + "drag the blue box: direct sun below " +
                   SunStudyClock (threshold, true);
        if (!tip.empty ())
            hudshell::Tip (tip, hudshell::TipSide::Above);
    }
    if (!stepped)
        ImGui::TextDisabled ("%s", SunLegendUnit () == SunValueUnit::Hours
                                       ? "0:00h                            9:00h+"
                                       : "0%                                   100%");
}

void DrawSunStudyInspectControl (int& mode, const char* first, const char* second)
{
    ImGui::TextUnformatted ("Mouse hover");
    const char* labels[] = { "Off##suninspect", "Cursor##suninspect", "Panel##suninspect" };
    const float width = (ImGui::GetContentRegionAvail ().x - 2.0f * ImGui::GetStyle ().ItemSpacing.x) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        if (i != 0)
            ImGui::SameLine ();
        ImGui::PushStyleColor (ImGuiCol_Button,
                               ImGui::GetStyleColorVec4 (mode == i ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
        if (ImGui::Button (labels[i], ImVec2 (width, 0)))
            mode = i;
        ImGui::PopStyleColor ();
    }
    if (mode != 2)
        return;
    // Fixed viewport prevents both height and width from following hover text.
    ImGui::BeginChild ("##sun-reading", ImVec2 (0, 3.0f * ImGui::GetTextLineHeightWithSpacing ()), ImGuiChildFlags_None,
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextWrapped ("%s", first != nullptr && first[0] != 0 ? first : "hover the model");
    if (second != nullptr && second[0] != 0) {
        ImGui::PushStyleColor (ImGuiCol_Text, ImGui::GetStyleColorVec4 (ImGuiCol_TextDisabled));
        ImGui::TextWrapped ("%s", second);
        ImGui::PopStyleColor ();
    }
    ImGui::EndChild ();
}
void SunStudyNote (const char* line, const char* detail)
{
    const float width = ImGui::GetContentRegionAvail ().x;
    std::string shown = line != nullptr ? line : "";
    const bool cut = ImGui::CalcTextSize (shown.c_str ()).x > width;
    if (cut) {
        while (!shown.empty () && ImGui::CalcTextSize ((shown + "...").c_str ()).x > width)
            shown.pop_back ();
        shown += "...";
    }
    const bool more = detail != nullptr && detail[0] != 0;
    ImGui::TextDisabled ("%s", shown.c_str ());
    if ((cut || more) && ImGui::BeginItemTooltip ()) {
        ImGui::PushTextWrapPos (ImGui::GetFontSize () * 30.0f);
        ImGui::TextUnformatted (line);
        if (more) {
            ImGui::Spacing ();
            ImGui::TextUnformatted (detail);
        }
        ImGui::PopTextWrapPos ();
        ImGui::EndTooltip ();
    }
}

// The speeds a timeline offers, as multiples of its base step.
constexpr float kTimelineSpeeds[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
constexpr const char* kTimelineSpeedNames[] = { "0.25x", "0.5x", "1x", "2x", "4x" };

bool DrawSunStudyTimeline (const char* id, int& index, int first, int last, HudTimeline& play, double stepSeconds,
                           const std::function<std::string ()>& label, bool hold)
{
    ImGui::PushID (id);
    const int before = index;
    index = std::clamp (index, first, last);
    // Playing: one step every stepSeconds / speed. `hold` (the previous frame
    // is still being prepared) pauses the clock, so a slow display drops no
    // frame and the player never runs ahead of what is on screen.
    const double now = ImGui::GetTime ();
    if (play.playing && hold)
        play.playedAt = now;
    else if (play.playing && now - play.playedAt > stepSeconds / double ((std::max) (play.speed, 0.05f))) {
        play.playedAt = now;
        if (index < last)
            ++index;
        else if (play.loop)
            index = first;
        else
            play.playing = false;
    }
    if (ImGui::ArrowButton ("##first", ImGuiDir_Left)) {
        index = first;
        play.playing = false;
    }
    ImGui::SetItemTooltip ("First");
    ImGui::SameLine (0.0f, 2.0f);
    if (ImGui::SmallButton ("<") && index > first)
        --index;
    ImGui::SetItemTooltip ("Step back");
    ImGui::SameLine (0.0f, 2.0f);
    if (ImGui::SmallButton (play.playing ? "pause" : "play")) {
        play.playing = !play.playing;
        play.playedAt = now;
        if (play.playing && index >= last && !play.loop)
            index = first;
    }
    ImGui::SameLine (0.0f, 2.0f);
    if (ImGui::SmallButton (">") && index < last)
        ++index;
    ImGui::SetItemTooltip ("Step forward");
    ImGui::SameLine (0.0f, 2.0f);
    if (ImGui::ArrowButton ("##last", ImGuiDir_Right)) {
        index = last;
        play.playing = false;
    }
    ImGui::SetItemTooltip ("Last");
    ImGui::SameLine ();
    int speed = 2;
    for (int i = 0; i < int (std::size (kTimelineSpeeds)); ++i)
        if (std::abs (kTimelineSpeeds[i] - play.speed) < 1e-3f)
            speed = i;
    ImGui::SetNextItemWidth (ImGui::CalcTextSize ("0.25x").x + ImGui::GetFrameHeight () * 1.6f);
    if (ImGui::Combo ("##speed", &speed, kTimelineSpeedNames, int (std::size (kTimelineSpeedNames))))
        play.speed = kTimelineSpeeds[speed];
    ImGui::SetItemTooltip ("Playback speed");
    ImGui::SameLine ();
    ImGui::Checkbox ("loop", &play.loop);
    ImGui::SetNextItemWidth (-1.0f);
    // Suppress SliderInt's own pre-input format. Drawing the label afterwards
    // covers automatic steps, buttons and a slider drag in the same frame.
    ImGui::SliderInt ("##timeline", &index, first, last, "", ImGuiSliderFlags_NoInput);
    const std::string text = label != nullptr ? label () : std::string ();
    if (!text.empty ()) {
        const ImVec2 min = ImGui::GetItemRectMin (), max = ImGui::GetItemRectMax ();
        const ImVec2 extent = ImGui::CalcTextSize (text.c_str ());
        const ImVec2 at (min.x + (max.x - min.x - extent.x) * 0.5f, min.y + (max.y - min.y - extent.y) * 0.5f);
        ImDrawList* draw = ImGui::GetWindowDrawList ();
        draw->PushClipRect (min, max, true);
        draw->AddText (at, ImGui::GetColorU32 (ImGuiCol_Text), text.c_str ());
        draw->PopClipRect ();
    }
    ImGui::PopID ();
    return index != before;
}

bool DrawSunStudyTimeline (const char* id, int& index, int first, int last, HudTimeline& play, double stepSeconds,
                           const char* label, bool hold)
{
    const std::string text = label != nullptr ? label : "";
    return DrawSunStudyTimeline (id, index, first, last, play, stepSeconds, [text] { return text; }, hold);
}

} // namespace geomsrv::archviz
