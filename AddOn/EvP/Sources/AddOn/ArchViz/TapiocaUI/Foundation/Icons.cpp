#include "ArchViz/TapiocaUI/Foundation/Icons.hpp"

#include "ArchViz/TapiocaUI/Foundation/IconData.hpp"

#include <imgui_internal.h> // FindRenderedTextEnd: a label's "##" suffix is not drawn

#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::ui {

namespace {

// A subpath this small is one of Iconoir's dots: a zero-length stroke shown by its round cap.
constexpr float kDotExtent = 0.02f;

// Two thirds of a button's side, whole pixels: 16 in the toolbar's 24.
float GlyphSide (float side)
{
    return std::round (side * 2.0f / 3.0f);
}

// The ground and the glyph's colour of a button pointed at, held, toggled on or at rest.
void ButtonColours (bool active, ImU32& ground, ImU32& ink)
{
    const bool held = ImGui::IsItemActive ();
    const bool hovered = ImGui::IsItemHovered ();
    ground = ImGui::GetColorU32 (held      ? ImGuiCol_ButtonActive
                                 : hovered ? ImGuiCol_ButtonHovered
                                 : active  ? ImGuiCol_HeaderActive
                                           : ImGuiCol_Button);
    ink = IconInk (active);
}

} // namespace

void DrawIcon (ImDrawList* draw, IconId icon, ImVec2 centre, float size, ImU32 colour)
{
    if (draw == nullptr || size <= 0.0f)
        return;
    const icondata::Glyph& glyph = icondata::GlyphAt (uint32_t (icon));
    const icondata::Stroke* const strokes = icondata::Strokes () + glyph.first;
    const float* const points = icondata::Points ();
    const ImVec2 origin (centre.x - size * 0.5f, centre.y - size * 0.5f);
    const float thickness = (std::max) (icondata::kStrokeWidth * size, 1.0f);
    const auto at = [&] (uint32_t index) {
        return ImVec2 (origin.x + points[2 * index] * size, origin.y + points[2 * index + 1] * size);
    };
    for (uint16_t s = 0; s < glyph.count; ++s) {
        const icondata::Stroke& stroke = strokes[s];
        if (stroke.count == 0)
            continue;
        const uint32_t first = stroke.first, last = stroke.first + stroke.count - 1;
        float extent = 0.0f;
        for (uint32_t i = first + 1; i <= last; ++i)
            extent = (std::max) (extent, (std::max) (std::fabs (points[2 * i] - points[2 * first]),
                                                     std::fabs (points[2 * i + 1] - points[2 * first + 1])));
        if (stroke.count == 1 || extent < kDotExtent) {
            draw->AddCircleFilled (at (first), thickness * 0.5f, colour);
            continue;
        }
        for (uint32_t i = first; i <= last; ++i)
            draw->PathLineTo (at (i));
        draw->PathStroke (colour, stroke.closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness);
        // Iconoir's caps are round; ImGui's stroke ends square, so an open end gets a disc.
        if (!stroke.closed) {
            draw->AddCircleFilled (at (first), thickness * 0.5f, colour);
            draw->AddCircleFilled (at (last), thickness * 0.5f, colour);
        }
    }
}

ImU32 IconInk (bool selected)
{
    return ImGui::GetColorU32 (ImGuiCol_Text, selected ? 1.0f : kIconRestAlpha);
}

void Icon (IconId icon, ImVec2 position, float size, ImU32 colour)
{
    DrawIcon (ImGui::GetWindowDrawList (), icon, ImVec2 (position.x + size * 0.5f, position.y + size * 0.5f), size,
              colour);
}

bool IconButton (const char* id, IconId icon, bool active)
{
    const float side = ImGui::GetFrameHeight ();
    const ImVec2 at = ImGui::GetCursorScreenPos ();
    const bool pressed = ImGui::InvisibleButton (id, ImVec2 (side, side));
    ImU32 ground = 0, ink = 0;
    ButtonColours (active, ground, ink);
    ImDrawList* const draw = ImGui::GetWindowDrawList ();
    draw->AddRectFilled (at, ImVec2 (at.x + side, at.y + side), ground, ImGui::GetStyle ().FrameRounding);
    DrawIcon (draw, icon, ImVec2 (at.x + side * 0.5f, at.y + side * 0.5f), GlyphSide (side), ink);
    return pressed;
}

bool IconTextButton (const char* id, IconId icon, const char* label, bool active)
{
    const ImGuiStyle& style = ImGui::GetStyle ();
    const char* const end = ImGui::FindRenderedTextEnd (label);
    const ImVec2 text = ImGui::CalcTextSize (label, end);
    const float side = ImGui::GetFrameHeight ();
    const float glyph = GlyphSide (side);
    const float lead = icon == IconId::None ? 0.0f : glyph + style.ItemInnerSpacing.x;
    const float width = style.FramePadding.x * 2.0f + lead + text.x;
    const ImVec2 at = ImGui::GetCursorScreenPos ();
    const bool pressed = ImGui::InvisibleButton (id, ImVec2 (width, side));
    ImU32 ground = 0, ink = 0;
    ButtonColours (active, ground, ink);
    ImDrawList* const draw = ImGui::GetWindowDrawList ();
    draw->AddRectFilled (at, ImVec2 (at.x + width, at.y + side), ground, style.FrameRounding);
    const float left = at.x + style.FramePadding.x;
    if (icon != IconId::None)
        DrawIcon (draw, icon, ImVec2 (left + glyph * 0.5f, at.y + side * 0.5f), glyph, ink);
    draw->AddText (ImVec2 (left + lead, at.y + (side - text.y) * 0.5f),
                   ImGui::GetColorU32 (ImGuiCol_Text), label, end);
    return pressed;
}

uint32_t IconCount ()
{
    return icondata::GlyphCount ();
}

const char* IconName (IconId icon)
{
    return icondata::GlyphAt (uint32_t (icon)).name;
}

const char* IconFile (IconId icon)
{
    return icondata::GlyphAt (uint32_t (icon)).file;
}

} // namespace geomsrv::archviz::ui
