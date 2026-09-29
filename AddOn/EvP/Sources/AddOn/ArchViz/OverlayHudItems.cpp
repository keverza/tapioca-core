// ArchViz/OverlayHudItems -- see the header.

#include "ArchViz/OverlayHudItems.hpp"

#include "ArchViz/OverlayScene.hpp" // RampAt: the ramp the guest's pixel shader draws

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlayhud {
namespace items {

namespace layers = overlaylayers;

namespace {

// The share of a smooth ramp's range either side of the value that a highlight shows.
constexpr double kHighlightHalfShare = 0.05;

} // namespace

ImVec4 Colour (uint32_t rgba)
{
    return ImVec4 (float ((rgba >> 24) & 0xFFu) / 255.0f, float ((rgba >> 16) & 0xFFu) / 255.0f,
                   float ((rgba >> 8) & 0xFFu) / 255.0f, float (rgba & 0xFFu) / 255.0f);
}

ImU32 Packed (uint32_t rgba)
{
    return IM_COL32 ((rgba >> 24) & 0xFFu, (rgba >> 16) & 0xFFu, (rgba >> 8) & 0xFFu, rgba & 0xFFu);
}

// ImGui's packed colour (R in the low byte) back to the caller's 0xRRGGBBAA.
uint32_t Unpacked (ImU32 col)
{
    const uint32_t r = col & 0xFFu, g = (col >> 8) & 0xFFu, b = (col >> 16) & 0xFFu, a = (col >> 24) & 0xFFu;
    return (r << 24) | (g << 16) | (b << 8) | a;
}

uint32_t WithAlpha (uint32_t rgba, float factor)
{
    const uint32_t a = uint32_t (std::lround (float (rgba & 0xFFu) * factor));
    return (rgba & 0xFFFFFF00u) | (std::min) (a, 255u);
}

std::string Number (double value, uint32_t decimals)
{
    char buffer[64] = {};
    std::snprintf (buffer, sizeof (buffer), "%.*f", int ((std::min) (decimals, 6u)), value);
    return buffer;
}

void ValueTip (const layers::Colormap& colormap, float t, double low, double high, uint32_t decimals,
               const std::string& unit, ImVec2 at, ImVec2 pivot, float scale, double band[2])
{
    t = (std::min) ((std::max) (t, 0.0f), 1.0f);
    std::string text;
    float colourAt = t;
    if (colormap.bands > 0) {
        const uint32_t bands = colormap.bands;
        const uint32_t k = (std::min) (uint32_t (t * float (bands)), bands - 1);
        band[0] = low + (high - low) * double (k) / double (bands);
        band[1] = low + (high - low) * double (k + 1) / double (bands);
        text = Number (band[0], decimals) + " \xE2\x80\x93 " + Number (band[1], decimals);
        colourAt = bands > 1 ? float (k) / float (bands - 1) : 0.5f;
    }
    else {
        const double value = low + (high - low) * double (t), half = (high - low) * kHighlightHalfShare;
        band[0] = (std::max) (value - half, (std::min) (low, high));
        band[1] = (std::min) (value + half, (std::max) (low, high));
        text = Number (value, decimals);
    }
    if (!unit.empty ())
        text += " " + unit;
    ImGui::SetNextWindowPos (at, ImGuiCond_Always, pivot);
    if (!ImGui::BeginTooltip ())
        return;
    const float s = ImGui::GetFontSize ();
    const ImVec2 p = ImGui::GetCursorScreenPos ();
    ImGui::GetWindowDrawList ()->AddRectFilled (p, ImVec2 (p.x + s, p.y + s),
                                                Packed (overlayscene::RampAt (colormap.stops, colourAt)), 2.0f * scale);
    ImGui::Dummy (ImVec2 (s, s));
    ImGui::SameLine ();
    ImGui::TextUnformatted (text.c_str ());
    ImGui::EndTooltip ();
}

void Rows (const layers::Panel& panel, size_t begin, size_t end, float scale)
{
    if (!ImGui::BeginTable ("##rows", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings))
        return;
    for (size_t i = begin; i < end; ++i) {
        const layers::PanelItem& item = panel.items[i];
        const bool sized = item.sizePixels > 0.0f;
        if (sized)
            ImGui::PushFont (nullptr, item.sizePixels * scale);
        ImGui::TableNextRow ();
        ImGui::TableSetColumnIndex (0);
        ImGui::PushStyleColor (ImGuiCol_Text, Colour (WithAlpha (panel.textRgba, 0.72f)));
        ImGui::TextUnformatted (item.text.c_str ());
        ImGui::PopStyleColor ();
        ImGui::TableSetColumnIndex (1);
        ImGui::PushStyleColor (ImGuiCol_Text, Colour ((item.rgba & 0xFFu) != 0 ? item.rgba : panel.textRgba));
        ImGui::TextUnformatted (item.value.c_str ());
        ImGui::PopStyleColor ();
        if (sized)
            ImGui::PopFont ();
    }
    ImGui::EndTable ();
}

void Text (const layers::Panel& panel, const layers::PanelItem& item, float width, float scale)
{
    const uint32_t colour = (item.rgba & 0xFFu) != 0 ? item.rgba : panel.textRgba;
    const bool sized = item.sizePixels > 0.0f;
    if (sized)
        ImGui::PushFont (nullptr, item.sizePixels * scale);
    ImGui::PushStyleColor (ImGuiCol_Text, Colour (colour));
    if (item.wrap)
        ImGui::PushTextWrapPos (ImGui::GetCursorPosX () + width);
    ImGui::TextUnformatted (item.text.c_str ());
    if (item.wrap)
        ImGui::PopTextWrapPos ();
    ImGui::PopStyleColor ();
    if (sized)
        ImGui::PopFont ();
}

void Progress (const layers::PanelItem& item, float width, float scale)
{
    const float w = item.widthPixels > 0.0f ? item.widthPixels * scale : width;
    const float h = (item.heightPixels > 0.0f ? item.heightPixels : 14.0f) * scale;
    ImGui::PushStyleColor (ImGuiCol_PlotHistogram, Colour ((item.rgba & 0xFFu) != 0 ? item.rgba : 0x3D8BFDFFu));
    const float fraction = float ((std::min) ((std::max) (item.fraction, 0.0), 1.0));
    ImGui::ProgressBar (fraction, ImVec2 (w, h), item.text.empty () ? nullptr : item.text.c_str ());
    ImGui::PopStyleColor ();
}

void Swatch (const layers::Panel& panel, const layers::PanelItem& item, float scale)
{
    const uint32_t colour = (item.rgba & 0xFFu) != 0 ? item.rgba : panel.textRgba;
    const float s = ImGui::GetFontSize ();
    const ImVec2 p = ImGui::GetCursorScreenPos ();
    ImGui::GetWindowDrawList ()->AddRectFilled (p, ImVec2 (p.x + s, p.y + s), Packed (colour), 2.0f * scale);
    ImGui::Dummy (ImVec2 (s, s));
    ImGui::SameLine ();
    ImGui::TextUnformatted (item.text.c_str ());
}

void Plot (const layers::PanelItem& item, float width, float scale)
{
    std::vector<float> values;
    values.reserve (item.values.size ());
    for (const double value : item.values)
        values.push_back (float (value));
    const float w = item.widthPixels > 0.0f ? item.widthPixels * scale : width;
    const float h = (item.heightPixels > 0.0f ? item.heightPixels : 48.0f) * scale;
    ImGui::PushStyleColor (ImGuiCol_PlotLines, Colour ((item.rgba & 0xFFu) != 0 ? item.rgba : 0x3D8BFDFFu));
    ImGui::PlotLines ("##plot", values.data (), int (values.size ()), 0,
                      item.text.empty () ? nullptr : item.text.c_str (), item.autoRange ? FLT_MAX : float (item.min),
                      item.autoRange ? FLT_MAX : float (item.max), ImVec2 (w, h));
    ImGui::PopStyleColor ();
}

void Table (const layers::PanelItem& item)
{
    size_t columns = item.columns.size ();
    for (const std::vector<std::string>& row : item.rows)
        columns = (std::max) (columns, row.size ());
    if (columns == 0 || !ImGui::BeginTable ("##table", int (columns),
                                            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings |
                                                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        return;
    if (!item.columns.empty ()) {
        for (size_t c = 0; c < columns; ++c)
            ImGui::TableSetupColumn (c < item.columns.size () ? item.columns[c].c_str () : "");
        ImGui::TableHeadersRow ();
    }
    for (const std::vector<std::string>& row : item.rows) {
        ImGui::TableNextRow ();
        for (size_t c = 0; c < row.size (); ++c) {
            ImGui::TableSetColumnIndex (int (c));
            ImGui::TextUnformatted (row[c].c_str ());
        }
    }
    ImGui::EndTable ();
}

bool Ramp (const layers::Panel& panel, const layers::PanelItem& item, float width, float scale, double band[2])
{
    if (!item.text.empty ())
        ImGui::TextUnformatted (item.text.c_str ());
    const float w = item.widthPixels > 0.0f ? item.widthPixels * scale : width;
    const float h = (item.heightPixels > 0.0f ? item.heightPixels : 12.0f) * scale;
    const ImVec2 p = ImGui::GetCursorScreenPos ();
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    const std::vector<layers::ColourStop>& stops = item.colormap.stops;
    if (item.colormap.bands > 0) {
        const uint32_t bands = item.colormap.bands;
        for (uint32_t k = 0; k < bands; ++k) {
            const float t = bands > 1 ? float (k) / float (bands - 1) : 0.5f;
            const ImU32 c = Packed (overlayscene::RampAt (stops, t));
            draw->AddRectFilled (ImVec2 (p.x + w * float (k) / float (bands), p.y),
                                 ImVec2 (p.x + w * float (k + 1) / float (bands), p.y + h), c);
        }
    }
    else if (!stops.empty ()) {
        draw->AddRectFilled (p, ImVec2 (p.x + w * stops.front ().at, p.y + h), Packed (stops.front ().rgba));
        for (size_t s = 1; s < stops.size (); ++s) {
            const ImU32 a = Packed (stops[s - 1].rgba), b = Packed (stops[s].rgba);
            draw->AddRectFilledMultiColor (ImVec2 (p.x + w * stops[s - 1].at, p.y),
                                           ImVec2 (p.x + w * stops[s].at, p.y + h), a, b, b, a);
        }
        draw->AddRectFilled (ImVec2 (p.x + w * stops.back ().at, p.y), ImVec2 (p.x + w, p.y + h),
                             Packed (stops.back ().rgba));
    }
    draw->AddRect (p, ImVec2 (p.x + w, p.y + h), Packed (WithAlpha (panel.textRgba, 0.45f)));

    // The ticks under the bar, each label centred on its tick and kept inside the bar.
    const double low = item.colormap.min, high = item.colormap.max;
    std::vector<double> values = item.tickValues;
    if (values.empty ()) {
        const uint32_t ticks = (std::max) (item.ticks, 2u);
        for (uint32_t k = 0; k < ticks; ++k)
            values.push_back (low + (high - low) * double (k) / double (ticks - 1));
    }
    const float font = ImGui::GetFontSize ();
    const ImU32 textColour = Packed ((item.rgba & 0xFFu) != 0 ? item.rgba : panel.textRgba);
    for (size_t k = 0; k < values.size (); ++k) {
        const double t = high > low ? (values[k] - low) / (high - low) : 0.0;
        if (t < -1e-9 || t > 1.0 + 1e-9)
            continue;
        const float x = p.x + w * float (t);
        draw->AddLine (ImVec2 (x, p.y + h), ImVec2 (x, p.y + h + 3.0f * scale), textColour, (std::max) (1.0f, scale));
        std::string label = k < item.tickLabels.size () ? item.tickLabels[k] : Number (values[k], item.decimals);
        if (!item.unit.empty () && k + 1 == values.size () && item.tickLabels.empty ())
            label += " " + item.unit;
        const ImVec2 size = ImGui::CalcTextSize (label.c_str ());
        const float left = (std::min) ((std::max) (x - size.x * 0.5f, p.x), p.x + w - size.x);
        draw->AddText (ImVec2 (left, p.y + h + 4.0f * scale), textColour, label.c_str ());
    }
    ImGui::Dummy (ImVec2 (w, h + 4.0f * scale + font));

    // Pointed at: the value there, over the bar at the pointer.
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    if (w <= 0.0f || !ImGui::IsWindowHovered () || !ImGui::IsMouseHoveringRect (p, ImVec2 (p.x + w, p.y + h)))
        return false;
    ValueTip (item.colormap, (mouse.x - p.x) / w, low, high, item.decimals, item.unit,
              ImVec2 (mouse.x, p.y - 2.0f * scale), ImVec2 (0.5f, 1.0f), scale, band);
    return true;
}

void Section (const layers::Panel& panel, const layers::PanelItem& item, bool& open, float scale)
{
    const bool sized = item.sizePixels > 0.0f;
    if (sized)
        ImGui::PushFont (nullptr, item.sizePixels * scale);
    ImGui::SetNextItemOpen (open, ImGuiCond_Always);
    open = ImGui::TreeNodeEx ("##section", ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                              "%s", item.text.c_str ());
    if (!item.value.empty ()) {
        ImGui::SameLine ();
        const float w = ImGui::CalcTextSize (item.value.c_str ()).x;
        ImGui::SetCursorPosX (ImGui::GetCursorPosX () + (std::max) (ImGui::GetContentRegionAvail ().x - w, 0.0f));
        ImGui::PushStyleColor (ImGuiCol_Text, Colour ((item.rgba & 0xFFu) != 0 ? item.rgba : panel.textRgba));
        ImGui::TextUnformatted (item.value.c_str ());
        ImGui::PopStyleColor ();
    }
    if (sized)
        ImGui::PopFont ();
}

} // namespace items
} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv
