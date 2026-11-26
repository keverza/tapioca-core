// The Planting tab's Climate section (#155 P1, #278): a local EPW file's
// station, its distance from the project, coverage and missing data, monthly
// temperature and global horizontal radiation, and a growing season read from
// its temperatures that the form can use as a custom season.
//
// ⚠️ NOT A LIGHT INPUT YET. The classes stay geometry-only (direct-sun hours)
// until #155 P2 aligns the weather's instants with the host's sun positions;
// the weather-weighted option stays disabled and says so.
//
// RENDER THREAD. A weather file (about 1.5 MB) is parsed on the click that
// picks it; the folder is listed on demand, never every frame.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudPlantingParts.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/PlantingFiles.hpp"

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
    climate.data = std::move (data);
    ComputeSeason (climate);
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
        std::string tip;
        for (int m = 0; m < 12; ++m) {
            char line[48];
            std::snprintf (line, sizeof (line), "%s  %.1f C  (%.1f to %.1f)\n", kMonths[m],
                           w.months[size_t (m)].meanDryBulbC, w.months[size_t (m)].minDryBulbC,
                           w.months[size_t (m)].maxDryBulbC);
            tip += line;
        }
        ImGui::SetTooltip ("%s", tip.c_str ());
    }
    double year = 0;
    for (float g : ghi)
        year += g;
    std::snprintf (label, sizeof (label), "global horizontal, %.0f kWh/m2 a year", year);
    ImGui::PlotHistogram ("##epwghi", ghi, 12, 0, label, 0.0f, ghiMax * 1.15f, ImVec2 (width, height));
    if (ImGui::IsItemHovered ()) {
        std::string tip;
        for (int m = 0; m < 12; ++m) {
            char line[64];
            std::snprintf (line, sizeof (line), "%s  %.1f kWh/m2%s\n", kMonths[m], ghi[m],
                           w.months[size_t (m)].globalMissing ? "  (some hours missing)" : "");
            tip += line;
        }
        ImGui::SetTooltip ("Interval energy: each value x its interval hours.\n%s", tip.c_str ());
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
    ImGui::BeginDisabled ();
    int later = hud.climate;
    ImGui::RadioButton ("Weather-weighted##plantingclimate", &later, 1);
    ImGui::EndDisabled ();
    hudshell::ItemTip ("Later: weight the hours by the weather and add daily light integral, once the weather's "
                       "instants are aligned with the sun positions (#155 P2).");
    const char* current = climate.selected >= 0 && size_t (climate.selected) < climate.files.size ()
                              ? climate.files[size_t (climate.selected)].c_str ()
                              : (climate.files.empty () ? "no .epw in the folder" : "pick a weather file");
    ImGui::SetNextItemWidth (-ImGui::GetFontSize () * 9.5f);
    if (ImGui::BeginCombo ("##epwfile", current)) {
        for (size_t i = 0; i < climate.files.size (); ++i)
            if (ImGui::Selectable (climate.files[i].c_str (), climate.selected == int (i)))
                Load (climate, int (i));
        ImGui::EndCombo ();
    }
    hudshell::ItemTip ("EnergyPlus weather files (.epw) in %s. Nothing is downloaded.", climate.folder.c_str ());
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Open folder##epw"))
        OpenFolderInExplorer (pl::TapiocaDataFolder (L"Weather"));
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Rescan##epw"))
        Rescan (climate);
    if (!climate.error.empty ())
        ImGui::TextColored (ImVec4 (0.95f, 0.45f, 0.40f, 1.0f), "%s", climate.error.c_str ());
    if (!climate.data)
        return;
    StationLines (climate, scene);
    QualityLines (*climate.data);
    MonthlyCharts (*climate.data);
    SeasonRow (hud);
}

} // namespace geomsrv::archviz::plantinghud
