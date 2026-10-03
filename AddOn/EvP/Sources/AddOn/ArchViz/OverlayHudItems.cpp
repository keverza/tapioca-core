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
    const float h = (item.heightPixels > 0.0f ? item.heightPixels : 12.0f) * scale;
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
    const float h = (item.heightPixels > 0.0f ? item.heightPixels : 40.0f) * scale;
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
    if (!item.info.empty ()) {
        // The help marker: a small circled i beside the title, its text when pointed at.
        // Tested by its rectangle: the section's row spans it and holds ImGui's hover.
        ImGui::SameLine ();
        const float line = ImGui::GetTextLineHeight ();
        const float d = line * 0.8f;
        const ImVec2 q = ImGui::GetCursorScreenPos ();
        const ImVec2 centre (q.x + d * 0.5f, q.y + line * 0.5f);
        const ImU32 muted = Packed (WithAlpha (panel.textRgba, 0.6f));
        ImDrawList* draw = ImGui::GetWindowDrawList ();
        draw->AddCircle (centre, d * 0.5f, muted, 0, (std::max) (1.0f, scale));
        const float small = ImGui::GetFontSize () * 0.7f;
        const ImVec2 glyph = ImGui::GetFont ()->CalcTextSizeA (small, FLT_MAX, 0.0f, "i");
        draw->AddText (ImGui::GetFont (), small, ImVec2 (centre.x - glyph.x * 0.5f, centre.y - glyph.y * 0.5f), muted,
                       "i");
        ImGui::Dummy (ImVec2 (d, line));
        const ImVec2 low = ImGui::GetItemRectMin (), high = ImGui::GetItemRectMax ();
        if (ImGui::IsWindowHovered () && ImGui::IsMouseHoveringRect (low, high)) {
            // At the marker, not the pointer: moving over it draws nothing new.
            ImGui::SetNextWindowPos (ImVec2 (low.x, high.y + 4.0f * scale), ImGuiCond_Always);
            if (ImGui::BeginTooltip ()) {
                ImGui::PushTextWrapPos (ImGui::GetFontSize () * 22.0f);
                ImGui::TextUnformatted (item.info.c_str ());
                ImGui::PopTextWrapPos ();
                ImGui::EndTooltip ();
            }
        }
    }
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

uint32_t SegmentColour (const layers::PanelItem& item, size_t index, size_t count)
{
    // Tableau's ten: distinct on a light card and on a dark one.
    static const uint32_t kPalette[] = { 0x4E79A7FFu, 0xF28E2BFFu, 0xE15759FFu, 0x76B7B2FFu, 0x59A14FFFu,
                                         0xEDC948FFu, 0xB07AA1FFu, 0xFF9DA7FFu, 0x9C755FFFu, 0xBAB0ACFFu };
    if (index < item.colors.size ())
        return item.colors[index];
    if (item.colormap.stops.size () >= 2)
        return overlayscene::RampAt (item.colormap.stops, count > 1 ? float (index) / float (count - 1) : 0.5f);
    return kPalette[index % (sizeof (kPalette) / sizeof (kPalette[0]))];
}

namespace {

// A small tooltip at `at` (its `pivot` there): a swatch of `rgba`, then `text`.
void KeyTip (uint32_t rgba, const std::string& text, ImVec2 at, ImVec2 pivot, float scale)
{
    ImGui::SetNextWindowPos (at, ImGuiCond_Always, pivot);
    if (!ImGui::BeginTooltip ())
        return;
    const float s = ImGui::GetFontSize ();
    const ImVec2 p = ImGui::GetCursorScreenPos ();
    ImGui::GetWindowDrawList ()->AddRectFilled (p, ImVec2 (p.x + s, p.y + s), Packed (rgba), 3.0f * scale);
    ImGui::Dummy (ImVec2 (s, s));
    ImGui::SameLine ();
    ImGui::TextUnformatted (text.c_str ());
    ImGui::EndTooltip ();
}

uint32_t PerRow (uint32_t perRow, uint32_t fallback)
{
    return (std::min) ((std::max) (perRow == 0 ? fallback : perRow, 1u), 4u);
}

} // namespace

