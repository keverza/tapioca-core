// The Planting tab's zones (#278): "Zones from polylines..." reads the closed
// Polylines and Fills selected in Archicad (PlantingZoneSource, main thread),
// lists them, and gives the area table a row per zone for the shown layer.
//
// A zone row is read from the layer's samples (ReadPlantingSite, plain values)
// with the area table's own edges and exact comparator, so a zone's classes
// match the host rows. Unavailable area is not stored per sample, so a zone
// row has no N/A figure; it says so.
//
// RENDER THREAD. Reads HudState and plain values from the runner.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudPlantingParts.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/PlantingZoneSource.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace geomsrv::archviz::plantinghud {

namespace pl = planting;

namespace {

// Takes a finished import, once.
void TakeImport (pl::ZonesState& z)
{
    const auto latest = plantingzonesource::Latest ();
    if (latest.serial == z.seenSerial)
        return;
    z.seenSerial = latest.serial;
    if (!z.importing)
        return;
    z.importing = false;
    z.skipped = latest.skipped;
    if (latest.zones.empty ()) {
        z.status = latest.error.empty () ? "No zone was read." : latest.error;
        return;
    }
    // Replace zones read before from the same element; add the rest.
    for (const auto& zone : latest.zones) {
        auto same = std::find_if (z.zones.begin (), z.zones.end (),
                                  [&] (const pl::PlantingZone& old) { return old.guid == zone.guid; });
        if (same != z.zones.end ()) {
            const pl::ZoneRole role = same->role; // the user's marking outlives a new outline
            const float allowed = same->allowedHeight;
            *same = zone;
            same->role = role;
            same->allowedHeight = allowed;
        }
        else if (z.zones.size () < pl::kMaxPlantingZones)
            z.zones.push_back (zone);
    }
    ++z.revision;
    char text[96];
    std::snprintf (text, sizeof (text), "Read %zu zone(s).", latest.zones.size ());
    z.status = text;
}

} // namespace

void DrawZonesRow (HudState& state)
{
    pl::PlantingHud& hud = state.planting;
    pl::ZonesState& z = hud.zones;
    TakeImport (z);
    ImGui::BeginDisabled (z.importing);
    if (ImGui::Button ("Zones from polylines...")) {
        z.importing = true;
        z.status = "Reading the selection...";
        plantingzonesource::RequestImport ();
    }
    ImGui::EndDisabled ();
    hudshell::ItemTip ("Read the closed Polylines and Fills selected in Archicad as planting zones. Each is projected "
                       "straight down onto the hosts and gets its own area table row; the Scheme can be limited to "
                       "one. Select them again and read to update.");
    ImGui::SameLine ();
    LaterButton ("Pick in viewer", "Later: click host surfaces in the viewer instead of selecting elements.");
    if (!z.status.empty ())
        ImGui::TextDisabled ("%s", z.status.c_str ());
    for (const auto& s : z.skipped)
        ImGui::TextDisabled ("Skipped %s", s.c_str ());
    if (z.zones.empty ())
        return;
    int remove = -1;
    for (size_t i = 0; i < z.zones.size (); ++i) {
        ImGui::PushID (int (i));
        auto& zone = z.zones[i];
        ImGui::Text ("%s  %.1f m2%s", zone.name.c_str (), zone.PlanArea (),
                     zone.rings.size () > 1 ? "  (with holes)" : "");
        hudshell::ItemTip ("%s %s\nPlan area of the outline, holes taken out; the measured area is in the area "
                           "table.",
                           zone.fill ? "Fill" : "Polyline", zone.guid.c_str ());
        ImGui::SameLine ();
        bool noGo = zone.role == pl::ZoneRole::NoGo;
        if (ImGui::Checkbox ("No-go", &noGo)) {
            zone.role = noGo ? pl::ZoneRole::NoGo : pl::ZoneRole::Bed;
            ++z.revision;
        }
        hudshell::ItemTip ("Keep plant crowns out of this zone: a path, an access or fire route, the ground in front "
                           "of a window. Your rule, never inferred. The Scheme and Fit keep every crown (radius plus "
                           "margin) clear of it in plan.");
        if (noGo) {
            ImGui::SameLine ();
            ImGui::SetNextItemWidth (ImGui::GetFontSize () * 5.5f);
            if (ImGui::DragFloat ("##allowed", &zone.allowedHeight, 0.05f, 0.0f, 10.0f,
                                  zone.allowedHeight > 0.0f ? "up to %.2f m" : "nothing"))
                ++z.revision;
            zone.allowedHeight = std::clamp (zone.allowedHeight, 0.0f, 10.0f);
            hudshell::ItemTip ("Plants up to this mature height may still grow here (a low bed under a window). "
                               "\"nothing\" keeps every plant out.");
        }
        ImGui::SameLine ();
        if (ImGui::SmallButton ("x"))
            remove = int (i);
        ImGui::PopID ();
    }
    if (remove >= 0) {
        z.zones.erase (z.zones.begin () + remove);
        ++z.revision;
        if (hud.scheme.zone == remove)
            hud.scheme.zone = -1;
        else if (hud.scheme.zone > remove)
            --hud.scheme.zone;
    }
    if (ImGui::SmallButton ("Clear zones")) {
        z.zones.clear ();
        z.status.clear ();
        z.skipped.clear ();
        ++z.revision;
        hud.scheme.zone = -1;
    }
}

