// The Planting tab's Scheme sub-tab (#278): pick catalogue plants, say how many
// of each, and see them placed on a measured layer as crown cylinders joined
// species by species, with the mix's community notes.
//
// The region is a kept layer's samples, read once per layer through the runner
// (ReadPlantingSite, plain values). Placement and notes are PlantingScheme's;
// the viewer geometry is published by PublishSchemeScene
// (DiligentHudPlantingSchemeScene.cpp), which the playground stubs.
//
// ⚠️ A PROPOSAL, NOT A PLAN: nothing is written to the model, and the panel
// says which checks were not made.
//
// RENDER THREAD. Reads HudState and plain values from the runner.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudPlantingParts.hpp"
#include "ArchViz/HudShell.hpp"
#include "NativeCommands/PlantingColumns.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace geomsrv::archviz::plantinghud {

namespace pl = planting;

namespace {

int SiteLayer (const pl::PlantingHud& hud)
{
    const auto& s = hud.scheme;
    if (s.layer >= 0 && size_t (s.layer) < hud.layers.size ())
        return s.layer;
    if (hud.shownLayer >= 0 && size_t (hud.shownLayer) < hud.layers.size ())
        return hud.shownLayer;
    return hud.layers.empty () ? -1 : int (hud.layers.size ()) - 1;
}

// Reads the layer's samples once per study, and reclassifies them when the
// class edges change.
void RefreshSite (pl::PlantingHud& hud, int layer)
{
    pl::SchemeState& s = hud.scheme;
    if (layer < 0) {
        s.site = {};
        s.siteStudy.clear ();
        s.siteHours.clear ();
        return;
    }
    const pl::Layer& l = hud.layers[size_t (layer)];
    if (s.siteStudy != l.studyId) {
        sunstudyhud::PlantingSite read;
        std::string error;
        s.siteStudy = l.studyId;
        s.siteEdges.clear ();
        s.site = {};
        s.siteHours.clear ();
        s.siteHosts.clear ();
        s.siteError.clear ();
        s.siteNoGoRevision = ~uint64_t (0);
        if (!sunstudyhud::ReadPlantingSite (l.studyId, l.height, read, error)) {
            s.siteError = error;
            return;
        }
        s.site.x = std::move (read.x);
        s.site.y = std::move (read.y);
        s.site.groundZ = std::move (read.groundZ);
        s.site.host = std::move (read.groups);
        s.site.area = std::move (read.areas);
        s.siteHours = std::move (read.hours);
        s.siteHosts = std::move (read.meshGuids);
        s.site.layerHeight = l.height;
        pl::SampleEdgeDistances (s.site); // once per layer: where a crown would leave the measured area
    }
    const std::vector<double> edges = pl::AreaEdges (hud);
    if (s.siteEdges != edges || s.site.cls.size () != s.siteHours.size ()) {
        s.site.cls.resize (s.siteHours.size ());
        for (size_t i = 0; i < s.siteHours.size (); ++i)
            s.site.cls[i] = pl::ClassOf (hud, s.siteHours[i]);
        s.siteEdges = edges;
    }
    // No-go zones keep crowns out: their plan distances, read when the zones change.
    if (s.siteNoGoRevision != hud.zones.revision) {
        pl::NoGoDistances (hud.zones.zones, s.site.x, s.site.y, s.site.noGoDistance, s.site.noGoAllowance);
        s.siteNoGoRevision = hud.zones.revision;
    }
}

// Asks the runner for the layer's room (measured once per study, on a worker)
// and takes it into the site when it is ready.
void RefreshRoom (pl::PlantingHud& hud, int layer)
{
    pl::SchemeState& s = hud.scheme;
    s.roomStatus.clear ();
    if (!s.checkSpace || layer < 0 || s.site.Size () == 0)
        return;
    if (s.site.clearance.Count () == s.site.Size ())
        return;
    sunstudyhud::RequestPlantingRoom (s.siteStudy, hud.layers[size_t (layer)].height);
    const sunstudyhud::PlantingRoom room = sunstudyhud::CurrentPlantingRoom ();
    if (room.studyId != s.siteStudy)
        return;
    char text[160];
    switch (room.state) {
        case sunstudyhud::PlantingRoom::State::Measuring:
            std::snprintf (text, sizeof (text), "Measuring the room around each sample... %.0f %%",
                           room.total > 0 ? 100.0 * double (room.done) / double (room.total) : 0.0);
            s.roomStatus = text;
            break;
        case sunstudyhud::PlantingRoom::State::Failed:
            s.roomStatus = "Walls and overhangs not checked: " + room.error;
            break;
        case sunstudyhud::PlantingRoom::State::Ready:
            if (room.clearance && room.clearance->Count () == s.site.Size ())
                s.site.clearance = *room.clearance;
            else
                s.roomStatus = "Walls and overhangs not checked: the room does not match the layer's samples.";
            break;
        case sunstudyhud::PlantingRoom::State::Idle:
            break;
    }
}

// Places again only when something it depends on changed.
void RefreshPlacement (pl::PlantingHud& hud)
{
    pl::SchemeState& s = hud.scheme;
    std::string from = s.siteStudy + "|" + hud.catalogue.plants.catalogueId + "|" +
                       std::to_string (hud.catalogue.plants.plants.size ()) + "|" + std::to_string (s.crownOverlap) +
                       "|" + std::to_string (hud.deepShade) + "|zone" + std::to_string (s.zone) + "@" +
                       std::to_string (hud.zones.revision) + "|room" + std::to_string (s.checkSpace) + "/" +
                       std::to_string (s.clearanceMargin) + "/" + std::to_string (s.site.clearance.Count ()) +
                       "|inside" + std::to_string (s.containCrowns) + "|layout" + std::to_string (s.pattern) + "/" +
                       std::to_string (s.start) + "/" + std::to_string (s.edgeOffset);
    for (double e : s.siteEdges)
        from += "|" + std::to_string (e);
    for (const auto& sp : s.species)
        from += "|" + sp.plantId + "*" + std::to_string (sp.count) + "@" + std::to_string (sp.density);
    if (from == s.placedFrom)
        return;
    pl::SchemeOptions options;
    options.crownOverlap = s.crownOverlap;
    options.checkSpace = s.checkSpace;
    options.clearanceMargin = s.clearanceMargin;
    options.containCrowns = s.containCrowns;
    options.pattern = pl::SchemePattern (s.pattern);
    options.start = pl::SchemeStart (s.start);
    options.edgeOffset = s.edgeOffset;
    s.result = pl::PlaceScheme (hud, hud.catalogue.plants, s.species, SchemeRegionSite (hud), options);
    s.placedFrom = from;
    ++s.revision;
}

// A density cell: the tick takes the recommended density; untick it and the
// slider sets your own, with a tick mark where the recommended one is.
void DensityCell (pl::SchemeSpecies& sp, const pl::Plant& plant, double spacing)
{
    const double recommended = pl::RecommendedDensity (plant);
    bool useRecommended = sp.density <= 0.0;
    if (ImGui::Checkbox ("##recommended", &useRecommended))
        sp.density = useRecommended ? 0.0 : recommended;
    hudshell::ItemTip ("Ticked: the recommended density, %.2f plants per m2 (%s).", recommended,
                       plant.densityPerM2 ? "the catalogue's planting density" : "mature crowns touching");
    ImGui::SameLine (0.0f, ImGui::GetStyle ().ItemInnerSpacing.x);
    const float lo = float (recommended / 8.0), hi = float (recommended * 4.0);
    float value = float (useRecommended ? recommended : sp.density);
    ImGui::SetNextItemWidth (-1);
    ImGui::BeginDisabled (useRecommended);
    if (ImGui::SliderFloat ("##density", &value, lo, hi, value < 1.0f ? "%.3f" : "%.2f",
                            ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
        sp.density = value;
    ImGui::EndDisabled ();
    // The tick mark: where the recommended density sits on the slider.
    const ImVec2 a = ImGui::GetItemRectMin (), b = ImGui::GetItemRectMax ();
    const float pad = ImGui::GetStyle ().GrabMinSize * 0.5f + ImGui::GetStyle ().FramePadding.x * 0.5f;
    const float t = std::log (float (recommended) / lo) / std::log (hi / lo);
    const float x = a.x + pad + t * (b.x - a.x - 2.0f * pad);
    ImGui::GetWindowDrawList ()->AddLine (ImVec2 (x, a.y), ImVec2 (x, a.y + (b.y - a.y) * 0.35f),
                                          ImGui::GetColorU32 (ImGuiCol_Text), 1.5f);
    hudshell::ItemTip ("Plants per m2 on the hex grid; spacing %.2f m. The mark is the recommended %.2f.", spacing,
                       recommended);
}

void Swatch (uint32_t rgba)
{
    const float size = ImGui::GetTextLineHeight ();
    const ImVec2 min = ImGui::GetCursorScreenPos ();
    ImGui::GetWindowDrawList ()->AddRectFilled (min, ImVec2 (min.x + size, min.y + size), AbgrOf (rgba), 2.0f);
    ImGui::Dummy (ImVec2 (size, size));
}

} // namespace

pl::SchemeSite SchemeRegionSite (const pl::PlantingHud& hud)
{
    const pl::SchemeState& s = hud.scheme;
    const auto& zones = hud.zones.zones;
    if (s.zone >= 0 && size_t (s.zone) < zones.size () && zones[size_t (s.zone)].role == pl::ZoneRole::Bed)
        return pl::RestrictToZone (s.site, zones[size_t (s.zone)]);
    if (s.zone == pl::kAllBeds) {
        std::vector<const pl::PlantingZone*> beds;
        for (const auto& z : zones)
            if (z.role == pl::ZoneRole::Bed)
                beds.push_back (&z);
        if (!beds.empty ())
            return pl::RestrictToZones (s.site, beds);
    }
    return s.site;
}

int RefreshSchemeRegion (pl::PlantingHud& hud)
{
    const int layer = SiteLayer (hud);
    RefreshSite (hud, layer);
    RefreshRoom (hud, layer);
    return layer;
}

void DrawSchemeRegion (pl::PlantingHud& hud, int layer)
{
    pl::SchemeState& s = hud.scheme;
    ImGui::SetNextItemWidth (ImGui::GetFontSize () * 12.0f);
    char label[96];
    if (layer >= 0)
        std::snprintf (label, sizeof (label), "%.2f m layer%s", hud.layers[size_t (layer)].height,
                       s.layer < 0 ? " (shown)" : "");
    else
        std::snprintf (label, sizeof (label), "no layer yet");
    if (ImGui::BeginCombo ("Region##schemeregion", label)) {
        if (ImGui::Selectable ("The shown layer", s.layer < 0))
            s.layer = -1;
        for (size_t i = 0; i < hud.layers.size (); ++i) {
            char item[96];
            std::snprintf (item, sizeof (item), "%.2f m  %s##schemelayer%zu", hud.layers[i].height,
                           hud.layers[i].label.c_str (), i);
            if (ImGui::Selectable (item, s.layer == int (i)))
                s.layer = int (i);
        }
        ImGui::EndCombo ();
    }
    hudshell::ItemTip ("Plants are placed on this layer's measured samples, and its light classes decide where each "
                       "may go. Unavailable area (too steep, under a ceiling) is never planted.");
    ImGui::SameLine ();
    const auto& zones = hud.zones.zones;
    const bool anyBed = std::any_of (zones.begin (), zones.end (),
                                     [] (const pl::PlantingZone& z) { return z.role == pl::ZoneRole::Bed; });
    if (s.zone >= int (zones.size ()) || (s.zone >= 0 && zones[size_t (s.zone)].role != pl::ZoneRole::Bed) ||
        (s.zone == pl::kAllBeds && !anyBed) || s.zone < pl::kAllBeds)
        s.zone = -1;
    ImGui::SetNextItemWidth (ImGui::GetFontSize () * 9.0f);
    const std::string zoneLabel = s.zone == pl::kAllBeds ? std::string ("all beds")
                                  : s.zone < 0           ? std::string ("whole layer")
                                                         : zones[size_t (s.zone)].name;
    ImGui::BeginDisabled (zones.empty ());
    if (ImGui::BeginCombo ("Zone##schemezone", zoneLabel.c_str ())) {
        if (ImGui::Selectable ("whole layer", s.zone == -1))
            s.zone = -1;
        if (anyBed && ImGui::Selectable ("all beds", s.zone == pl::kAllBeds))
            s.zone = pl::kAllBeds;
        for (size_t i = 0; i < zones.size (); ++i) {
            if (zones[i].role != pl::ZoneRole::Bed)
                continue; // a no-go zone is never a region
            const std::string item = zones[i].name + "##schemezone" + std::to_string (i);
            if (ImGui::Selectable (item.c_str (), s.zone == int (i)))
                s.zone = int (i);
        }
        ImGui::EndCombo ();
    }
    ImGui::EndDisabled ();
    hudshell::ItemTip (zones.empty () ? "Read zones first: Light > Planting areas > Zones from selection."
                                      : "Place only inside this zone in plan, or inside every bed at once (all beds).");
    if (!s.siteError.empty ())
        ImGui::TextColored (ImVec4 (0.95f, 0.45f, 0.40f, 1.0f), "Region not read: %s", s.siteError.c_str ());
    else if (layer >= 0)
        ImGui::TextDisabled ("%zu samples on %zu host(s), read at %.2f m.", s.site.Size (), s.siteHosts.size (),
                             s.site.layerHeight);
    ImGui::Checkbox ("Clear of walls and overhangs##scheme", &s.checkSpace);
    hudshell::ItemTip ("Keep each mature crown, as a cylinder of its spread and height, clear of the study's own "
                       "geometry: balconies and eaves above, walls beside. Measured once per layer with horizontal "
                       "rays at levels up to 25 m; a slender post between rays can be missed. Roots are not "
                       "assessed.");
    ImGui::SameLine ();
    ImGui::SetNextItemWidth (ImGui::GetFontSize () * 6.0f);
    ImGui::SliderFloat ("Margin##schemeclearance", &s.clearanceMargin, 0.0f, 2.0f, "%.2f m");
    hudshell::ItemTip ("Kept clear around and above each mature crown, beyond its own size: from walls and "
                       "overhangs, and from no-go zones. It moves placed plants and redraws the Fit map; with "
                       "nothing placed, the viewer does not change.");
    if (!s.roomStatus.empty ())
        ImGui::TextDisabled ("%s", s.roomStatus.c_str ());
    if (!s.site.noGoAllowance.empty ())
        ImGui::TextDisabled ("%zu no-go zone(s) keep crowns out.", s.site.noGoAllowance.size ());
}

namespace {

void SpeciesTable (pl::PlantingHud& hud)
{
    pl::SchemeState& s = hud.scheme;
    const auto& plants = hud.catalogue.plants;
    if (s.species.empty ()) {
        ImGui::TextDisabled ("No plants yet. Pick one in Plants and press Add to scheme.");
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg;
    if (!ImGui::BeginTable ("##scheme-species", 6, flags))
        return;
    ImGui::TableSetupColumn ("##colour", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight ());
    ImGui::TableSetupColumn ("Plant", ImGuiTableColumnFlags_WidthStretch, 2.4f);
    ImGui::TableSetupColumn ("Count", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize () * 4.5f);
    ImGui::TableSetupColumn ("Density /m2", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize () * 7.5f);
    ImGui::TableSetupColumn ("Placed", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize () * 3.2f);
    ImGui::TableSetupColumn ("##remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize () * 1.4f);
    ImGui::TableHeadersRow ();
    int remove = -1;
    for (size_t i = 0; i < s.species.size (); ++i) {
        ImGui::PushID (int (i));
        const int at = pl::FindPlant (plants, s.species[i].plantId);
        ImGui::TableNextRow ();
        ImGui::TableNextColumn ();
        Swatch (pl::SchemeSpeciesRgba (int (i)));
        ImGui::TableNextColumn ();
        const std::string name = at >= 0 ? plants.plants[size_t (at)].DisplayName () : s.species[i].plantId;
        if (ImGui::Selectable (name.c_str (), s.highlight == int (i)))
            s.highlight = s.highlight == int (i) ? -1 : int (i);
        hudshell::ItemTip ("Select to emphasise this species in the viewer.");
        ImGui::TableNextColumn ();
        ImGui::SetNextItemWidth (-1);
        int count = s.species[i].count;
        if (ImGui::InputInt ("##count", &count, 1, 5))
            s.species[i].count = std::clamp (count, 0, 400);
        hudshell::ItemTip ("How many to place; 0 places as many as fit at the density.");
        ImGui::TableNextColumn ();
        if (at >= 0)
            DensityCell (s.species[i], plants.plants[size_t (at)],
                         i < s.result.spacing.size () ? s.result.spacing[i] : 0.0);
        ImGui::TableNextColumn ();
        if (i < s.result.placed.size ()) {
            const int placed = s.result.placed[i], wanted = s.result.wanted[i];
            if (wanted > 0 && placed < wanted)
                ImGui::TextColored (ImVec4 (0.95f, 0.7f, 0.3f, 1.0f), "%d/%d", placed, wanted);
            else
                ImGui::Text ("%d", placed);
            if (i < s.result.sizes.size () && i < s.result.room.size ()) {
                const pl::SchemeRoom& room = s.result.room[i];
                hudshell::ItemTip ("Crown radius %.2f m (from %s), height %.2f m (from %s).\n"
                                   "Samples on its light: %zu. Room for the crown: %zu; under an overhang: %zu; "
                                   "too close to a wall: %zu; wider than measured: %zu; in a no-go zone: %zu.%s",
                                   s.result.sizes[i].radius, s.result.sizes[i].radiusFrom.c_str (),
                                   s.result.sizes[i].height, s.result.sizes[i].heightFrom.c_str (), room.lightSamples,
                                   room.fits, room.overhead, room.narrow, room.beyond, room.noGo,
                                   s.result.spaceChecked ? "" : "\nWalls and overhangs not checked.");
            }
            else if (i < s.result.sizes.size ())
                hudshell::ItemTip ("Crown radius %.2f m (from %s), marker height %.2f m (from %s).\n"
                                   "Walls and overhangs not checked.",
                                   s.result.sizes[i].radius, s.result.sizes[i].radiusFrom.c_str (),
                                   s.result.sizes[i].height, s.result.sizes[i].heightFrom.c_str ());
        }
        ImGui::TableNextColumn ();
        if (ImGui::SmallButton ("x"))
            remove = int (i);
        hudshell::ItemTip ("Remove from the scheme.");
        ImGui::PopID ();
    }
    ImGui::EndTable ();
    if (remove >= 0) {
        s.species.erase (s.species.begin () + remove);
        if (s.highlight == remove)
            s.highlight = -1;
        else if (s.highlight > remove)
            --s.highlight;
    }
}

// Create Columns: the placed plants as round Columns in Archicad, on request.
void ColumnsRow (pl::SchemeState& s)
{
    const plantingcolumns::Status status = plantingcolumns::Current ();
    if (status.serial != s.columnsSeen) {
        s.columnsSeen = status.serial;
        s.columnsStatus = status.message;
    }
    ImGui::BeginDisabled (s.result.plants.empty () || status.busy);
    if (ImGui::Button ("Create Columns in Archicad...##scheme"))
        ImGui::OpenPopup ("Create Columns?##scheme");
    ImGui::EndDisabled ();
    hudshell::ItemTip ("Write the placed plants to Archicad as round Columns, 0.3 m wide: trees 3 m tall, shrubs "
                       "1.5 m, smaller plants 0.5 m, each standing on its host. Layer, material and story follow "
                       "Archicad's Column tool.");
    if (ImGui::BeginPopup ("Create Columns?##scheme")) {
        ImGui::Text ("%zu round Columns, one per placed plant.", s.result.plants.size ());
        ImGui::TextDisabled ("One Undo in Archicad removes them all.");
        if (ImGui::Button ("Create")) {
            std::vector<plantingcolumns::Column> columns;
            columns.reserve (s.result.plants.size ());
            for (const pl::PlacedPlant& p : s.result.plants)
                columns.push_back ({ p.x, p.y, p.z, p.schematicHeight });
            std::string error;
            s.columnsStatus = plantingcolumns::Request (std::move (columns), pl::kSchematicDiameter, error)
                                  ? "Creating the Columns..."
                                  : "The Columns were not created: " + error;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if (ImGui::Button ("Cancel"))
            ImGui::CloseCurrentPopup ();
        ImGui::EndPopup ();
    }
    if (!s.columnsStatus.empty ()) {
        ImGui::SameLine ();
        ImGui::TextWrapped ("%s", s.columnsStatus.c_str ());
    }
}

void Notes (const pl::SchemeState& s)
{
    if (s.result.notes.empty ())
        return;
    ImGui::SeparatorText ("Community");
    for (const pl::SchemeNote& n : s.result.notes) {
        const ImVec4 colour = n.severity == pl::SchemeSeverity::Conflict  ? ImVec4 (0.95f, 0.45f, 0.40f, 1.0f)
                              : n.severity == pl::SchemeSeverity::Warning ? ImVec4 (0.95f, 0.72f, 0.30f, 1.0f)
                                                                          : ImGui::GetStyle ().Colors[ImGuiCol_Text];
        ImGui::PushStyleColor (ImGuiCol_Text, colour);
        ImGui::TextWrapped ("%s", n.text.c_str ());
        ImGui::PopStyleColor ();
    }
    ImGui::TextDisabled ("Site-first: light, water and ground before competition. Above-ground fit only: roots, "
                         "utilities, paths and spacing rules are not checked.");
}

} // namespace

void AddToScheme (pl::PlantingHud& hud, const pl::Plant& plant)
{
    auto& species = hud.scheme.species;
    for (const auto& sp : species)
        if (sp.plantId == plant.id)
            return;
    const int count = plant.habit == pl::Habit::Tree ? 1 : plant.habit == pl::Habit::Shrub ? 3 : 7;
    species.push_back ({ plant.id, count });
}

void DrawSchemeTab (HudState& state)
{
    pl::PlantingHud& hud = state.planting;
    hud.highlightMask = 0;
    EnsureCatalogue (state);
    pl::SchemeState& s = hud.scheme;
    const int layer = RefreshSchemeRegion (hud);
    RefreshPlacement (hud);

    ImGui::TextWrapped ("Try a planting mix: each plant is placed where its light suits it and its grown crown has "
                        "room, then checked against its neighbours.");
    DrawSchemeRegion (hud, layer);
    ImGui::Separator ();
    SpeciesTable (hud);
    if (hud.catalogue.selected >= 0 && size_t (hud.catalogue.selected) < hud.catalogue.plants.plants.size ()) {
        const pl::Plant& p = hud.catalogue.plants.plants[size_t (hud.catalogue.selected)];
        const std::string label = "Add " + p.DisplayName () + "##schemeadd";
        if (ImGui::SmallButton (label.c_str ()))
            AddToScheme (hud, p);
        hudshell::ItemTip ("The plant selected in Plants.");
    }

    // Placement settings change only placed plants: inert, and said so, until there are some.
    ImGui::BeginDisabled (s.species.empty ());
    ImGui::SetNextItemWidth (ImGui::GetFontSize () * 8.0f);
    float overlap = s.crownOverlap * 100.0f;
    if (ImGui::SliderFloat ("Crown overlap##scheme", &overlap, 0.0f, 50.0f, "%.0f %%"))
        s.crownOverlap = overlap / 100.0f;
    hudshell::ItemTip ("How far neighbouring mature crowns may overlap, as a share of the smaller crown's radius. "
                       "0 keeps them touching at most.");
    ImGui::SameLine ();
    ImGui::Checkbox ("Crowns inside the region##scheme", &s.containCrowns);
    hudshell::ItemTip ("Keep each mature crown inside the region in plan: inside the chosen zone, and inside the "
                       "measured layer's outline. Off, a crown may overhang the bed's edge.");
    ImGui::SetNextItemWidth (ImGui::GetFontSize () * 6.5f);
    ImGui::Combo ("##schemepattern", &s.pattern, "Hex grid\0Spread out\0");
    hudshell::ItemTip ("Hex grid: an even, packed grid at each species' density, the layout of a planting plan. "
                       "Spread out: each plant as far as possible from the others, for a few specimens.");
    ImGui::SameLine ();
    ImGui::BeginDisabled (s.pattern != int (pl::SchemePattern::Hex));
    ImGui::SetNextItemWidth (ImGui::GetFontSize () * 6.5f);
    ImGui::Combo ("##schemestart", &s.start, "From centre\0From corner\0");
    ImGui::EndDisabled ();
    hudshell::ItemTip ("Where the grid starts, and which points are planted first when the count is less than what "
                       "fits: outward from the middle of the usable area, or from its lower-left corner.");
    ImGui::SameLine ();
    ImGui::SetNextItemWidth (ImGui::GetFontSize () * 5.5f);
    ImGui::DragFloat ("Edge offset##scheme", &s.edgeOffset, 0.01f, 0.0f, 5.0f, "%.2f m");
    s.edgeOffset = std::clamp (s.edgeOffset, 0.0f, 5.0f);
    hudshell::ItemTip ("Kept clear inside the region's edge: beyond each crown when crowns stay inside the region, "
                       "else from each plant's centre.");
    ImGui::RadioButton ("Schematic##schememarker", &s.marker, int (pl::SchemeMarker::Schematic));
    hudshell::ItemTip ("A 0.3 m stem: trees 3 m tall, shrubs 1.5 m, smaller plants 0.5 m, with the mature crown's "
                       "outline on the ground. What Create Columns makes.");
    ImGui::SameLine ();
    ImGui::RadioButton ("Disc##schememarker", &s.marker, int (pl::SchemeMarker::Disc));
    hudshell::ItemTip ("A low cylinder at each plant's mature crown radius: the plan reads.");
    ImGui::SameLine ();
    ImGui::RadioButton ("Mature height##schememarker", &s.marker, int (pl::SchemeMarker::Mature));
    hudshell::ItemTip ("Each cylinder drawn to the catalogue's mature height (its stated end when open, as in 16+).");
    ImGui::SameLine ();
    ImGui::Checkbox ("Show in viewer##scheme", &s.show);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Clear##scheme")) {
        s.species.clear ();
        s.highlight = -1;
    }
    ImGui::EndDisabled ();
    if (!s.species.empty ())
        ImGui::Text ("%zu plant(s) placed.", s.result.plants.size ());
    Notes (s);
    ColumnsRow (s);
}

} // namespace geomsrv::archviz::plantinghud