void Metrics (const layers::Panel& panel, const layers::PanelItem& item, float width, float scale)
{
    const int per = int (PerRow (item.perRow, 2u));
    const float w = item.widthPixels > 0.0f ? item.widthPixels * scale : width;
    const float size = item.sizePixels > 0.0f ? item.sizePixels * scale : ImGui::GetFontSize ();
    const uint32_t valueColour = (item.rgba & 0xFFu) != 0 ? item.rgba : panel.textRgba;
    ImGui::PushStyleVar (ImGuiStyleVar_CellPadding, ImVec2 (6.0f * scale, 2.0f * scale));
    if (ImGui::BeginTable ("##metrics", per,
                           ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings,
                           ImVec2 (w, 0.0f))) {
        for (const std::vector<std::string>& cell : item.rows) {
            ImGui::TableNextColumn ();
            ImGui::PushFont (nullptr, size * 0.9f);
            ImGui::PushStyleColor (ImGuiCol_Text, Colour (WithAlpha (panel.textRgba, 0.72f)));
            ImGui::TextUnformatted (cell.empty () ? "" : cell[0].c_str ());
            ImGui::PopStyleColor ();
            ImGui::PopFont ();
            ImGui::PushFont (nullptr, size * 1.2f);
            ImGui::PushStyleColor (ImGuiCol_Text, Colour (valueColour));
            ImGui::TextUnformatted (cell.size () < 2 ? "" : cell[1].c_str ());
            ImGui::PopStyleColor ();
            ImGui::PopFont ();
        }
        ImGui::EndTable ();
    }
    ImGui::PopStyleVar ();
}

void Keys (const layers::Panel& panel, size_t begin, size_t end, float width, float scale)
{
    const layers::PanelItem& first = panel.items[begin];
    bool valued = false;
    for (size_t i = begin; i < end; ++i)
        valued = valued || !panel.items[i].value.empty ();
    // A plain key -- names only, one to a row -- is drawn as it always was.
    if (!valued && PerRow (first.perRow, 1u) == 1u) {
        for (size_t i = begin; i < end; ++i) {
            ImGui::PushID (int (i));
            Swatch (panel, panel.items[i], scale);
            ImGui::PopID ();
        }
        return;
    }
    const int per = int (PerRow (first.perRow, 1u));
    const float w = first.widthPixels > 0.0f ? first.widthPixels * scale : width;
    ImGui::PushStyleVar (ImGuiStyleVar_CellPadding, ImVec2 (4.0f * scale, 1.0f * scale));
    if (ImGui::BeginTable ("##keys", per, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings,
                           ImVec2 (w, 0.0f))) {
        for (size_t i = begin; i < end; ++i) {
            const layers::PanelItem& item = panel.items[i];
            ImGui::TableNextColumn ();
            const float s = ImGui::GetFontSize ();
            const ImVec2 p = ImGui::GetCursorScreenPos ();
            const uint32_t colour = (item.rgba & 0xFFu) != 0 ? item.rgba : panel.textRgba;
            ImGui::GetWindowDrawList ()->AddRectFilled (p, ImVec2 (p.x + s, p.y + s), Packed (colour), 3.0f * scale);
            ImGui::Dummy (ImVec2 (s, s));
            ImGui::SameLine ();
            ImGui::TextUnformatted (item.text.c_str ());
            if (!item.value.empty ()) {
                ImGui::SameLine ();
                const float v = ImGui::CalcTextSize (item.value.c_str ()).x;
                ImGui::SetCursorPosX (ImGui::GetCursorPosX () +
                                      (std::max) (ImGui::GetContentRegionAvail ().x - v, 0.0f));
                ImGui::TextUnformatted (item.value.c_str ());
            }
        }
        ImGui::EndTable ();
    }
    ImGui::PopStyleVar ();
}

void Stack (const layers::Panel& panel, const layers::PanelItem& item, float width, float scale)
{
    const float w = item.widthPixels > 0.0f ? item.widthPixels * scale : width;
    const float h = (item.heightPixels > 0.0f ? item.heightPixels : 18.0f) * scale;
    const ImVec2 p = ImGui::GetCursorScreenPos ();
    ImGui::Dummy (ImVec2 (w, h));
    double total = 0.0;
    size_t first = item.values.size (), last = 0;
    for (size_t i = 0; i < item.values.size (); ++i)
        if (item.values[i] > 0.0) {
            total += item.values[i];
            first = (std::min) (first, i);
            last = i;
        }
    if (!(total > 0.0) || w <= 0.0f)
        return;
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    const float rounding = (std::min) (6.0f * scale, h * 0.5f);
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    const bool pointed = ImGui::IsWindowHovered () && ImGui::IsMouseHoveringRect (p, ImVec2 (p.x + w, p.y + h));
    float x = p.x;
    for (size_t i = 0; i < item.values.size (); ++i) {
        const double value = item.values[i];
        if (!(value > 0.0))
            continue;
        const float x1 = i == last ? p.x + w : x + w * float (value / total);
        const uint32_t colour = SegmentColour (item, i, item.values.size ());
        ImDrawFlags corners = 0;
        if (i == first)
            corners |= ImDrawFlags_RoundCornersLeft;
        if (i == last)
            corners |= ImDrawFlags_RoundCornersRight;
        if (corners == 0)
            corners = ImDrawFlags_RoundCornersNone;
        draw->AddRectFilled (ImVec2 (x, p.y), ImVec2 (x1, p.y + h), Packed (colour), rounding, corners);
        // Its share inside it, where it fits.
        const std::string share = Number (100.0 * value / total, item.decimals) + "%";
        const ImVec2 size = ImGui::CalcTextSize (share.c_str ());
        if (x1 - x > size.x + 10.0f * scale)
            draw->AddText (ImVec2 (x + 6.0f * scale, p.y + (h - size.y) * 0.5f), Packed (Contrast (colour)),
                           share.c_str ());
        if (pointed && mouse.x >= x && mouse.x < x1) {
            std::string text = i < item.labels.size () ? item.labels[i] + "  " : std::string ();
            text += Number (value, item.decimals);
            if (!item.unit.empty ())
                text += " " + item.unit;
            text += "  (" + share + ")";
            KeyTip (colour, text, ImVec2 ((x + x1) * 0.5f, p.y - 2.0f * scale), ImVec2 (0.5f, 1.0f), scale);
        }
        x = x1;
    }
}