const pl::ZonesState& RefreshZoneRows (HudState& state, const std::string& studyId, float layerHeight)
{
    pl::PlantingHud& hud = state.planting;
    pl::ZonesState& z = hud.zones;
    const std::vector<double> edges = pl::AreaEdges (hud);
    if (z.zones.empty () || (z.study == studyId && z.edges == edges && z.rowsRevision == z.revision))
        return z;
    z.study = studyId;
    z.edges = edges;
    z.rowsRevision = z.revision;
    z.rows.clear ();
    z.outside = {};
    z.rowsSum = z.measured = 0.0;
    z.error.clear ();
    sunstudyhud::PlantingSite site;
    if (!sunstudyhud::ReadPlantingSite (studyId, layerHeight, site, z.error))
        return z;
    const std::vector<uint32_t> masks = pl::ZoneMasks (z.zones, site.x, site.y);
    for (size_t i = 0; i < z.zones.size () && i < pl::kMaxPlantingZones; ++i) {
        z.rows.push_back (pl::ClassAreasInZone (masks, 1u << i, site.hours, site.areas, edges));
        z.rowsSum += z.rows.back ().measured;
    }
    z.outside = pl::ClassAreasInZone (masks, 0u, site.hours, site.areas, edges);
    for (double a : site.areas)
        z.measured += a;
    return z;
}

const std::vector<pl::ZoneClassAreas>* ZoneRowsOfLayer (HudState& state, int layer)
{
    pl::PlantingHud& hud = state.planting;
    pl::ZonesState& z = hud.zones;
    if (layer < 0 || size_t (layer) >= hud.layers.size () || z.zones.empty ())
        return nullptr;
    const pl::Layer& l = hud.layers[size_t (layer)];
    const std::vector<double> edges = pl::AreaEdges (hud);
    pl::ZonesState::LayerRows& cached = z.byStudy[l.studyId];
    if (cached.revision != z.revision || cached.edges != edges) {
        cached.revision = z.revision;
        cached.edges = edges;
        cached.rows.clear ();
        cached.error.clear ();
        sunstudyhud::PlantingSite site;
        if (sunstudyhud::ReadPlantingSite (l.studyId, l.height, site, cached.error)) {
            const std::vector<uint32_t> masks = pl::ZoneMasks (z.zones, site.x, site.y);
            for (size_t i = 0; i < z.zones.size () && i < pl::kMaxPlantingZones; ++i)
                cached.rows.push_back (pl::ClassAreasInZone (masks, 1u << i, site.hours, site.areas, edges));
        }
    }
    return cached.rows.size () == z.zones.size () ? &cached.rows : nullptr;
}

pl::PlantFit FitForPlantsTab (HudState& state, const pl::Plant& plant)
{
    pl::PlantingHud& hud = state.planting;
    const pl::PlantFit fit = pl::FitOf (hud, plant);
    const int zone = hud.catalogue.zone;
    if (zone < 0 || size_t (zone) >= hud.zones.zones.size ())
        return fit;
    const auto* rows = ZoneRowsOfLayer (state, fit.layer);
    if (rows == nullptr) {
        pl::PlantFit unknown = fit;
        unknown.status = pl::PlantFit::Status::Unknown;
        unknown.reason = "the zone could not be read on that layer";
        return unknown;
    }
    return pl::FitInZone (hud, plant, fit, (*rows)[size_t (zone)]);
}

} // namespace geomsrv::archviz::plantinghud
