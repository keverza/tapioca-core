// The Analysis page's Planting tab (#278 Stage 1C): planting suitability as
// light by layer height. One vertical panel -- areas, period, layers, classes,
// climate, run, results, areas table, inspector, report -- with the Plants and
// Compare sub-tabs laid out ahead of their stages.
//
// ⚠️ THE LAYOUT IS AHEAD OF THE FUNCTIONALITY ON PURPOSE. Controls for later
// stages (EPW weather, plant catalogue, polyline zones, comparison, the
// planting report page, exports) are drawn DISABLED with a tip naming what they
// will do, so the panel's shape can be reviewed and tested now. A disabled
// control never changes a study.
//
// RENDER THREAD, like the rest of the HUD. Runs go through the HUD sun runner
// (NativeCommands/SunStudyHudRunner.hpp), one study per ticked height; the
// form, queue and colours are planting::PlantingHud (PlantingHudModel.hpp).
//
// The tint is the sun study's shared channel. While this tab is open it writes
// the shared hours display (stepped class bands, or the hours ramp) and holds
// the Sun tab's own settings; ReleasePlantingDisplay puts them back.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudPlantingParts.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/GradientPresetChoice.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudShellTabs.hpp"
#include "ArchViz/SunStudyPreview.hpp"
#include "ArchViz/TapiocaUI/Foundation/AnalysisRunControl.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "SunStudy/SunStudyBackend.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace geomsrv::archviz {

namespace pl = planting;
static_assert (pl::kLayersKept == sunstudyhud::kPlantingLayersKept, "the tab's count is the runner's");

namespace {

constexpr int kSunViewHours = 1; // DiligentHudSunStudy.cpp's "Sunstudy" view: the hours map
constexpr const char* kMonthNames[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

float LabelColumn ()
{
    return ImGui::GetFontSize () * 6.0f;
}

// ---- the shared tint --------------------------------------------------------

} // namespace

bool plantinghud::LaterButton (const char* label, const char* tip, ImVec2 size)
{
    ImGui::BeginDisabled ();
    ImGui::Button (label, size);
    ImGui::EndDisabled ();
    hudshell::ItemTip ("%s", tip);
    return false;
}

std::vector<uint32_t> plantinghud::ShownClassColours (const pl::PlantingHud& hud)
{
    const int count = pl::ClassCount (hud);
    std::vector<uint32_t> colours;
    if (hud.classPreset.empty ()) {
        for (int cls = 0; cls < count; ++cls)
            colours.push_back (pl::ClassRgba (hud, cls));
    }
    else {
        const auto stops = SunStudyRampStops (hud.classPreset, hud.classCustom, hud.classFlipped);
        for (const auto& stop : pl::ClassStops (hud, kSunRampTop))
            colours.push_back (gradientchoice::ColourAt (stops, stop.at));
    }
    // A selected plant's highlight (Plants tab): the classes it does not accept, dimmed.
    if (hud.highlightMask != 0)
        for (size_t cls = 0; cls < colours.size (); ++cls)
            if (!(hud.highlightMask & (1u << cls)))
                colours[cls] = pl::DimmedRgba (colours[cls]);
    return colours;
}

namespace {

using plantinghud::LaterButton;

// Hold the Sun tab's display once, then draw this tab's.
void ApplyPlantingDisplay (HudState& state)
{
    auto& hold = state.planting.sunHold;
    if (!hold.held) {
        hold.held = true;
        hold.preset = state.sunGradientPreset;
        hold.flipped = state.sunGradientFlipped;
        hold.custom = state.sunGradientCustom;
        hold.stepped = state.sunGradientStepped;
        hold.bands = state.sunBandCount;
        hold.edges.assign (std::begin (state.sunBandEdges), std::end (state.sunBandEdges));
        hold.lowBlue = state.sunLowBlue;
        hold.view = state.sunView;
    }
    const pl::PlantingHud& hud = state.planting;
    state.sunView = kSunViewHours;
    state.sunLowBlue = false;
    if (hud.metric == pl::kSunClass) {
        int count = 0;
        pl::ClassBands (hud, kSunRampTop, count, state.sunBandEdges, int (std::size (state.sunBandEdges)));
        state.sunBandCount = std::clamp (count, kSunBandsMin, kSunBandsMax);
        state.sunGradientStepped = true;
        state.sunGradientFlipped = false;
        auto stops = pl::ClassStops (hud, kSunRampTop);
        const auto colours = plantinghud::ShownClassColours (hud);
        for (size_t i = 0; i < stops.size () && i < colours.size (); ++i)
            stops[i].rgba = colours[i];
        state.sunGradientPreset = overlaylayers::kCustomGradientId;
        state.sunGradientCustom = std::move (stops);
        return;
    }
    state.sunGradientStepped = false;
    state.sunGradientPreset = hud.hoursPreset;
    state.sunGradientFlipped = hud.hoursFlipped;
    state.sunGradientCustom = hud.hoursCustom;
}

// ---- the run queue ----------------------------------------------------------

void SubmitNextHeight (HudState& state)
{
    pl::PlantingHud& hud = state.planting;
    if (hud.pending.empty ())
        return;
    const float height = hud.pending.front ();
    hud.pending.erase (hud.pending.begin ());
    const pl::SeasonDates dates = pl::DatesOf (hud.season, hud.custom);
    sunstudyhud::Request request;
    request.year = hud.year;
    request.month = dates.fromMonth;
    request.day = dates.fromDay;
    request.period = dates.singleDay ? "day" : dates.wholeYear ? "year" : "period";
    request.toMonth = dates.toMonth;
    request.toDay = dates.toDay;
    request.dailyResolved = hud.everyDay;
    request.timestep = pl::kTimestepMinutes[std::clamp (hud.timestep, 0, pl::kTimestepCount - 1)];
    request.grid = hud.grid;
    request.contextElements = hud.context;
    request.backend = evp::sunstudy::SunStudyBackendForChoice (hud.backend);
    request.receptorHeight = height;
    request.receptorMinHostUp = hud.minHostUp;
    request.analysisElements = hud.hosts;
    uint64_t accepted = 0;
    hud.runError.clear ();
    if (!sunstudyhud::Submit (request, hud.runError, &accepted)) {
        hud.pending.clear ();
        hud.runningRun = 0;
        hud.runningHeight = -1.0f;
        return;
    }
    hud.runningRun = accepted;
    hud.runningHeight = height;
}

// ---- sections ---------------------------------------------------------------

// One role list over the viewer's selection, as the Sun tab's Roles rows:
// putting an element in one list takes it out of the other.
void RoleRow (const char* label, const char* tip, std::vector<std::string>& role, std::vector<std::string>& other,
              const std::vector<std::string>& selection)
{
    ImGui::PushID (label);
    ImGui::Text ("%s  %u element(s)", label, unsigned (role.size ()));
    hudshell::ItemTip ("%s", tip);
    ImGui::BeginDisabled (selection.empty ());
    const auto take = [&] (bool replace) {
        if (replace)
            role.clear ();
        for (const auto& guid : selection) {
            if (std::find (role.begin (), role.end (), guid) == role.end ())
                role.push_back (guid);
            other.erase (std::remove (other.begin (), other.end (), guid), other.end ());
        }
    };
    if (ImGui::SmallButton ("Replace"))
        take (true);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Add"))
        take (false);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Remove"))
        for (const auto& guid : selection)
            role.erase (std::remove (role.begin (), role.end (), guid), role.end ());
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Clear"))
        role.clear ();
    ImGui::PopID ();
}

void AreasSection (HudState& state)
{
    pl::PlantingHud& hud = state.planting;
    if (!ImGui::CollapsingHeader ("Planting areas", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    RoleRow ("Hosts",
             "The planting beds measured: terrain, slabs, roofs. Select them in the viewer, then Replace or Add.",
             hud.hosts, hud.context, state.visibilitySelection);
    RoleRow ("Context", "Casts shadow and is never measured: buildings, walls, balconies, existing trees.", hud.context,
             hud.hosts, state.visibilitySelection);
    ImGui::TextDisabled ("Everything else casts shadow too.");
    LaterButton ("Zones from polylines...",
                 "Later: closed polylines or fills projected onto the hosts as named zones, each with its own area "
                 "table row.");
    ImGui::SameLine ();
    LaterButton ("Pick in viewer", "Later: click host surfaces in the viewer instead of selecting elements.");
}

void PeriodSection (pl::PlantingHud& hud)
{
    if (!ImGui::CollapsingHeader ("Period", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::SetNextItemWidth (-LabelColumn ());
    if (ImGui::BeginCombo ("season##planting", pl::SeasonName (hud.season))) {
        for (int season = 0; season < pl::kSeasonCount; ++season) {
            if (ImGui::Selectable (pl::SeasonName (season), season == hud.season))
                hud.season = season;
            hudshell::ItemTip ("%s", pl::SeasonTip (season));
        }
        ImGui::EndCombo ();
    }
    hudshell::ItemTip ("%s", pl::SeasonTip (hud.season));
    if (hud.season == pl::kCustom) {
        const auto dateRow = [] (const char* id, int& month, int& day) {
            ImGui::PushID (id);
            ImGui::SetNextItemWidth (ImGui::GetFontSize () * 4.5f);
            int index = std::clamp (month, 1, 12) - 1;
            if (ImGui::Combo ("##month", &index, kMonthNames, 12))
                month = index + 1;
            ImGui::SameLine ();
            ImGui::SetNextItemWidth (ImGui::GetFontSize () * 5.0f);
            ImGui::SliderInt ("##day", &day, 1, 31);
            ImGui::SameLine ();
            ImGui::TextUnformatted (id);
            ImGui::PopID ();
        };
        dateRow ("from", hud.custom.fromMonth, hud.custom.fromDay);
        dateRow ("to", hud.custom.toMonth, hud.custom.toDay);
    }
    ImGui::SetNextItemWidth (-LabelColumn ());
    ImGui::InputInt ("year##planting", &hud.year, 1, 10);
    hud.year = std::clamp (hud.year, 1, 9999);
    const pl::SeasonDates dates = pl::DatesOf (hud.season, hud.custom);
    const int days = pl::SeasonDays (dates, hud.year);
    if (days < 1)
        hudshell::ErrorNotice ("The period ends before it starts");
    else
        ImGui::TextDisabled ("%s %d to %s %d, %d day(s)", kMonthNames[dates.fromMonth - 1], dates.fromDay,
                             kMonthNames[dates.toMonth - 1], dates.toDay, days);
    if (dates.singleDay)
        return;
    int sampling = hud.everyDay ? 1 : 0;
    ImGui::RadioButton ("Preview##plantingsampling", &sampling, 0);
    hudshell::ItemTip ("One middle day per month, weighted by the days it stands for: an estimate.");
    ImGui::SameLine ();
    char every[40];
    std::snprintf (every, sizeof (every), "Every day (%d)##plantingsampling", days);
    ImGui::RadioButton (every, &sampling, 1);
    hudshell::ItemTip ("Every day traced: the reference, and about %d times the preview's work.",
                       (std::max) (1, days / (std::max) (1, (std::min) (12, days))));
    hud.everyDay = sampling == 1;
}

void LayersSection (pl::PlantingHud& hud)
{
    if (!ImGui::CollapsingHeader ("Layers", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::TextDisabled ("Light measured this high above each host, facing up.");
    for (int slot = 0; slot < pl::kHeightCount; ++slot) {
        ImGui::PushID (slot);
        ImGui::Checkbox ("##on", &hud.heightOn[slot]);
        ImGui::SameLine ();
        ImGui::SetNextItemWidth (ImGui::GetFontSize () * 5.0f);
        ImGui::DragFloat ("##height", &hud.heights[slot], 0.01f, 0.0f, pl::kMaxHeight, "%.2f m");
        hud.heights[slot] = std::clamp (hud.heights[slot], 0.0f, pl::kMaxHeight);
        ImGui::SameLine ();
        ImGui::TextUnformatted (pl::HeightName (slot));
        if (std::fabs (hud.heights[slot] - pl::kDefaultHeights[slot]) > 0.001f) {
            ImGui::SameLine ();
            if (ImGui::SmallButton ("reset"))
                hud.heights[slot] = pl::kDefaultHeights[slot];
        }
        ImGui::PopID ();
    }
    hudshell::ItemTip ("A ceiling closer than 0.10 m above a layer (a balcony over a bed) makes that point "
                       "unavailable, drawn hatched; it is never counted as shade.");
}

void ClassesSection (HudState& state)
{
    if (!ImGui::CollapsingHeader ("Light classes", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    plantinghud::EnsureCatalogue (state);
    pl::PlantingHud& hud = state.planting;
    const auto& profiles = hud.catalogue.profiles;
    const auto active =
        std::find_if (profiles.begin (), profiles.end (), [&] (const pl::Profile& p) { return p.id == hud.profileId; });
    ImGui::SetNextItemWidth (-LabelColumn ());
    if (ImGui::BeginCombo ("profile##planting", active == profiles.end () ? "custom" : active->name.c_str ())) {
        for (const auto& profile : profiles)
            if (ImGui::Selectable (profile.name.c_str (), profile.id == hud.profileId))
                pl::ApplyProfile (hud, profile);
        if (profiles.empty ())
            ImGui::TextDisabled ("no profiles: see the Plants tab");
        ImGui::EndCombo ();
    }
    if (active != profiles.end ())
        hudshell::ItemTip ("%s%s%s\nPicking a profile sets its class edges%s. Profiles live in profiles.json "
                           "(Plants tab, Open folder). An exact edge belongs to the upper class.",
                           active->source.c_str (),
                           active->seasonNote.empty () ? "" : "\nSeason: ", active->seasonNote.c_str (),
                           active->hasSeason ? ", its season" : "");
    ImGui::SetNextItemWidth (-LabelColumn ());
    float edges[2] = { hud.partialEdge, hud.sunEdge };
    if (ImGui::DragFloat2 ("edges h##planting", edges, 0.05f, 0.0f, 24.0f, "%.2f h")) {
        hud.partialEdge = edges[0];
        hud.sunEdge = edges[1];
        hud.profileId.clear (); // edited by hand: no longer the profile's
    }
    hudshell::ItemTip ("Partial shade from the first edge, full sun from the second, in hours a day.");
    ImGui::Checkbox ("Deep shade below##planting", &hud.deepShade);
    hudshell::ItemTip ("A display subdivision of shade, not a municipal class.");
    if (hud.deepShade) {
        ImGui::SameLine ();
        ImGui::SetNextItemWidth (ImGui::GetFontSize () * 5.0f);
        ImGui::DragFloat ("##deep", &hud.deepShadeEdge, 0.05f, 0.0f, 24.0f, "%.2f h");
    }
    pl::ClampEdges (hud);
    if (ImGui::SmallButton ("Standard edges")) {
        hud.partialEdge = pl::kDefaultPartial;
        hud.sunEdge = pl::kDefaultSun;
        hud.deepShadeEdge = pl::kDefaultDeepShade;
    }
}

void AdvancedSection (pl::PlantingHud& hud)
{
    if (!ImGui::CollapsingHeader ("Advanced##planting"))
        return;
    ImGui::SetNextItemWidth (-LabelColumn ());
    hud.timestep = std::clamp (hud.timestep, 0, pl::kTimestepCount - 1);
    char minutes[16];
    std::snprintf (minutes, sizeof (minutes), "%d min", pl::kTimestepMinutes[hud.timestep]);
    ImGui::SliderInt ("timestep##planting", &hud.timestep, 0, pl::kTimestepCount - 1, minutes,
                      ImGuiSliderFlags_NoInput);
    ImGui::SetNextItemWidth (-LabelColumn ());
    ImGui::SliderFloat ("cell size##planting", &hud.grid, 0.1f, 5.0f, "%.2f m", ImGuiSliderFlags_Logarithmic);
    hud.grid = std::clamp (hud.grid, 0.1f, 5.0f);
    ImGui::SetNextItemWidth (-LabelColumn ());
    float slope = std::acos (std::clamp (hud.minHostUp, 0.01f, 1.0f)) * 180.0f / 3.14159265f;
    if (ImGui::SliderFloat ("max slope##planting", &slope, 5.0f, 85.0f, "%.0f deg"))
        hud.minHostUp = std::cos (slope * 3.14159265f / 180.0f);
    hudshell::ItemTip ("Steeper host surfaces are unavailable for planting (hatched), not measured.");
    ImGui::RadioButton ("CPU##plantingbackend", &hud.backend, 0);
    ImGui::SameLine ();
    ImGui::RadioButton ("GPU##plantingbackend", &hud.backend, 1);
}

void RunRow (HudState& state)
{
    using Stage = tapioca::ui::AnalysisRunStage;
    pl::PlantingHud& hud = state.planting;
    const auto status = sunstudyhud::CurrentJob ();
    const bool ours = hud.runningRun != 0 && status.runs == hud.runningRun;
    const std::vector<float> heights = pl::ChosenHeights (hud);
    const pl::SeasonDates dates = pl::DatesOf (hud.season, hud.custom);
    tapioca::ui::AnalysisRunView view;
    char label[48];
    std::snprintf (label, sizeof (label), heights.size () == 1 ? "Run 1 layer" : "Run %zu layers", heights.size ());
    view.runLabel = label;
    view.enabled =
        !hud.hosts.empty () && !heights.empty () && pl::SeasonDays (dates, hud.year) >= 1 && (!status.running || ours);
    char progress[96] = {};
    if (ours && status.running) {
        view.stage = status.cancelling ? Stage::Cancelling : Stage::Calculating;
        const double share = status.period           ? sunstudyhud::PeriodProgress (status)
                             : status.totalSteps > 0 ? double (status.resolvedSteps) / double (status.totalSteps)
                                                     : 0.0;
        view.resolved = size_t (share * 1000.0 + 0.5);
        view.total = 1000;
        const int index = hud.queueTotal - int (hud.pending.size ());
        std::snprintf (progress, sizeof (progress), "Layer %d / %d, %.2f m  (%d%%)", index, hud.queueTotal,
                       hud.runningHeight, int (share * 100.0));
        view.progressText = progress;
    }
    switch (tapioca::ui::AnalysisRunControl ("##plantingrun", view)) {
        case tapioca::ui::AnalysisRunAction::Run:
            plantinghud::RunHeights (state, pl::ChosenHeights (hud));
            break;
        case tapioca::ui::AnalysisRunAction::Cancel:
            hud.pending.clear ();
            sunstudyhud::CancelActive ();
            break;
        case tapioca::ui::AnalysisRunAction::None:
            break;
    }
    if (hud.hosts.empty ())
        ImGui::TextDisabled ("Pick the planting areas first.");
    else if (status.running && !ours)
        ImGui::TextDisabled ("Another sun study is running.");
    if (!hud.runError.empty ())
        hudshell::ErrorNotice (hud.runError);
}

} // namespace

// ---- entry points -----------------------------------------------------------

void plantinghud::RunHeights (HudState& state, std::vector<float> heights)
{
    pl::PlantingHud& hud = state.planting;
    hud.pending = std::move (heights);
    hud.queueTotal = int (hud.pending.size ());
    SubmitNextHeight (state);
}

void plantinghud::ShowLayer (HudState& state, size_t index)
{
    pl::PlantingHud& hud = state.planting;
    if (index >= hud.layers.size () || hud.runningRun != 0 || hud.showingRun != 0)
        return;
    const pl::Layer& layer = hud.layers[index];
    if (!sunstudyhud::IsPlantingLayerKept (layer.studyId)) {
        RunHeights (state, { layer.height });
        return;
    }
    sunstudyhud::Request request;
    request.showStudy = layer.studyId;
    request.receptorHeight = layer.height;
    uint64_t accepted = 0;
    hud.runError.clear ();
    if (sunstudyhud::Submit (request, hud.runError, &accepted)) {
        hud.showingRun = accepted;
        hud.showingHeight = layer.height;
    }
}

void plantinghud::ClearLayers (HudState& state)
{
    pl::PlantingHud& hud = state.planting;
    if (hud.runningRun != 0 || hud.showingRun != 0)
        return;
    sunstudyhud::ForgetPlantingLayers ();
    hud.layers.clear ();
    hud.shownLayer = -1;
    hud.areas = {};
}

void plantinghud::RefreshAreas (HudState& state, const std::string& studyId, uint64_t version)
{
    pl::PlantingHud& hud = state.planting;
    if (!pl::AreasStale (hud, studyId, version))
        return;
    auto& areas = hud.areas;
    areas.studyId = studyId;
    areas.version = version;
    areas.edges = pl::AreaEdges (hud);
    sunstudyhud::PlantingAreas read;
    std::string error;
    if (sunstudyhud::ReadPlantingAreas (studyId, areas.edges, read, error)) {
        areas.period = read.period;
        pl::KeepLayerAreas (hud, studyId, areas.edges, read.table.total.classAreas, read.table.total.evaluatedArea);
        areas.table = std::move (read.table);
        areas.hostGuids = std::move (read.hostGuids);
        areas.error.clear ();
        return;
    }
    areas.table = {};
    areas.hostGuids.clear ();
    areas.error = error.empty () ? "The area table could not be read." : error;
}

void plantinghud::RefreshLayerAreas (HudState& state)
{
    pl::PlantingHud& hud = state.planting;
    if (hud.runningRun != 0)
        return;
    const std::vector<double> edges = pl::AreaEdges (hud);
    for (pl::Layer& layer : hud.layers) {
        if (layer.areaEdges == edges)
            continue; // read (or tried) at these edges
        sunstudyhud::PlantingAreas read;
        std::string error;
        if (sunstudyhud::ReadPlantingAreas (layer.studyId, edges, read, error)) {
            layer.classAreas = read.table.total.classAreas;
            layer.evaluatedArea = read.table.total.evaluatedArea;
        }
        else {
            layer.classAreas.clear (); // no longer kept: unknown until traced again
            layer.evaluatedArea = 0.0;
        }
        layer.areaEdges = edges;
    }
}

void OpenPlantingBuilder (HudState& state)
{
    pl::PlantingHud& hud = state.planting;
    const std::vector<size_t> chosen = pl::PageLayers (hud, kPlantingMaxTiles);
    if (chosen.empty ()) {
        hud.runError = "Show a layer once before opening Layout Builder.";
        return;
    }
    LayoutReportPage page;
    page.type = ReportTemplate::Planting;
    const std::vector<float> edges = pl::ClassEdges (hud);
    for (int cls = 0; cls < pl::ClassCount (hud); ++cls)
        page.classNames.push_back (pl::ClassName (hud, cls));
    std::vector<std::shared_ptr<const SunStudyAtlasUpload>> displays;
    std::string period;
    for (const size_t index : chosen) {
        const pl::Layer& layer = hud.layers[index];
        ReportPanel panel;
        panel.date = pl::LayerName (hud, layer.height);
        ReportAreaRow row;
        row.name = panel.date;
        sunstudyhud::PlantingAreas areas;
        std::string error;
        if (sunstudyhud::ReadPlantingAreas (layer.studyId, pl::AreaEdges (hud), areas, error)) {
            row.classAreas = areas.table.total.classAreas;
            row.unavailable = areas.table.total.UnavailableArea ();
            row.measured = areas.table.total.evaluatedArea;
            panel.metric = pl::ClassShares (hud, row.classAreas);
        }
        else
            panel.metric = "Areas unavailable: " + error;
        if (period.empty ())
            period = layer.period ? layer.label + ", mean direct sun a day" : "one day's direct sun";
        page.panels.push_back (std::move (panel));
        page.areaRows.push_back (std::move (row));
        displays.push_back (layer.display);
    }
    std::string classes = "classes at ";
    for (size_t i = 0; i < edges.size (); ++i) {
        char edge[16];
        std::snprintf (edge, sizeof (edge), "%.4g", edges[i]);
        classes += (i == 0 ? "" : i + 1 == edges.size () ? " and " : ", ") + std::string (edge);
    }
    classes += " h a day (Vilnius 2024)";
    page.subtitle = "Light by layer height  |  " + period + "  |  " + classes;
    page.notes = { "Classes from each sample's own hours, exact at an edge: a value at an edge belongs to the upper "
                   "class. Plan areas in square metres.",
                   "Unavailable (hatched, N/A): the host is too steep, or a ceiling is within 0.10 m above the layer. "
                   "It is never counted as shade." };
    if (!viewerhud::BeginCaptureFrame (state, kShotSunStudy))
        return;
    auto& f = state.captureFrame;
    f.builder = true;
    f.studyDay = f.seasonal = false;
    f.plantingTemplate = std::move (page);
    f.plantingPayloads = std::move (displays);
    f.SelectWorkflow (int (ReportTemplate::Planting));
    f.paperDirty = true;
}

void ReleasePlantingDisplay (HudState& state)
{
    auto& hold = state.planting.sunHold;
    if (!hold.held)
        return;
    state.sunGradientPreset = hold.preset;
    state.sunGradientFlipped = hold.flipped;
    state.sunGradientCustom = hold.custom;
    state.sunGradientStepped = hold.stepped;
    state.sunBandCount = hold.bands;
    std::copy_n (hold.edges.begin (), (std::min) (hold.edges.size (), std::size (state.sunBandEdges)),
                 state.sunBandEdges);
    state.sunLowBlue = hold.lowBlue;
    state.sunView = hold.view;
    hold = {};
}

void ServicePlantingRuns (HudState& state)
{
    pl::PlantingHud& hud = state.planting;
    if (hud.showingRun != 0) {
        const auto status = sunstudyhud::CurrentJob ();
        if (status.runs != hud.showingRun || !status.running) {
            const float height = hud.showingHeight;
            const bool lost = status.runs == hud.showingRun && status.stage == "failed";
            hud.showingRun = 0;
            hud.showingHeight = -1.0f;
            // Evicted between the click and the show: trace it again.
            if (lost && hud.runningRun == 0) {
                hud.runError.clear ();
                plantinghud::RunHeights (state, { height });
            }
        }
    }
    if (hud.runningRun == 0)
        return;
    const auto status = sunstudyhud::CurrentJob ();
    // Marked while it runs, before its claim arrives: the claim then selects
    // the Planting tab, not Sun study (AnalysisTabs.hpp).
    if (status.runs == hud.runningRun && status.receptorHeight >= 0.0)
        analysistabs::MarkPlanting (state.analysisFocus, status.studyId);
    if (status.runs != hud.runningRun) {
        // Another run took the runner: this queue is over.
        hud.pending.clear ();
        hud.runningRun = 0;
        hud.runningHeight = -1.0f;
        return;
    }
    if (status.running)
        return;
    if (status.stage == "done" && !status.studyId.empty ()) {
        pl::KeepLayer (hud, hud.runningHeight, status.studyId, status.period,
                       status.period ? status.periodLabel + (status.estimate ? " (estimate)" : "") : std::string ());
        analysistabs::MarkPlanting (state.analysisFocus, status.studyId);
        if (status.period) {
            state.sunPeriodStudyId = status.studyId;
            state.sunPeriodDays = double (status.representedDays);
            state.sunPeriodLabel = status.periodLabel;
            state.sunPeriodCarrierDate = status.carrierDate;
            state.sunPeriodEstimate = status.estimate;
        }
    }
    else {
        hud.pending.clear ();
        if (hud.runError.empty ())
            hud.runError = status.error.empty () ? "The planting study stopped: " + status.stage : status.error;
    }
    hud.runningRun = 0;
    hud.runningHeight = -1.0f;
    SubmitNextHeight (state);
}

void DrawPlantingHudSection (HudState& state, const DiligentSceneStats& scene)
{
    ServicePlantingRuns (state);
    ApplyPlantingDisplay (state);
    if (!hudshell::BeginSubTabs ("##planting-tabs"))
        return;
    if (ImGui::BeginTabItem ("Light")) {
        pl::PlantingHud& hud = state.planting;
        hud.highlightMask = 0; // the Plants tab's highlight is drawn only there
        ImGui::BeginDisabled (hud.runningRun != 0);
        AreasSection (state);
        PeriodSection (hud);
        LayersSection (hud);
        ClassesSection (state);
        plantinghud::DrawClimateSection (state, scene);
        AdvancedSection (hud);
        ImGui::EndDisabled ();
        RunRow (state);
        plantinghud::DrawResults (state, scene);
        ImGui::EndTabItem ();
    }
    const bool openPlants = std::exchange (state.planting.catalogue.openPlantsTab, false);
    if (ImGui::BeginTabItem ("Plants", nullptr, openPlants ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        plantinghud::DrawPlantsTab (state);
        ImGui::EndTabItem ();
    }
    if (ImGui::BeginTabItem ("Compare")) {
        state.planting.highlightMask = 0;
        plantinghud::DrawCompareTab (state);
        ImGui::EndTabItem ();
    }
    hudshell::EndSubTabs ();
}

} // namespace geomsrv::archviz
