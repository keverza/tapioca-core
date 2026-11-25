// The Planting tab's results (#278 Stage 1C): layer switch, metric, legend and
// colours, the area table, the point inspector and the report row; and the
// Plants and Compare sub-tabs, laid out ahead of their stages.
//
// ⚠️ LATER STAGES ARE DRAWN DISABLED, never hidden (DiligentHudPlanting.cpp):
// the area figures, the planting report page, exports, the catalogue and the
// comparison say what they will be, and change nothing.
//
// RENDER THREAD. Reads HudState and the scene's overlay status only.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudPlantingParts.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/GradientPresetChoice.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/SunStudyPreview.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace geomsrv::archviz::plantinghud {

namespace pl = planting;

namespace {

constexpr uint32_t kHatchLight = 0xBDBDBDFFu; // kArchVizSunTintPS's UnavailableHatchColor stripes
constexpr uint32_t kHatchDark = 0x949494FFu;

} // namespace

ImU32 AbgrOf (uint32_t rgba)
{
    return IM_COL32 ((rgba >> 24) & 0xff, (rgba >> 16) & 0xff, (rgba >> 8) & 0xff, rgba & 0xff);
}

// The unavailable state's legend swatch: the map's own diagonal hatch.
void DrawHatchSwatch (float size)
{
    const ImVec2 min = ImGui::GetCursorScreenPos ();
    const ImVec2 max (min.x + size, min.y + size);
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    draw->AddRectFilled (min, max, AbgrOf (kHatchLight));
    draw->PushClipRect (min, max, true);
    const float stripe = size / 4.0f;
    for (float x = -size; x < size; x += 2.0f * stripe)
        draw->AddQuadFilled ({ min.x + x, max.y }, { min.x + x + stripe, max.y }, { min.x + x + stripe + size, min.y },
                             { min.x + x + size, min.y }, AbgrOf (kHatchDark));
    draw->PopClipRect ();
    ImGui::Dummy (ImVec2 (size, size));
}

std::string ClassRangeOf (const pl::PlantingHud& hud, int cls)
{
    const std::vector<float> edges = pl::ClassEdges (hud);
    char text[48];
    if (cls == 0)
        std::snprintf (text, sizeof (text), "under %.4g h", edges.front ());
    else if (cls == int (edges.size ()))
        std::snprintf (text, sizeof (text), "%.4g h or more", edges.back ());
    else
        std::snprintf (text, sizeof (text), "%.4g to %.4g h", edges[size_t (cls - 1)], edges[size_t (cls)]);
    return text;
}