void Bars (const layers::Panel& panel, const layers::PanelItem& item, float width, float scale)
{
    const float w = item.widthPixels > 0.0f ? item.widthPixels * scale : width;
    const float h = (item.heightPixels > 0.0f ? item.heightPixels : 80.0f) * scale;
    const size_t n = item.values.size ();
    const ImVec2 p = ImGui::GetCursorScreenPos ();
    const float font = ImGui::GetFontSize ();
    const float small = font * 0.85f;
    const bool labelled = !item.labels.empty ();
    const float below = (labelled ? small + 4.0f * scale : 0.0f) + (item.text.empty () ? 0.0f : font + 4.0f * scale);
    ImGui::Dummy (ImVec2 (w, h + below));
    if (n == 0 || w <= 0.0f)
        return;

    // The value axis: from the lowest of zero and the values to the highest, or as given.
    double bottom = 0.0, top = 0.0;
    for (const double value : item.values) {
        bottom = (std::min) (bottom, value);
        top = (std::max) (top, value);
    }
    if (!item.autoRange) {
        bottom = item.min;
        top = item.max;
    }
    if (!(top > bottom))
        top = bottom + 1.0;
    ImFont* const face = ImGui::GetFont ();
    const double ticks[3] = { bottom, (bottom + top) * 0.5, top };
    std::string tickText[3];
    float axis = 0.0f;
    for (int k = 0; k < 3; ++k) {
        tickText[k] = Number (ticks[k], item.decimals) + item.unit;
        axis = (std::max) (axis, face->CalcTextSizeA (small, FLT_MAX, 0.0f, tickText[k].c_str ()).x);
    }
    axis += 6.0f * scale;
    const ImVec2 low (p.x + axis, p.y + small * 0.5f), high (p.x + w, p.y + h);
    const float plotW = high.x - low.x, plotH = high.y - low.y;
    if (plotW <= 1.0f || plotH <= 1.0f)
        return;
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    const ImU32 grid = Packed (WithAlpha (panel.textRgba, 0.14f));
    const ImU32 muted = Packed (WithAlpha (panel.textRgba, 0.72f));
    auto yOf = [&] (double value) { return high.y - float ((value - bottom) / (top - bottom)) * plotH; };
    for (int k = 0; k < 3; ++k) {
        const float y = yOf (ticks[k]);
        draw->AddLine (ImVec2 (low.x, y), ImVec2 (high.x, y), grid, (std::max) (1.0f, scale));
        const ImVec2 size = face->CalcTextSizeA (small, FLT_MAX, 0.0f, tickText[k].c_str ());
        draw->AddText (face, small, ImVec2 (low.x - 6.0f * scale - size.x, y - size.y * 0.5f), muted,
                       tickText[k].c_str ());
    }
    draw->AddLine (ImVec2 (low.x, low.y), ImVec2 (low.x, high.y), grid, (std::max) (1.0f, scale));

    // The bars, their tops rounded, and the labels under every one that fits.
    const float slot = plotW / float (n);
    const float bar = (std::max) (slot * 0.72f, 1.0f);
    const float zero = yOf ((std::max) (bottom, (std::min) (0.0, top)));
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    const bool pointed = ImGui::IsWindowHovered () && ImGui::IsMouseHoveringRect (low, ImVec2 (high.x, high.y));
    float widest = 0.0f;
    for (const std::string& label : item.labels)
        widest = (std::max) (widest, face->CalcTextSizeA (small, FLT_MAX, 0.0f, label.c_str ()).x);
    const size_t stride = (std::max) (size_t (1), size_t (std::ceil ((widest + 4.0f * scale) / slot)));
    for (size_t i = 0; i < n; ++i) {
        const float x0 = low.x + slot * float (i) + (slot - bar) * 0.5f;
        const float y = yOf (item.values[i]);
        const uint32_t colour = SegmentColour (item, i, n);
        const float r = (std::min) (3.0f * scale, bar * 0.5f);
        draw->AddRectFilled (ImVec2 (x0, (std::min) (y, zero)), ImVec2 (x0 + bar, (std::max) (y, zero)),
                             Packed (colour), r,
                             y <= zero ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersBottom);
        if (labelled && i < item.labels.size () && i % stride == 0) {
            const ImVec2 size = face->CalcTextSizeA (small, FLT_MAX, 0.0f, item.labels[i].c_str ());
            draw->AddText (face, small, ImVec2 (x0 + bar * 0.5f - size.x * 0.5f, high.y + 3.0f * scale), muted,
                           item.labels[i].c_str ());
        }
        if (pointed && mouse.x >= low.x + slot * float (i) && mouse.x < low.x + slot * float (i + 1)) {
            std::string text = i < item.labels.size () ? item.labels[i] + "  " : std::string ();
            text += Number (item.values[i], item.decimals) + item.unit;
            KeyTip (colour, text, ImVec2 (x0 + bar * 0.5f, (std::min) (y, zero) - 2.0f * scale), ImVec2 (0.5f, 1.0f),
                    scale);
        }
    }
    if (!item.text.empty ()) {
        const ImVec2 size = ImGui::CalcTextSize (item.text.c_str ());
        draw->AddText (
            ImVec2 (low.x + (plotW - size.x) * 0.5f, high.y + (labelled ? small + 4.0f * scale : 0.0f) + 2.0f * scale),
            muted, item.text.c_str ());
    }
}

