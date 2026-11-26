// The Planting tab's Climate section (#155 P1, #278): a local EPW file's
// station, its distance from the project, coverage and missing data, monthly
// temperature and global horizontal radiation, and a growing season read from
// its temperatures that the form can use as a custom season.
//
// Weather-weighted light (#278 Stage 5): for the shown layer, Archicad's sun
// for the weather year (checked, #155 P2) and a climate run on the layer's
// samples give monthly daily light integral, energy and weather coverage. The
// light classes stay geometry-only (direct-sun hours); the weather figures sit
// beside them, labelled an estimate, never mixed in.
//
// RENDER THREAD. A weather file (about 1.5 MB) is parsed on the click that
// picks it; the folder is listed on demand, never every frame.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudPlantingParts.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/PlantHardiness.hpp"
#include "ArchViz/PlantingFiles.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace geomsrv::archviz::plantinghud {

namespace pl = planting;
namespace ss = evp::sunstudy;

namespace {

constexpr const char* kMonths[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

// Short names of the fields a missing count is worth showing for.
struct FieldLabel {
    ss::WeatherField field;
    const char* name;
};
constexpr FieldLabel kWatched[] = {
    { ss::WeatherField::GlobalHorizontal, "global horizontal" },
    { ss::WeatherField::DirectNormal, "direct normal" },
    { ss::WeatherField::DiffuseHorizontal, "diffuse horizontal" },
    { ss::WeatherField::DryBulb, "dry bulb" },
    { ss::WeatherField::RelativeHumidity, "humidity" },
    { ss::WeatherField::WindSpeed, "wind speed" },
    { ss::WeatherField::GlobalIlluminance, "global illuminance" },
};

void Rescan (pl::ClimateState& climate)
{
    climate.scanned = true;
    const auto folder = pl::TapiocaDataFolder (L"Weather");
    climate.folder = pl::PathText (folder);
    std::string error;
    const std::string current = climate.selected >= 0 && size_t (climate.selected) < climate.files.size ()
                                    ? climate.files[size_t (climate.selected)]
                                    : "";
    climate.files = pl::ListWeatherFiles (folder, error);
    if (!error.empty ())
        climate.error = error;
    const auto it = std::find (climate.files.begin (), climate.files.end (), current);
    climate.selected = it == climate.files.end () ? -1 : int (it - climate.files.begin ());
    if (climate.selected < 0)
        climate.data.reset ();
}

void ComputeSeason (pl::ClimateState& climate)
{
    climate.season = {};
    if (!climate.data)
        return;
    const bool wraps = climate.data->days.size () >= 365;
    std::string error;
    if (!ss::FindGrowingSeason (climate.data->days, climate.windowDays, climate.thresholdC, wraps, climate.season,
                                error))
        climate.error = error;
}

void Load (pl::ClimateState& climate, int index)
{
    climate.selected = index;
    climate.data.reset ();
    climate.error.clear ();
    if (index < 0 || size_t (index) >= climate.files.size ())
        return;
    auto data = std::make_shared<ss::WeatherData> ();
    std::string error;
    if (!ss::ReadWeatherFile (pl::TapiocaDataFolder (L"Weather") /
                                  std::filesystem::u8path (climate.files[size_t (index)]),
                              *data, error)) {
        climate.error = error;
        return;
    }
    // The coldest hour in the file, and the half zone it falls in: a suggestion.
    climate.fileZone = -1;
    climate.fileMinimumC = 0.0;
    if (pl::ColdestHourC (*data, climate.fileMinimumC))
        climate.fileZone = pl::UsdaHalfZoneOf (climate.fileMinimumC);
    const std::string name = climate.files[size_t (index)];
    auto lower = [] (std::string s) {
        std::transform (s.begin (), s.end (), s.begin (), [] (unsigned char c) { return char (std::tolower (c)); });
        return s;
    };
    climate.fileNormals = lower (name).find ("normals") != std::string::npos ||
                          lower (data->identity.source).find ("normals") != std::string::npos;
    climate.data = std::move (data);
    ComputeSeason (climate);
}

// Winter hardiness: the file's coldest hour as a suggestion, and the design
// zone the Plants filter and the Scheme check against.
void HardinessRow (pl::ClimateState& climate)
{
    ImGui::SeparatorText ("Winter hardiness");
    if (climate.fileZone >= 0) {
        ImGui::Text ("Coldest hour in the file: %.1f C, USDA %s by that value.", climate.fileMinimumC,
                     pl::UsdaName (climate.fileZone).c_str ());
        hudshell::ItemTip ("A USDA zone is the mean of many winters' lowest temperatures. A typical-year file is "
                           "built from typical months, so its coldest hour usually runs warmer than the zone, often "
                           "by one zone. Check a local hardiness map before choosing.");
        if (climate.fileNormals)
            ImGui::TextColored (ImVec4 (0.95f, 0.72f, 0.30f, 1.0f),
                                "A normals file: averaged, so its cold extremes are smoothed away. Do not take the "
                                "zone from it.");
        else
            ImGui::TextDisabled ("A typical year reads warmer than the zone; take it as a warm limit.");
    }
    const std::string label =
        climate.designZone >= 0 ? "USDA " + pl::UsdaName (climate.designZone) : std::string ("not set");
    ImGui::SetNextItemWidth (ImGui::GetFontSize () * 7.0f);
    if (ImGui::BeginCombo ("Design zone##hardiness", label.c_str ())) {
        if (ImGui::Selectable ("not set", climate.designZone < 0))
            climate.designZone = -1;
        for (int h = 0; h < pl::kUsdaHalfZones; ++h) {
            const std::string item = "USDA " + pl::UsdaName (h);
            if (ImGui::Selectable (item.c_str (), climate.designZone == h))
                climate.designZone = h;
        }
        ImGui::EndCombo ();
    }
    hudshell::ItemTip ("The winter the planting must survive. Plants checks it (Hardy here) and the Scheme notes "
                       "any plant whose catalogue rating is colder-limited than this.");
    if (climate.fileZone >= 0 && !climate.fileNormals) {
        ImGui::SameLine ();
        const std::string use = "Use " + pl::UsdaName (climate.fileZone) + "##hardinessuse";
        if (ImGui::SmallButton (use.c_str ()))
            climate.designZone = climate.fileZone;
        hudshell::ItemTip ("Take the file's reading. It is likely warmer than the real zone.");
        ImGui::SameLine ();
        const int colder = std::max (0, climate.fileZone - 2);
        const std::string safer = "Use " + pl::UsdaName (colder) + " (one zone colder)##hardinesssafer";
        if (ImGui::SmallButton (safer.c_str ()))
            climate.designZone = colder;
        hudshell::ItemTip ("A cautious reading: one whole zone colder than the file's coldest hour.");
    }
}

void StationLines (const pl::ClimateState& climate, const DiligentSceneStats& scene)
{
    const ss::WeatherData& w = *climate.data;
    const ss::WeatherIdentity& id = w.identity;
    ImGui::Text ("%s%s%s, %s  |  WMO %s", id.city.c_str (), id.region.empty () || id.region == "-" ? "" : ", ",
                 id.region == "-" ? "" : id.region.c_str (), id.country.c_str (), id.wmo.c_str ());
    ImGui::TextDisabled ("%.4f %c, %.4f %c  |  %.0f m  |  UTC%+.1f standard  |  %s", std::abs (id.latitude),
                         id.latitude < 0 ? 'S' : 'N', std::abs (id.longitude), id.longitude < 0 ? 'W' : 'E',
                         id.elevationM, id.timeZoneHours, id.source.c_str ());
    if (!id.comments[0].empty ())
        hudshell::ItemTip ("%s", id.comments[0].c_str ());
    if (!scene.sunApplied) {
        ImGui::TextDisabled ("Distance: the project's location is not known yet.");
        return;
    }
    double metres = 0;
    std::string error;
    if (!ss::StationDistance (id.latitude, id.longitude, scene.latitudeDegrees, scene.longitudeDegrees, metres,
                              error)) {
        ImGui::TextDisabled ("Distance: %s", error.c_str ());
        return;
    }
    const double dz = id.elevationM - double (scene.siteAltitudeMetres);
    const pl::StationMatch match = pl::MatchStation (metres, dz);
    const ImVec4 colour = match == pl::StationMatch::Near   ? ImVec4 (0.45f, 0.80f, 0.45f, 1.0f)
                          : match == pl::StationMatch::Warn ? ImVec4 (0.95f, 0.75f, 0.30f, 1.0f)
                                                            : ImVec4 (0.95f, 0.45f, 0.40f, 1.0f);
    ImGui::TextColored (colour, "%s",
                        match == pl::StationMatch::Near   ? "Near"
                        : match == pl::StationMatch::Warn ? "Check"
                                                          : "Far");
    ImGui::SameLine ();
    ImGui::Text ("%.1f km from the project, %.0f m %s", metres / 1000.0, std::abs (dz), dz >= 0 ? "higher" : "lower");
    hudshell::ItemTip ("Near: within 30 km and 50 m of height. Beyond 150 km or 300 m the station describes another "
                       "climate. Proposed defaults.");
}

void QualityLines (const ss::WeatherData& w)
{
    const ss::WeatherIdentity& id = w.identity;
    ImGui::TextDisabled ("%d records  |  %u per hour  |  %02d/%02d to %02d/%02d%s  |  sha %.8s",
                         int (w.records.size ()), id.recordsPerHour, id.startMonth, id.startDay, id.endMonth, id.endDay,
                         id.leapYear ? "  |  leap" : "", id.sha256.c_str ());
    hudshell::ItemTip ("Each record covers the interval before its timestamp, in the station's standard time.\n"
                       "SHA-256: %s",
                       id.sha256.c_str ());
    std::string missing;
    for (const auto& f : kWatched) {
        const size_t n = id.missing[size_t (f.field)] + id.outOfRange[size_t (f.field)];
        if (n)
            missing += (missing.empty () ? "" : ", ") + std::string (f.name) + " " + std::to_string (n);
    }
    if (missing.empty ())
        ImGui::TextDisabled ("No missing radiation, temperature, humidity or wind values.");
    else
        ImGui::TextColored (ImVec4 (0.95f, 0.75f, 0.30f, 1.0f), "Missing or invalid: %s", missing.c_str ());
    if (id.radiationInDark)
        ImGui::TextColored (ImVec4 (0.95f, 0.75f, 0.30f, 1.0f), "%d records report radiation with the sun down.",
                            int (id.radiationInDark));
}

// The month under the mouse in the plot just drawn, as ImGui's own plot picks
// the point it highlights: a line snaps to the nearest of its 12 points, a bar
// chart to the bar under the mouse.
int HoveredMonth (bool bars)
{
    const ImVec2 pad = ImGui::GetStyle ().FramePadding;
    const float x0 = ImGui::GetItemRectMin ().x + pad.x, x1 = ImGui::GetItemRectMax ().x - pad.x;
    const float t = std::clamp ((ImGui::GetIO ().MousePos.x - x0) / std::max (x1 - x0, 1.0f), 0.0f, 0.9999f);
    return std::clamp (bars ? int (t * 12.0f) : int (t * 11.0f + 0.5f), 0, 11);
}

void MonthlyCharts (const ss::WeatherData& w)
{
    float temps[12] = {}, ghi[12] = {};
    float lo = 0, hi = 0, ghiMax = 0;
    for (int m = 0; m < 12; ++m) {
        const auto& month = w.months[size_t (m)];
        temps[m] = std::isfinite (month.meanDryBulbC) ? float (month.meanDryBulbC) : 0.0f;
        ghi[m] = float (month.globalHorizontalKwhM2);
        lo = std::min (lo, temps[m]);
        hi = std::max (hi, temps[m]);
        ghiMax = std::max (ghiMax, ghi[m]);
    }
    const float width = ImGui::GetContentRegionAvail ().x;
    const float height = ImGui::GetFontSize () * 3.2f;
    char label[64];
    std::snprintf (label, sizeof (label), "mean dry bulb, %.0f to %.0f C", lo, hi);
    ImGui::PlotLines ("##epwtemp", temps, 12, 0, label, lo - 2.0f, hi + 2.0f, ImVec2 (width, height));
    if (ImGui::IsItemHovered ()) {
        const int m = HoveredMonth (false);
        const auto& month = w.months[size_t (m)];
        ImGui::SetTooltip ("%s: mean %.1f C, coldest %.1f C, warmest %.1f C", kMonths[m], month.meanDryBulbC,
                           month.minDryBulbC, month.maxDryBulbC);
    }
    double year = 0;
    for (float g : ghi)
        year += g;
    std::snprintf (label, sizeof (label), "global horizontal, %.0f kWh/m2 a year", year);
    ImGui::PlotHistogram ("##epwghi", ghi, 12, 0, label, 0.0f, ghiMax * 1.15f, ImVec2 (width, height));
    if (ImGui::IsItemHovered ()) {
        const int m = HoveredMonth (true);
        ImGui::SetTooltip ("%s: %.1f kWh/m2 of sun on open ground%s", kMonths[m], ghi[m],
                           w.months[size_t (m)].globalMissing ? " (some hours missing)" : "");
    }
}

void SeasonRow (pl::PlantingHud& hud)
{
    pl::ClimateState& climate = hud.climateState;
    bool changed = false;
    ImGui::SetNextItemWidth (ImGui::GetFontSize () * 4.5f);
    changed |= ImGui::DragFloat ("C##epwthreshold", &climate.thresholdC, 0.1f, -10.0f, 20.0f, "%.1f");
    hudshell::ItemTip ("In season while the running mean of the daily mean temperature stays at or above this.");
    ImGui::SameLine ();
    ImGui::SetNextItemWidth (ImGui::GetFontSize () * 4.5f);
    int window = climate.windowDays;
    if (ImGui::InputInt ("days##epwwindow", &window, 2, 2)) {
        climate.windowDays = std::clamp (window | 1, 1, 31);
        changed = true;
    }
    hudshell::ItemTip ("The centred running mean's window, an odd number of days.");
    if (changed)
        ComputeSeason (climate);
    const ss::WeatherData& w = *climate.data;
    pl::SeasonDates dates;
    if (!climate.season.found) {
        ImGui::TextDisabled ("No day reaches the threshold.");
        return;
    }
    const auto& a = w.days[climate.season.first];
    const auto& b = w.days[climate.season.last];
    const size_t n = climate.season.last >= climate.season.first
                         ? climate.season.last - climate.season.first + 1
                         : w.days.size () - climate.season.first + climate.season.last + 1;
    ImGui::Text ("Season: %d %s to %d %s, %d days", a.day, kMonths[(a.month + 11) % 12], b.day,
                 kMonths[(b.month + 11) % 12], int (n));
    const bool usable = pl::SeasonDatesFromWeather (w.days, climate.season, dates);
    ImGui::SameLine ();
    ImGui::BeginDisabled (!usable);
    if (ImGui::SmallButton ("Use##epwseason")) {
        hud.season = pl::kCustom;
        hud.custom = dates;
    }
    ImGui::EndDisabled ();
    hudshell::ItemTip (usable ? "Set the study period to these dates (Custom)."
                              : "It runs over New Year; a custom season stays within one year.");
}

// The shown layer's weather-weighted light: run it, follow it, show it.
void WeatherLight (pl::PlantingHud& hud, const pl::ClimateState& climate)
{
    if (hud.shownLayer < 0 || size_t (hud.shownLayer) >= hud.layers.size ()) {
        ImGui::TextDisabled ("Trace a layer first: the weather is weighed on a layer's samples.");
        return;
    }
    const pl::Layer& layer = hud.layers[size_t (hud.shownLayer)];
    if (!climate.data) {
        ImGui::TextDisabled ("Pick a weather file.");
        return;
    }
    const sunstudyhud::PlantingClimate run = sunstudyhud::CurrentPlantingClimate ();
    const bool mine =
        run.studyId == layer.studyId && run.weatherSha256 == climate.data->identity.sha256 && run.year == hud.year;
    using State = sunstudyhud::PlantingClimate::State;
    const bool busy = mine && (run.state == State::Capturing || run.state == State::Tracing);
    ImGui::BeginDisabled (busy || (mine && run.state == State::Ready));
    char label[96];
    std::snprintf (label, sizeof label, "Weigh the %.2f m layer by the weather##climaterun", layer.height);
    if (ImGui::Button (label))
        sunstudyhud::RequestPlantingClimate (layer.studyId, layer.height, climate.data, hud.year);
    ImGui::EndDisabled ();
    hudshell::ItemTip ("Every hour of the weather file on this layer: Archicad's sun for each hour, checked "
                       "against an independent calculation, then direct, sky and ground-reflected light per "
                       "sample. Takes a while on a large layer; the viewer stays usable.");
    if (!mine)
        return;
    switch (run.state) {
        case State::Capturing:
            ImGui::TextDisabled ("Reading Archicad's sun for every hour of the year...");
            return;
        case State::Tracing: {
            const float f = run.total ? float (run.done) / float (run.total) : 0.0f;
            char text[64];
            std::snprintf (text, sizeof text, "%zu of %zu hours", run.done, run.total);
            ImGui::ProgressBar (f, ImVec2 (-1, 0), text);
            return;
        }
        case State::Failed:
            ImGui::TextColored (ImVec4 (0.95f, 0.45f, 0.40f, 1.0f), "%s", run.error.c_str ());
            return;
        case State::Idle:
        case State::Ready:
            break;
    }
    if (run.state != State::Ready)
        return;
    ImGui::TextDisabled ("%s, %d: %zu samples, %.0f s. Archicad's sun agrees with an independent calculation "
                         "to %.2f deg on average.",
                         run.station.c_str (), run.year, run.samples, run.seconds, run.sunMeanDegrees);
    const ImGuiTableFlags flags =
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable ("##climatelight", 4, flags)) {
        ImGui::TableSetupColumn ("Month");
        ImGui::TableSetupColumn ("DLI mol/m2/day");
        ImGui::TableSetupColumn ("Light kWh/m2");
        ImGui::TableSetupColumn ("Weather hours");
        ImGui::TableHeadersRow ();
        for (int m = 0; m < 12; ++m) {
            const double dli = run.dli[size_t (m)];
            if (std::isnan (dli))
                continue;
            ImGui::TableNextRow ();
            ImGui::TableNextColumn ();
            ImGui::TextUnformatted (kMonths[m]);
            ImGui::TableNextColumn ();
            ImGui::Text ("%.1f", dli);
            ImGui::TableNextColumn ();
            ImGui::Text ("%.0f", run.energyKwh[size_t (m)]);
            ImGui::TableNextColumn ();
            ImGui::Text ("%.0f %%", 100.0 * run.coverage[size_t (m)]);
        }
        ImGui::EndTable ();
    }
    hudshell::ItemTip ("Area-weighted over the layer's samples. DLI: the month's light as photosynthetic photons "
                       "a day (0.45 of the energy is photosynthetic light, 4.57 umol per joule): an estimate. "
                       "Weather hours: the share of the month's hours with radiation in the file.");
    ImGui::TextDisabled ("Sky view %.2f (geometry). Estimate: ground taken as unshaded, everything that casts a "
                         "shadow treated as solid.",
                         run.skyView);
}

} // namespace

void DrawClimateSection (HudState& state, const DiligentSceneStats& scene)
{
    pl::PlantingHud& hud = state.planting;
    if (!ImGui::CollapsingHeader ("Climate"))
        return;
    pl::ClimateState& climate = hud.climateState;
    if (!climate.scanned)
        Rescan (climate);
    ImGui::RadioButton ("Geometry only##plantingclimate", &hud.climate, 0);
    hudshell::ItemTip ("Direct-sun hours from the sun's path and the model alone: no weather, no clouds.");
    ImGui::SameLine ();
    ImGui::RadioButton ("Weather-weighted##plantingclimate", &hud.climate, 1);
    hudshell::ItemTip ("The shown layer under the picked weather file: daily light integral and light energy "
                       "per month, beside the sun hours. The light classes stay sun hours.");
    const char* current = climate.selected >= 0 && size_t (climate.selected) < climate.files.size ()
                              ? climate.files[size_t (climate.selected)].c_str ()
                              : (climate.files.empty () ? "no .epw or .zip in the folder" : "pick a weather file");
    ImGui::SetNextItemWidth (-ImGui::GetFontSize () * 9.5f);
    if (ImGui::BeginCombo ("##epwfile", current)) {
        for (size_t i = 0; i < climate.files.size (); ++i)
            if (ImGui::Selectable (climate.files[i].c_str (), climate.selected == int (i)))
                Load (climate, int (i));
        ImGui::EndCombo ();
    }
    hudshell::ItemTip ("EnergyPlus weather files in %s: an .epw, or the .zip as downloaded (its .epw is read "
                       "without unpacking). Nothing is downloaded.",
                       climate.folder.c_str ());
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Open folder##epw"))
        OpenFolderInExplorer (pl::TapiocaDataFolder (L"Weather"));
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Rescan##epw"))
        Rescan (climate);
    if (!climate.error.empty ())
        ImGui::TextColored (ImVec4 (0.95f, 0.45f, 0.40f, 1.0f), "%s", climate.error.c_str ());
    if (!climate.data) {
        if (climate.designZone >= 0 || !climate.files.empty ())
            HardinessRow (climate); // a design zone needs no weather file
        return;
    }
    StationLines (climate, scene);
    if (hud.climate == 1)
        WeatherLight (hud, climate);
    QualityLines (*climate.data);
    MonthlyCharts (*climate.data);
    SeasonRow (hud);
    HardinessRow (climate);
}

} // namespace geomsrv::archviz::plantinghud