namespace {

void LayerRow (HudState& state, const SunStudyOverlayStatus& study)
{
    pl::PlantingHud& hud = state.planting;
    ImGui::TextUnformatted ("Layer");
    for (size_t i = 0; i < hud.layers.size (); ++i) {
        const pl::Layer& layer = hud.layers[i];
        const bool shown = study.drawing && study.studyId == layer.studyId;
        if (shown)
            hud.shownLayer = int (i);
        ImGui::SameLine ();
        ImGui::PushID (int (i));
        ImGui::PushStyleColor (ImGuiCol_Button,
                               ImGui::GetStyleColorVec4 (shown ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
        char label[24];
        std::snprintf (label, sizeof (label), "%.2f m", layer.height);
        ImGui::BeginDisabled (hud.runningRun != 0 || hud.showingRun != 0);
        if (ImGui::SmallButton (label) && !shown)
            ShowLayer (state, i);
        ImGui::EndDisabled ();
        ImGui::PopStyleColor ();
        hudshell::ItemTip (shown ? "On screen."
                                 : "Show this layer again: kept, not traced again. Month views and playback stay "
                                   "with the newest period.");
        ImGui::PopID ();
    }
    if (hud.showingRun != 0) {
        ImGui::SameLine ();
        ImGui::TextDisabled ("showing...");
    }
    ImGui::SameLine ();
    ImGui::BeginDisabled (hud.runningRun != 0 || hud.showingRun != 0);
    if (ImGui::SmallButton ("Clear##plantinglayers"))
        ClearLayers (state);
    ImGui::EndDisabled ();
    hudshell::ItemTip ("Forget every layer and free the studies kept for them (at most %d).", int (pl::kLayersKept));
}

void LegendSection (HudState& state)
{
    pl::PlantingHud& hud = state.planting;
    const float swatch = ImGui::GetFrameHeight () * 0.75f;
    if (hud.metric == pl::kSunClass) {
        gradientchoice::GradientPresetChoice ("Colours##plantingclass", hud.classPreset, hud.classCustom,
                                              hud.classFlipped, ImGui::GetContentRegionAvail ().x * 0.6f,
                                              overlaylayers::GradientCategory::Sun, "Class swatches");
        hudshell::ItemTip ("Class swatches: one colour per class, below. Or any gradient, sampled at each class.");
        const std::vector<uint32_t> colours = ShownClassColours (hud);
        const int first = hud.deepShade ? 0 : 1;
        for (int cls = 0; cls < int (colours.size ()); ++cls) {
            ImGui::PushID (cls);
            uint32_t rgba = colours[size_t (cls)];
            if (hud.classPreset.empty ()) {
                if (gradientchoice::ColourChoice ("##class", rgba, false))
                    hud.classRgba[std::clamp (cls + first, 0, pl::kMaxClasses - 1)] = rgba;
                hudshell::ItemTip ("Pick this class's colour. Display only: no study reruns.");
            }
            else
                ImGui::ColorButton ("##class", ImGui::ColorConvertU32ToFloat4 (AbgrOf (rgba)),
                                    ImGuiColorEditFlags_NoTooltip, ImVec2 (swatch, swatch));
            ImGui::SameLine ();
            ImGui::Text ("%s, %s", pl::ClassName (hud, cls), ClassRangeOf (hud, cls).c_str ());
            ImGui::PopID ();
        }
        if (hud.classPreset.empty () && ImGui::SmallButton ("Reset colours##planting")) {
            const pl::PlantingHud defaults;
            std::copy (std::begin (defaults.classRgba), std::end (defaults.classRgba), hud.classRgba);
        }
    }
    else {
        gradientchoice::GradientPresetChoice ("Colours##plantinghours", hud.hoursPreset, hud.hoursCustom,
                                              hud.hoursFlipped, ImGui::GetContentRegionAvail ().x * 0.6f,
                                              overlaylayers::GradientCategory::Sun);
        const ImVec2 min = ImGui::GetCursorScreenPos ();
        const float width = ImGui::GetContentRegionAvail ().x;
        const ImVec2 max (min.x + width, min.y + swatch);
        gradientchoice::DrawSwatch (ImGui::GetWindowDrawList (), min, max,
                                    SunStudyRampStops (hud.hoursPreset, hud.hoursCustom, hud.hoursFlipped));
        ImGui::Dummy (ImVec2 (width, swatch));
        ImGui::TextDisabled ("0 h");
        ImGui::SameLine (width - ImGui::CalcTextSize ("9 h+").x);
        ImGui::TextDisabled ("9 h+");
    }
    DrawHatchSwatch (swatch);
    ImGui::SameLine ();
    ImGui::TextUnformatted ("Unavailable");
    hudshell::ItemTip ("Too steep to plant, or under a ceiling (a balcony) closer than 0.10 m above the layer. Not "
                       "shade: it is left out of the class areas.");
}

// One area figure: m2, or its share of the row's measured area.
void AreaCell (double area, double measured, bool share)
{
    if (share && measured > 0.0)
        ImGui::Text ("%.0f%%", area / measured * 100.0);
    else
        ImGui::Text (area >= 100.0 ? "%.0f" : "%.1f", area);
}

void AreaRow (const char* name, const char* tip, const evp::sunstudy::PlantingAreaRow& row, int classes, bool share)
{
    ImGui::TableNextRow ();
    ImGui::TableNextColumn ();
    ImGui::TextUnformatted (name);
    if (tip != nullptr && tip[0] != 0)
        hudshell::ItemTip ("%s", tip);
    for (int cls = 0; cls < classes; ++cls) {
        ImGui::TableNextColumn ();
        AreaCell (size_t (cls) < row.classAreas.size () ? row.classAreas[size_t (cls)] : 0.0, row.evaluatedArea, share);
    }
    ImGui::TableNextColumn ();
    const double unavailable = row.UnavailableArea ();
    ImGui::Text (unavailable >= 100.0 ? "%.0f" : "%.1f", unavailable);
    if (unavailable > 0.0)
        hudshell::ItemTip ("Unavailable, never shade:\n%.1f m2 too steep\n%.1f m2 under a ceiling\n%.1f m2 no host",
                           row.steepArea, row.blockedArea, row.invalidArea);
}

void AreaTable (HudState& state, const SunStudyOverlayStatus& study)
{
    pl::PlantingHud& hud = state.planting;
    if (!ImGui::CollapsingHeader ("Areas (plan m2)", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    const bool planting =
        study.drawing && std::any_of (hud.layers.begin (), hud.layers.end (),
                                      [&] (const pl::Layer& layer) { return layer.studyId == study.studyId; });
    if (!planting) {
        ImGui::TextDisabled ("Run a layer, or show one, to measure its areas.");
        return;
    }
    if (hud.runningRun != 0 && hud.areas.studyId != study.studyId) {
        ImGui::TextDisabled ("Measured when the run finishes.");
        return;
    }
    RefreshAreas (state, study.studyId, study.version);
    const auto& areas = hud.areas;
    if (!areas.error.empty ()) {
        hudshell::ErrorNotice (areas.error);
        return;
    }
    const int classes = int (areas.edges.size ()) + 1;
    const ImGuiTableFlags flags =
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable ("##planting-areas", classes + 2, flags)) {
        ImGui::TableSetupColumn ("Host", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        for (int cls = 0; cls < classes; ++cls)
            ImGui::TableSetupColumn (pl::ClassName (hud, cls));
        ImGui::TableSetupColumn ("N/A");
        ImGui::TableHeadersRow ();
        for (size_t i = 0; i < areas.table.hosts.size (); ++i) {
            ImGui::PushID (int (i));
            const std::string& guid = i < areas.hostGuids.size () ? areas.hostGuids[i] : std::string ();
            AreaRow (pl::HostLabel (i, guid).c_str (), guid.c_str (), areas.table.hosts[i], classes,
                     hud.areas.showShares);
            ImGui::PopID ();
        }
        if (areas.table.hosts.size () != 1)
            AreaRow ("All hosts", nullptr, areas.table.total, classes, hud.areas.showShares);
        ImGui::EndTable ();
    }
    ImGui::Checkbox ("Shares##plantingareas", &hud.areas.showShares);
    hudshell::ItemTip ("Each class as a percent of the row's measured area; N/A stays in m2.");
    ImGui::SameLine ();
    ImGui::TextDisabled ("%.1f m2 measured, %s", areas.table.total.evaluatedArea,
                         areas.period ? "mean hours a day" : "hours of the day");
    hudshell::ItemTip ("Exact classes from the raw hours: a value at an edge belongs to the upper class. Plan area. "
                       "Unavailable area (N/A) is never counted as shade.");
    if (areas.table.total.unclassified > 0)
        ImGui::TextDisabled ("%zu sample(s) without a value left out.", areas.table.total.unclassified);
}

void InspectSection (HudState& state)
{
    const pl::PlantingHud& hud = state.planting;
    char first[96] = {}, second[96] = {};
    if (state.sunReadingState == 1) {
        const int cls = pl::ClassOf (hud, state.sunReadingHours);
        std::snprintf (first, sizeof (first), "%s%.2f h direct sun a day", state.sunReadingNearest ? "~ " : "",
                       state.sunReadingHours);
        std::snprintf (second, sizeof (second), "%s (%s)", pl::ClassName (hud, cls), ClassRangeOf (hud, cls).c_str ());
    }
    else if (state.sunReadingState == 2)
        std::snprintf (first, sizeof (first), "not measured here");
    DrawSunStudyInspectControl (state.sunInspect, first, second);
    LaterButton ("Monthly profile", "Later: this point's hours month by month, at every layer.");
    ImGui::SameLine ();
    LaterButton ("Find plants here", "Later: catalogue plants whose light needs this point meets.");
}

void ReportRow (HudState& state, const DiligentSceneStats& scene)
{
    if (!ImGui::CollapsingHeader ("Report", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    viewerhud::DrawCaptureRow (state, kShotSunStudy, false);
    if (state.captureFrame.active && !state.captureFrame.builder) {
        auto& f = state.captureFrame;
        f.entryMode = 0; // the hours map, in this tab's classes or ramp
        f.entryStep = 0;
        f.pinnedStudy = scene.sunStudy.studyId;
        f.pinnedVersion = scene.sunStudy.version;
    }
    LaterButton ("Export CSV", "Later: the area table and per-sample hours as CSV.");
    ImGui::SameLine ();
    LaterButton ("Bake zones", "Later: approved class zones as fills on a dedicated layer. Never automatic.");
}

} // namespace

void DrawResults (HudState& state, const DiligentSceneStats& scene)
{
    pl::PlantingHud& hud = state.planting;
    SunStudyOverlayStatus study = scene.sunStudy;
    study.drawing = study.drawing && study.analysisKind == 0;
    if (!ImGui::CollapsingHeader ("Results", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    if (hud.layers.empty ())
        ImGui::TextDisabled ("Run a layer to see its light classes.");
    else
        LayerRow (state, study);
    // The shown layer's own period, kept with it: an earlier layer is shown
    // from its stored mean hours a day, not from the newest period's totals.
    const auto shownLayer = std::find_if (hud.layers.begin (), hud.layers.end (), [&] (const pl::Layer& layer) {
        return study.drawing && layer.studyId == study.studyId;
    });
    state.sunShowsPeriod = shownLayer != hud.layers.end () && shownLayer->period;
    if (state.sunShowsPeriod)
        ImGui::TextDisabled ("%s: mean a day", shownLayer->label.c_str ());
    ImGui::RadioButton ("Sun class##plantingmetric", &hud.metric, pl::kSunClass);
    ImGui::SameLine ();
    ImGui::RadioButton ("Hours a day##plantingmetric", &hud.metric, pl::kHoursADay);
    ImGui::SameLine ();
    ImGui::BeginDisabled ();
    int later = hud.metric;
    ImGui::RadioButton ("Days in sun##plantingmetric", &later, pl::kDaysAtSun);
    ImGui::EndDisabled ();
    hudshell::ItemTip ("Later: the share of the period's days at or above the full-sun edge (consistency).");
    LegendSection (state);
    AreaTable (state, study);
    InspectSection (state);
    ReportRow (state, scene);
}

void DrawPlantsTab (HudState& state)
{
    pl::PlantingHud& hud = state.planting;
    ImGui::TextDisabled ("Plant catalogue: Stage 3.");
    LaterButton ("Pick plant catalogue...",
                 "Later: a local plant list (JSON) with each species' light class, mature height and spread.");
    ImGui::SameLine ();
    ImGui::TextDisabled ("%s", hud.catalogPath.empty () ? "no catalogue" : hud.catalogPath.c_str ());
    ImGui::BeginDisabled ();
    int size = 0;
    ImGui::SetNextItemWidth (-ImGui::GetFontSize () * 6.0f);
    ImGui::Combo ("plant size##planting", &size, "All\0Ground cover\0Low\0Shrub\0Tree\0");
    int fit = 0;
    ImGui::RadioButton ("Light only##plantingfit", &fit, 0);
    ImGui::SameLine ();
    ImGui::RadioButton ("Space##plantingfit", &fit, 1);
    ImGui::SameLine ();
    ImGui::RadioButton ("Combined##plantingfit", &fit, 2);
    ImGui::EndDisabled ();
    hudshell::ItemTip ("Later: where each plant size fits by light, by space for its mature crown, or both.");
    if (ImGui::BeginTable ("##planting-plants", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn ("Plant");
        ImGui::TableSetupColumn ("Light");
        ImGui::TableSetupColumn ("Size");
        ImGui::TableSetupColumn ("Fits");
        ImGui::TableHeadersRow ();
        ImGui::TableNextRow ();
        ImGui::TableNextColumn ();
        ImGui::TextDisabled ("no catalogue loaded");
        ImGui::EndTable ();
    }
    LaterButton ("Show mature envelope", "Later: the selected plant's mature height and spread at the point.");
}

void DrawCompareTab (HudState& state)
{
    (void) state;
    ImGui::TextDisabled ("Scenario comparison: Stage 4.");
    ImGui::BeginDisabled ();
    int a = 0, b = 1;
    ImGui::SetNextItemWidth (-ImGui::GetFontSize () * 6.0f);
    ImGui::Combo ("before##planting", &a, "Existing\0Proposed\0");
    ImGui::SetNextItemWidth (-ImGui::GetFontSize () * 6.0f);
    ImGui::Combo ("after##planting", &b, "Existing\0Proposed\0");
    int view = 0;
    ImGui::RadioButton ("Class change##plantingcompare", &view, 0);
    ImGui::SameLine ();
    ImGui::RadioButton ("Hours lost##plantingcompare", &view, 1);
    ImGui::EndDisabled ();
    hudshell::ItemTip ("Later: two layers or two massing options side by side, and where a class was lost.");
    LaterButton ("Compare", "Later: needs two finished studies of the same hosts and layer.");
}

} // namespace geomsrv::archviz::plantinghud

namespace geomsrv::archviz {

// The capture legend's colour column for a planting map (#278): the classes as
// flat swatches with their ranges, then the unavailable hatch. The strip's
// left columns -- mini plan, north arrow, location and period -- stay the sun
// study's own (DrawSunStudyCaptureLegend).
void DrawPlantingCaptureColours (const HudState& state)
{
    const planting::PlantingHud& hud = state.planting;
    ImGui::TextUnformatted ("Light class, direct sun a day");
    const std::vector<uint32_t> colours = plantinghud::ShownClassColours (hud);
    const float size = ImGui::GetTextLineHeight ();
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    for (int cls = int (colours.size ()) - 1; cls >= 0; --cls) {
        if (cls != int (colours.size ()) - 1)
            ImGui::SameLine (0.0f, size);
        const ImVec2 at = ImGui::GetCursorScreenPos ();
        draw->AddRectFilled (at, ImVec2 (at.x + size, at.y + size), plantinghud::AbgrOf (colours[size_t (cls)]));
        ImGui::Dummy (ImVec2 (size, size));
        ImGui::SameLine ();
        ImGui::Text ("%s, %s", planting::ClassName (hud, cls), plantinghud::ClassRangeOf (hud, cls).c_str ());
    }
    plantinghud::DrawHatchSwatch (size);
    ImGui::SameLine ();
    ImGui::TextUnformatted ("Unavailable: too steep, or under a ceiling; never shade");
}

} // namespace geomsrv::archviz
