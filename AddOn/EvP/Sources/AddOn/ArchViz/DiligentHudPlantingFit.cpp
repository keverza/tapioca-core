// The Planting tab's Fit sub-tab (#278 Stage 2): "what size of plant fits
// here?" without picking a species. Editable size classes are checked on the
// Scheme's region (layer, zone, measured room, no-go zones); the viewer
// colours every sample by the largest class that fits, and the panel gives the
// area of each class by reason and the reasons at the hovered point.
//
// The evaluation is PlantingSizeFit's; the map is drawn with the scheme's
// scene (PublishSchemeScene) while this sub-tab is open.
//
// RENDER THREAD. Reads HudState and plain values from the runner.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudPlantingParts.hpp"
#include "ArchViz/HudShell.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace geomsrv::archviz::plantinghud {

namespace pl = planting;

namespace {

constexpr pl::SizeReason kShownReasons[] = { pl::SizeReason::Fits,   pl::SizeReason::Light, pl::SizeReason::Overhead,
                                             pl::SizeReason::Narrow, pl::SizeReason::NoGo,  pl::SizeReason::Unknown };

void Swatch (uint32_t rgba)
{
    const float size = ImGui::GetTextLineHeight ();
    const ImVec2 min = ImGui::GetCursorScreenPos ();
    ImGui::GetWindowDrawList ()->AddRectFilled (min, ImVec2 (min.x + size, min.y + size), AbgrOf (rgba), 2.0f);
    ImGui::Dummy (ImVec2 (size, size));
}

int RankOf (const pl::SizeFitResult& fit, int cls)
{
    const auto at = std::find (fit.order.begin (), fit.order.end (), cls);
    return at == fit.order.end () ? -1 : int (at - fit.order.begin ());
}

// Evaluates again only when something it depends on changed.
void RefreshFit (pl::PlantingHud& hud)
{
    pl::FitState& f = hud.fit;
    const pl::SchemeState& s = hud.scheme;
    char head[160];
    std::snprintf (head, sizeof (head), "%s|zone%d@%llu|room%d/%zu|m%.3f|ds%d|n%zu", s.siteStudy.c_str (), s.zone,
                   (unsigned long long) hud.zones.revision, int (s.checkSpace), s.site.clearance.Count (),
                   double (s.clearanceMargin), int (hud.deepShade), s.site.Size ());
    std::string from = head;
    for (double e : s.siteEdges)
        from += "|" + std::to_string (e);
    for (const auto& c : f.classes)
        from += "|" + c.name + "/" + std::to_string (c.spread) + "/" + std::to_string (c.height) + "/" +
                std::to_string (c.light[0]) + std::to_string (c.light[1]) + std::to_string (c.light[2]) +
                std::to_string (c.on);
    if (from == f.from)
        return;
    f.site = SchemeRegionSite (hud);
    if (!s.checkSpace)
        f.site.clearance = {}; // switched off: space is not known, not passed
    f.result = pl::EvaluateSizeClasses (hud, f.site, f.classes, s.clearanceMargin);
    f.from = from;
    ++f.revision;
}

void ClassTable (pl::FitState& f)
{
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg;
    if (!ImGui::BeginTable ("##fit-classes", 8, flags))
        return;
    const float em = ImGui::GetFontSize ();
    ImGui::TableSetupColumn ("##on", ImGuiTableColumnFlags_WidthFixed, em * 1.4f);
    ImGui::TableSetupColumn ("##colour", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight ());
    ImGui::TableSetupColumn ("Class", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn ("Spread", ImGuiTableColumnFlags_WidthFixed, em * 4.2f);
    ImGui::TableSetupColumn ("Height", ImGuiTableColumnFlags_WidthFixed, em * 4.2f);
    ImGui::TableSetupColumn ("Sh", ImGuiTableColumnFlags_WidthFixed, em * 1.4f);
    ImGui::TableSetupColumn ("Pa", ImGuiTableColumnFlags_WidthFixed, em * 1.4f);
    ImGui::TableSetupColumn ("Fu", ImGuiTableColumnFlags_WidthFixed, em * 1.4f);
    ImGui::TableHeadersRow ();
    for (size_t c = 0; c < f.classes.size (); ++c) {
        pl::SizeClass& sc = f.classes[c];
        ImGui::PushID (int (c));
        ImGui::TableNextRow ();
        ImGui::TableNextColumn ();
        ImGui::Checkbox ("##on", &sc.on);
        hudshell::ItemTip ("Check this class.");
        ImGui::TableNextColumn ();
        const int rank = RankOf (f.result, int (c));
        Swatch (rank >= 0 ? pl::SizeClassRgba (rank) : 0x00000000u);
        ImGui::TableNextColumn ();
        char name[48];
        std::snprintf (name, sizeof (name), "%s", sc.name.c_str ());
        ImGui::SetNextItemWidth (-1);
        if (ImGui::InputText ("##name", name, sizeof (name)))
            sc.name = name;
        ImGui::TableNextColumn ();
        ImGui::SetNextItemWidth (-1);
        if (ImGui::DragFloat ("##spread", &sc.spread, 0.05f, 0.1f, 30.0f, "%.1f m"))
            sc.spread = std::clamp (sc.spread, 0.1f, 30.0f);
        hudshell::ItemTip ("Mature crown diameter.");
        ImGui::TableNextColumn ();
        ImGui::SetNextItemWidth (-1);
        if (ImGui::DragFloat ("##height", &sc.height, 0.05f, 0.1f, 40.0f, "%.1f m"))
            sc.height = std::clamp (sc.height, 0.1f, 40.0f);
        hudshell::ItemTip ("Mature height.");
        for (int l = 0; l < pl::kLightClassCount; ++l) {
            ImGui::TableNextColumn ();
            ImGui::PushID (l);
            ImGui::Checkbox ("##light", &sc.light[size_t (l)]);
            hudshell::ItemTip ("Accepts %s.", pl::LightName (pl::LightClass (l)));
            ImGui::PopID ();
        }
        ImGui::PopID ();
    }
    ImGui::EndTable ();
    if (ImGui::SmallButton ("Default classes##fit"))
        f.classes = pl::DefaultSizeClasses ();
    hudshell::ItemTip ("Small 1.5 m across and 1.5 m tall, Medium 4 m and 6 m, Large 10 m and 15 m, every light "
                       "class. Proposed templates, not catalogue data.");
}

void Legend (const pl::FitState& f)
{
    for (size_t r = 0; r < f.result.order.size (); ++r) {
        Swatch (pl::SizeClassRgba (int (r)));
        ImGui::SameLine ();
        ImGui::TextUnformatted (f.classes[size_t (f.result.order[r])].name.c_str ());
        ImGui::SameLine ();
    }
    Swatch (pl::kSizeNoneRgba);
    ImGui::SameLine ();
    ImGui::TextUnformatted ("none fits");
    ImGui::SameLine ();
    Swatch (pl::kSizeUnknownRgba);
    ImGui::SameLine ();
    ImGui::TextUnformatted ("not known");
    hudshell::ItemTip ("Each sample shows the largest class that fits there. Classes are checked one by one, not "
                       "assumed nested. \"Not known\": the room was not measured, or the crown is wider than the "
                       "12 m measured.");
}

void AreaTable (const pl::FitState& f)
{
    const auto& r = f.result;
    const bool areas = f.site.area.size () == f.site.Size ();
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg;
    if (!ImGui::BeginTable ("##fit-areas", 1 + int (std::size (kShownReasons)), flags))
        return;
    ImGui::TableSetupColumn (areas ? "m2" : "samples", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    for (pl::SizeReason reason : kShownReasons)
        ImGui::TableSetupColumn (pl::SizeReasonName (reason), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow ();
    for (int c : r.order) {
        ImGui::TableNextRow ();
        ImGui::TableNextColumn ();
        ImGui::TextUnformatted (f.classes[size_t (c)].name.c_str ());
        for (pl::SizeReason reason : kShownReasons) {
            ImGui::TableNextColumn ();
            const double v = r.area[size_t (c)][size_t (reason)];
            if (v > 0)
                ImGui::Text (areas ? "%.1f" : "%.0f", v);
            else
                ImGui::TextDisabled ("-");
        }
    }
    ImGui::EndTable ();
    ImGui::TextDisabled ("%s evaluated: %.1f %s. A class's row sums to it; one reason per sample, checked in this "
                         "order: light, no-go zone, overhang, wall.",
                         f.site.Size () == 0 ? "Nothing" : "Region", r.measured, areas ? "m2" : "samples");
}

void Here (const HudState& state, pl::FitState& f)
{
    f.previewRow = SIZE_MAX;
    const pl::SchemeState& s = state.planting.scheme;
    if (state.sunReadingState != 1 || state.sunReadingStudy != s.siteStudy) {
        ImGui::TextDisabled ("Hover a measured point of this layer to see why each class does or does not fit.");
        return;
    }
    size_t row = f.site.Size ();
    for (size_t i = 0; i < f.site.Size (); ++i)
        if (f.site.SourceOf (i) == state.sunReadingSample) {
            row = i;
            break;
        }
    if (row == f.site.Size () || f.result.reason.empty ()) {
        ImGui::TextDisabled ("The hovered point is outside the region.");
        return;
    }
    f.previewRow = row;
    std::string text = "Here:";
    for (int c : f.result.order)
        text += " " + f.classes[size_t (c)].name + " " +
                pl::SizeReasonName (f.result.ReasonAt (row, size_t (c), f.classes.size ())) + ";";
    text.back () = '.';
    if (s.site.clearance.Count () == s.site.Size () && s.site.Size () > 0) {
        const size_t at = f.site.SourceOf (row);
        const float above = s.site.clearance.overhead[at];
        char room[96];
        if (above >= 0.0f)
            std::snprintf (room, sizeof (room), " Overhead %.1f m.", double (above));
        else
            std::snprintf (room, sizeof (room), " Open above.");
        text += room;
    }
    ImGui::TextWrapped ("%s", text.c_str ());
}

} // namespace

void DrawFitTab (HudState& state)
{
    pl::PlantingHud& hud = state.planting;
    hud.highlightMask = 0;
    pl::FitState& f = hud.fit;
    f.tabOpen = true;
    const int layer = RefreshSchemeRegion (hud);
    RefreshFit (hud);

    DrawSchemeRegion (hud, layer);
    ImGui::SeparatorText ("Size classes");
    ClassTable (f);
    ImGui::SeparatorText ("Largest that fits");
    ImGui::Checkbox ("Show in viewer##fit", &f.show);
    hudshell::ItemTip ("Colour every sample of the region by the largest class that fits, while this sub-tab is open.");
    ImGui::SameLine ();
    ImGui::Checkbox ("Envelopes at the cursor##fit", &f.preview);
    hudshell::ItemTip ("At the hovered point, draw each class's mature crown as a cylinder: in its colour where it "
                       "fits, red-brown where it does not. Shows what an overhang or a wall is in the way of.");
    Legend (f);
    ImGui::NewLine ();
    AreaTable (f);
    Here (state, f);
    if (!f.result.spaceChecked)
        ImGui::TextColored (ImVec4 (0.95f, 0.72f, 0.30f, 1.0f),
                            "Walls and overhangs are not measured yet: no class is known to fit.");
    ImGui::TextDisabled ("Above-ground fit only: rooting, soil depth and utilities are not assessed. Light and space "
                         "are checked separately; tick all three light classes of a class to leave light out.");
}

} // namespace geomsrv::archviz::plantinghud