namespace {

// A control's label over it, muted as a row's label is.
void LabelOver (const layers::Panel& panel, const layers::PanelItem& item)
{
    if (item.text.empty ())
        return;
    ImGui::PushStyleColor (ImGuiCol_Text, Colour (WithAlpha (panel.textRgba, 0.72f)));
    ImGui::TextUnformatted (item.text.c_str ());
    ImGui::PopStyleColor ();
}

// `text` where ImGui reads a printf format: its '%' doubled.
std::string Literal (const std::string& text)
{
    std::string out;
    for (const char c : text) {
        out += c;
        if (c == '%')
            out += '%';
    }
    return out;
}

} // namespace

bool Checkbox (const layers::PanelItem& item, bool& on)
{
    return ImGui::Checkbox ((item.text + "##" + item.id).c_str (), &on);
}

std::string SliderText (const layers::PanelItem& item, double value)
{
    return Number (value, item.decimals) + (item.unit.empty () ? std::string () : " " + item.unit);
}

bool Slider (const layers::Panel& panel, const layers::PanelItem& item, double& value, float width, bool& released)
{
    LabelOver (panel, item);
    const std::string format = "%." + std::to_string ((std::min) (item.decimals, 6u)) + "f" +
                               (item.unit.empty () ? std::string () : " " + Literal (item.unit));
    ImGui::SetNextItemWidth (width);
    double v = value;
    // ⚠️ NO TEXT ENTRY: Ctrl+click would turn the bar into a text field the HUD takes no
    // keys for.
    const bool moved = ImGui::SliderScalar (("##" + item.id).c_str (), ImGuiDataType_Double, &v, &item.min, &item.max,
                                            format.c_str (), ImGuiSliderFlags_NoInput);
    released = ImGui::IsItemDeactivatedAfterEdit ();
    if (!moved)
        return false;
    if (item.step > 0.0)
        v = item.min + std::round ((v - item.min) / item.step) * item.step;
    v = (std::min) ((std::max) (v, item.min), item.max);
    if (v == value)
        return false;
    value = v;
    return true;
}

bool Combo (const layers::Panel& panel, const layers::PanelItem& item, uint32_t& chosen, float width)
{
    LabelOver (panel, item);
    ImGui::SetNextItemWidth (width);
    const char* preview = chosen < item.labels.size () ? item.labels[chosen].c_str () : "";
    // Every option shown, none scrolled to: the wheel is Archicad's zoom, even over the HUD.
    if (!ImGui::BeginCombo (("##" + item.id).c_str (), preview, ImGuiComboFlags_HeightLargest))
        return false;
    bool changed = false;
    for (uint32_t k = 0; k < uint32_t (item.labels.size ()); ++k) {
        ImGui::PushID (int (k));
        if (ImGui::Selectable (item.labels[k].c_str (), k == chosen) && k != chosen) {
            chosen = k;
            changed = true;
        }
        ImGui::PopID ();
    }
    ImGui::EndCombo ();
    return changed;
}

bool Button (const layers::PanelItem& item, float width)
{
    return ImGui::Button ((item.text + "##" + item.id).c_str (), ImVec2 (item.widthPixels > 0.0f ? width : 0.0f, 0.0f));
}

} // namespace items
} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv
