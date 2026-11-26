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
#include "NativeCommands/SunStudyHudRunner.hpp"

#include <imgui.h>

#include <algorithm>
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
        if (!sunstudyhud::ReadPlantingSite (l.studyId, l.height, read, error)) {
            s.siteError = error;
            return;
        }
        s.site.x = std::move (read.x);
        s.site.y = std::move (read.y);
        s.site.groundZ = std::move (read.groundZ);
        s.site.host = std::move (read.groups);
        s.siteHours = std::move (read.hours);
        s.siteHosts = std::move (read.meshGuids);
        s.site.layerHeight = l.height;
    }
    const std::vector<double> edges = pl::AreaEdges (hud);
    if (s.siteEdges != edges || s.site.cls.size () != s.siteHours.size ()) {
        s.site.cls.resize (s.siteHours.size ());
        for (size_t i = 0; i < s.siteHours.size (); ++i)
            s.site.cls[i] = pl::ClassOf (hud, s.siteHours[i]);
        s.siteEdges = edges;
    }
}

// Places again only when something it depends on changed.
void RefreshPlacement (pl::PlantingHud& hud)
{
    pl::SchemeState& s = hud.scheme;
    std::string from = s.siteStudy + "|" + hud.catalogue.plants.catalogueId + "|" +
                       std::to_string (hud.catalogue.plants.plants.size ()) + "|" + std::to_string (s.crownOverlap) +
                       "|" + std::to_string (hud.deepShade) + "|zone" + std::to_string (s.zone) + "@" +
                       std::to_string (hud.zones.revision);
    for (double e : s.siteEdges)
        from += "|" + std::to_string (e);
    for (const auto& sp : s.species)
        from += "|" + sp.plantId + "*" + std::to_string (sp.count);
    if (from == s.placedFrom)
        return;
    pl::SchemeOptions options;
    options.crownOverlap = s.crownOverlap;
    const bool inZone = s.zone >= 0 && size_t (s.zone) < hud.zones.zones.size ();
    s.result =
        pl::PlaceScheme (hud, hud.catalogue.plants, s.species,
                         inZone ? pl::RestrictToZone (s.site, hud.zones.zones[size_t (s.zone)]) : s.site, options);
    s.placedFrom = from;
    ++s.revision;
}

void Swatch (uint32_t rgba)
{
    const float size = ImGui::GetTextLineHeight ();
    const ImVec2 min = ImGui::GetCursorScreenPos ();
    ImGui::GetWindowDrawList ()->AddRectFilled (min, ImVec2 (min.x + size, min.y + size), AbgrOf (rgba), 2.0f);
    ImGui::Dummy (ImVec2 (size, size));
}

void RegionRow (pl::PlantingHud& hud, int layer)
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
    if (s.zone >= int (zones.size ()))
        s.zone = -1;
    ImGui::SetNextItemWidth (ImGui::GetFontSize () * 9.0f);
    const std::string zoneLabel = s.zone < 0 ? std::string ("whole layer") : zones[size_t (s.zone)].name;
    ImGui::BeginDisabled (zones.empty ());
    if (ImGui::BeginCombo ("Zone##schemezone", zoneLabel.c_str ())) {
        if (ImGui::Selectable ("whole layer", s.zone < 0))
            s.zone = -1;
        for (size_t i = 0; i < zones.size (); ++i) {
            const std::string item = zones[i].name + "##schemezone" + std::to_string (i);
            if (ImGui::Selectable (item.c_str (), s.zone == int (i)))
                s.zone = int (i);
        }
        ImGui::EndCombo ();
    }
    ImGui::EndDisabled ();
    hudshell::ItemTip (zones.empty () ? "Read zones first: Light > Planting areas > Zones from polylines."
                                      : "Place only on the samples inside this zone, in plan.");
    if (!s.siteError.empty ())
        ImGui::TextColored (ImVec4 (0.95f, 0.45f, 0.40f, 1.0f), "Region not read: %s", s.siteError.c_str ());
    else if (layer >= 0)
        ImGui::TextDisabled ("%zu samples on %zu host(s), read at %.2f m.", s.site.Size (), s.siteHosts.size (),
                             s.site.layerHeight);
}

void SpeciesTable (pl::PlantingHud& hud)
{
    pl::SchemeState& s = hud.scheme;
    const auto& plants = hud.catalogue.plants;
    if (s.species.empty ()) {
        ImGui::TextDisabled ("No plants yet. Pick one in Plants and press Add to scheme.");
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg;
    if (!ImGui::BeginTable ("##scheme-species", 5, flags))
        return;
    ImGui::TableSetupColumn ("##colour", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight ());
    ImGui::TableSetupColumn ("Plant", ImGuiTableColumnFlags_WidthStretch, 2.4f);
    ImGui::TableSetupColumn ("Count", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize () * 4.5f);
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
        hudshell::ItemTip ("How many to place; 0 places as many as fit.");
        ImGui::TableNextColumn ();
        if (i < s.result.placed.size ()) {
            const int placed = s.result.placed[i], wanted = s.result.wanted[i];
            if (wanted > 0 && placed < wanted)
                ImGui::TextColored (ImVec4 (0.95f, 0.7f, 0.3f, 1.0f), "%d/%d", placed, wanted);
            else
                ImGui::Text ("%d", placed);
            if (i < s.result.sizes.size ())
                hudshell::ItemTip ("Crown radius %.2f m (from %s), marker height %.2f m (from %s).",
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
    ImGui::TextDisabled ("Site-first: light, water and ground before competition. Roots, utilities, paths and "
                         "spacing rules are not checked.");
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
    const int layer = SiteLayer (hud);
    RefreshSite (hud, layer);
    RefreshPlacement (hud);

    RegionRow (hud, layer);
    ImGui::Separator ();
    SpeciesTable (hud);
    if (hud.catalogue.selected >= 0 && size_t (hud.catalogue.selected) < hud.catalogue.plants.plants.size ()) {
        const pl::Plant& p = hud.catalogue.plants.plants[size_t (hud.catalogue.selected)];
        const std::string label = "Add " + p.DisplayName () + "##schemeadd";
        if (ImGui::SmallButton (label.c_str ()))
            AddToScheme (hud, p);
        hudshell::ItemTip ("The plant selected in Plants.");
    }

    ImGui::SetNextItemWidth (ImGui::GetFontSize () * 8.0f);
    float overlap = s.crownOverlap * 100.0f;
    if (ImGui::SliderFloat ("Crown overlap##scheme", &overlap, 0.0f, 50.0f, "%.0f %%"))
        s.crownOverlap = overlap / 100.0f;
    hudshell::ItemTip ("How far neighbouring mature crowns may overlap, as a share of the smaller crown's radius. "
                       "0 keeps them touching at most.");
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
    if (!s.species.empty ())
        ImGui::Text ("%zu plant(s) placed.", s.result.plants.size ());
    Notes (s);
    ImGui::BeginDisabled ();
    ImGui::SmallButton ("Bake to model...##scheme");
    ImGui::EndDisabled ();
    hudshell::ItemTip ("Later: place approved plants as objects on a dedicated layer. Never automatic.");
}

} // namespace geomsrv::archviz::plantinghud
