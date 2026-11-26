// The Planting tab's Plants sub-tab (#278 Stage 3): the local catalogue, its
// search and filters, each plant's light fit at the layer nearest its height,
// and the highlight that dims the classes a selected plant does not accept.
//
// ⚠️ UNKNOWN IS NOT A MATCH (ArchViz/PlantCatalogue.hpp). A plant without light
// classes, or one whose layer has not been measured, says why it is unknown.
//
// RENDER THREAD. The catalogue files are small and read on a click; opening
// the folder in Explorer runs on its own short-lived thread, outside ImGui.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudPlantingParts.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/PlantHardiness.hpp"
#include "ArchViz/PlantingFiles.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#endif

namespace geomsrv::archviz::plantinghud {

namespace pl = planting;

void OpenFolderInExplorer (const std::filesystem::path& folder)
{
#ifdef _WIN32
    std::thread ([path = folder.wstring ()] () {
        const HRESULT com = ::CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ::ShellExecuteW (nullptr, L"open", path.c_str (), nullptr, nullptr, SW_SHOWNORMAL);
        if (SUCCEEDED (com))
            ::CoUninitialize ();
    }).detach ();
#else
    (void) folder;
#endif
}

void ReloadCatalogue (pl::CatalogueState& cat)
{
    cat.attempted = true;
    const auto folder = pl::TapiocaDataFolder (L"Planting");
    std::vector<std::string> created;
    std::string error;
    if (!pl::SeedCatalogueFolder (folder, created, error)) {
        cat.problems = { error };
        cat.status = "The catalogue folder could not be prepared.";
        return;
    }
    // An unedited copy of an older shipped seed takes the newer one (backed up first).
    std::vector<std::string> updated;
    std::string backup;
    const bool refreshed = pl::RefreshShippedCatalogue (folder, updated, backup, error);
    pl::ReadCatalogueFolder (folder, cat);
    for (const auto& name : created)
        cat.status += " Created " + name + ".";
    for (const auto& name : updated)
        cat.status += " Updated " + name + " to the newer shipped catalogue (it was unedited; backup *" + backup + ").";
    if (!refreshed)
        cat.problems.push_back ("The newer shipped catalogue was not applied: " + error);
    cat.edited = pl::PlantsFileIsEdited (folder);
}

void EnsureCatalogue (HudState& state)
{
    if (!state.planting.catalogue.attempted)
        ReloadCatalogue (state.planting.catalogue);
}

namespace {

std::string LightLetters (const pl::Plant& p)
{
    std::string s;
    for (int c = 0; c < pl::kLightClassCount; ++c)
        if (p.light[size_t (c)])
            s += (s.empty () ? "" : ", ") + std::string (pl::LightName (pl::LightClass (c)));
    return s.empty () ? "not given" : s;
}

std::string RangeText (const std::optional<pl::Range>& r)
{
    if (!r)
        return "not given";
    char text[64];
    if (r->openMin && r->openMax)
        return "not given";
    if (r->openMin)
        std::snprintf (text, sizeof (text), "up to %.2f m", r->max);
    else if (r->openMax)
        std::snprintf (text, sizeof (text), "%.2f m and more", r->min);
    else
        std::snprintf (text, sizeof (text), "%.2f to %.2f m", r->min, r->max);
    return text;
}

std::string MoistureText (const pl::Plant& p)
{
    std::string s;
    for (pl::MoistureClass m : p.moistureClasses)
        s += (s.empty () ? "" : ", ") + std::string (pl::MoistureClassName (m));
    return s;
}

std::string LayerText (const pl::Plant& p)
{
    std::string s;
    for (pl::PlantingLayer l : p.plantingLayers)
        s += (s.empty () ? "" : ", ") + std::string (pl::PlantingLayerName (l));
    return s;
}

std::string SitesText (const pl::Plant& p)
{
    std::string s;
    for (int i = 0; i < pl::kSiteCount; ++i) {
        if (!p.siteSuitable[size_t (i)])
            continue;
        s += (s.empty () ? "" : ", ") + std::string (pl::SiteName (pl::Site (i)));
        if (!p.siteSoil[size_t (i)].empty ())
            s += " (" + p.siteSoil[size_t (i)] + ")";
    }
    return s;
}

void CatalogueRow (HudState& state)
{
    pl::CatalogueState& cat = state.planting.catalogue;
    if (ImGui::Button ("Reload##plantcat"))
        ReloadCatalogue (cat);
    hudshell::ItemTip ("Read plants.json and profiles.json again. A file with a problem is refused whole and the "
                       "previous catalogue stays in use.");
    ImGui::SameLine ();
    if (ImGui::Button ("Open folder##plantcat"))
        OpenFolderInExplorer (pl::TapiocaDataFolder (L"Planting"));
    hudshell::ItemTip ("%s", cat.folder.c_str ());
    ImGui::SameLine ();
    if (ImGui::Button ("Restore defaults...##plantcat"))
        ImGui::OpenPopup ("Restore the shipped catalogue?##plantcat");
    hudshell::ItemTip ("Replace both files with the shipped Vilnius 2024 catalogue, after copying them to a dated "
                       "backup.");
    if (ImGui::BeginPopup ("Restore the shipped catalogue?##plantcat")) {
        ImGui::TextUnformatted ("plants.json and profiles.json are copied to a dated backup,\nthen replaced with the "
                                "shipped catalogue.");
        if (ImGui::Button ("Back up and restore")) {
            std::string backup, error;
            if (pl::RestoreCatalogueDefaults (pl::TapiocaDataFolder (L"Planting"), backup, error)) {
                ReloadCatalogue (cat);
                cat.status += " Backup: *" + backup + ".";
            }
            else {
                cat.problems = { error };
            }
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if (ImGui::Button ("Cancel"))
            ImGui::CloseCurrentPopup ();
        ImGui::EndPopup ();
    }
    if (ImGui::Button ("Export CSV##plantcat")) {
        std::string error;
        if (pl::ExportCatalogueCsv (pl::TapiocaDataFolder (L"Planting"), cat.plants, error))
            cat.status = std::string ("Wrote ") + pl::kCsvExportFile + " in the catalogue folder.";
        else
            cat.problems = { error };
    }
    hudshell::ItemTip ("One row per plant, for a spreadsheet: lists are joined with '|'. plants.json stays the "
                       "catalogue.");
    ImGui::SameLine ();
    if (ImGui::Button ("Import plants.csv...##plantcat"))
        ImGui::OpenPopup ("Import plants.csv?##plantcat");
    hudshell::ItemTip ("Replace plants.json with the plants in plants.csv from the catalogue folder, after a "
                       "dated backup. Same columns as the export.");
    if (ImGui::BeginPopup ("Import plants.csv?##plantcat")) {
        ImGui::TextUnformatted ("plants.csv is checked like plants.json. If it has no problem,\nplants.json is "
                                "copied to a dated backup and replaced.");
        if (ImGui::Button ("Back up and import")) {
            std::string backup;
            if (pl::ImportCatalogueCsv (pl::TapiocaDataFolder (L"Planting"), cat, backup))
                cat.status += " Imported plants.csv; backup: plants" + backup + ".";
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if (ImGui::Button ("Cancel"))
            ImGui::CloseCurrentPopup ();
        ImGui::EndPopup ();
    }
    ImGui::TextWrapped ("%s", cat.status.c_str ());
    if (cat.edited)
        ImGui::TextDisabled ("plants.json is your own (edited or imported): newer shipped data is not applied to it. "
                             "Restore defaults takes the shipped catalogue, after a backup.");
    if (!cat.problems.empty () && ImGui::TreeNodeEx ("Problems##plantcat", ImGuiTreeNodeFlags_DefaultOpen,
                                                     "%d problem(s)", int (cat.problems.size ()))) {
        ImGui::PushStyleColor (ImGuiCol_Text, ImVec4 (0.95f, 0.45f, 0.40f, 1.0f));
        for (const auto& p : cat.problems)
            ImGui::TextWrapped ("%s", p.c_str ());
        ImGui::PopStyleColor ();
        ImGui::TreePop ();
    }
    // ⚠️ NO SOURCE LINES in the tab (the owner's decision, 2026-11-26): provenance
    // stays in the catalogue files and the private sources record, never on screen.
}

void Filters (pl::CatalogueState& cat, int designZone, const std::vector<pl::PlantingZone>& zones)
{
    char text[128] = {};
    std::snprintf (text, sizeof (text), "%s", cat.filter.text.c_str ());
    ImGui::SetNextItemWidth (ImGui::GetFontSize () * 9.0f);
    if (ImGui::InputTextWithHint ("##plantsearch", "search names", text, sizeof (text)))
        cat.filter.text = text;
    ImGui::SameLine ();
    ImGui::SetNextItemWidth (ImGui::GetFontSize () * 6.5f);
    const char* habitLabel = cat.filter.habit < 0 ? "any habit" : pl::HabitName (pl::Habit (cat.filter.habit));
    if (ImGui::BeginCombo ("##planthabit", habitLabel)) {
        if (ImGui::Selectable ("any habit", cat.filter.habit < 0))
            cat.filter.habit = -1;
        for (int h = 1; h < pl::kHabitCount; ++h)
            if (ImGui::Selectable (pl::HabitName (pl::Habit (h)), cat.filter.habit == h))
                cat.filter.habit = h;
        ImGui::EndCombo ();
    }
    ImGui::SameLine ();
    ImGui::SetNextItemWidth (ImGui::GetFontSize () * 6.5f);
    const char* lightLabel = cat.filter.light < 0 ? "any light" : pl::LightName (pl::LightClass (cat.filter.light));
    if (ImGui::BeginCombo ("##plantlight", lightLabel)) {
        if (ImGui::Selectable ("any light", cat.filter.light < 0))
            cat.filter.light = -1;
        for (int c = 0; c < pl::kLightClassCount; ++c)
            if (ImGui::Selectable (pl::LightName (pl::LightClass (c)), cat.filter.light == c))
                cat.filter.light = c;
        ImGui::EndCombo ();
    }
    hudshell::ItemTip ("Plants that accept this class.");

    // The standard's own columns. A blank cell never matches a filter.
    auto combo = [] (const char* id, const char* any, int& value, int first, int count, const char* (*name) (int),
                     float widthEm) {
        ImGui::SetNextItemWidth (ImGui::GetFontSize () * widthEm);
        if (ImGui::BeginCombo (id, value < 0 ? any : name (value))) {
            if (ImGui::Selectable (any, value < 0))
                value = -1;
            for (int i = first; i < count; ++i)
                if (ImGui::Selectable (name (i), value == i))
                    value = i;
            ImGui::EndCombo ();
        }
    };
    combo (
        "##plantsection", "any section", cat.filter.section, 1, pl::kSectionCount,
        [] (int i) { return pl::SectionName (pl::PlantSection (i)); }, 6.5f);
    hudshell::ItemTip ("The catalogue's table: trees, shrubs, or perennials and subshrubs.");
    ImGui::SameLine ();
    combo (
        "##plantsite", "any site", cat.filter.site, 0, pl::kSiteCount,
        [] (int i) { return pl::SiteName (pl::Site (i)); }, 7.5f);
    hudshell::ItemTip ("Plants the catalogue recommends for this kind of site. The perennials table gives no sites.");
    ImGui::SameLine ();
    combo (
        "##plantwater", "any water", cat.filter.moisture, 0, pl::kMoistureClassCount,
        [] (int i) { return pl::MoistureClassName (pl::MoistureClass (i)); }, 7.5f);
    hudshell::ItemTip ("Plants listed with this water need. The trees table gives none.");
    ImGui::SameLine ();
    ImGui::Checkbox ("BREEAM##plantbreeam", &cat.filter.breeamOnly);
    hudshell::ItemTip ("Only plants the catalogue marks for BREEAM.");
    {
        const int zone = designZone;
        if (zone < 0)
            cat.filter.hardyZone = -1;
        ImGui::SameLine ();
        ImGui::BeginDisabled (zone < 0);
        bool hardy = cat.filter.hardyZone >= 0;
        const std::string label =
            zone >= 0 ? "Hardy in " + pl::UsdaName (zone) + "##planthardy" : std::string ("Hardy here##planthardy");
        if (ImGui::Checkbox (label.c_str (), &hardy))
            cat.filter.hardyZone = hardy ? zone : -1;
        if (hardy && cat.filter.hardyZone != zone)
            cat.filter.hardyZone = zone; // the design zone moved
        ImGui::EndDisabled ();
        hudshell::ItemTip (zone >= 0 ? "Only plants whose catalogue rating (USDA or RHS) is hardy in the design zone. "
                                       "A plant without a rating is left out."
                                     : "Set a design zone first: Light > Climate > Winter hardiness.");
    }
    {
        if (cat.zone >= int (zones.size ()) || (cat.zone >= 0 && zones[size_t (cat.zone)].role != pl::ZoneRole::Bed))
            cat.zone = -1;
        ImGui::SameLine ();
        ImGui::SetNextItemWidth (ImGui::GetFontSize () * 7.5f);
        ImGui::BeginDisabled (zones.empty ());
        const std::string label = cat.zone < 0 ? std::string ("whole layer") : zones[size_t (cat.zone)].name;
        if (ImGui::BeginCombo ("##plantzone", label.c_str ())) {
            if (ImGui::Selectable ("whole layer", cat.zone < 0))
                cat.zone = -1;
            for (size_t i = 0; i < zones.size (); ++i) {
                if (zones[i].role != pl::ZoneRole::Bed)
                    continue;
                const std::string item = zones[i].name + "##plantzone" + std::to_string (i);
                if (ImGui::Selectable (item.c_str (), cat.zone == int (i)))
                    cat.zone = int (i);
            }
            ImGui::EndCombo ();
        }
        ImGui::EndDisabled ();
        hudshell::ItemTip (zones.empty () ? "Read zones first: Light > Planting areas > Zones from selection."
                                          : "Read Fits inside this bed, on each plant's own layer, and list only the "
                                            "plants with compatible area there.");
    }
    if (cat.filter.Any () || !cat.filter.text.empty () || cat.zone >= 0) {
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Clear##plantfilters")) {
            cat.filter = {};
            cat.zone = -1;
        }
    }
    if (cat.filter.light < 0)
        cat.filterFromPointHeight = -1;
    if (cat.filterFromPointHeight >= 0)
        ImGui::TextDisabled ("Light from a point at the %.2f m layer: %s.", cat.filterFromPointHeight,
                             pl::LightName (pl::LightClass (cat.filter.light)));
}

std::string FitText (const pl::PlantFit& fit)
{
    char text[96];
    switch (fit.status) {
        case pl::PlantFit::Status::Fits:
            std::snprintf (text, sizeof (text), "%.0f%% (%.1f m²)", fit.Share () * 100.0, fit.compatibleArea);
            return text;
        case pl::PlantFit::Status::None:
            return "no compatible area";
        default:
            return "unknown";
    }
}

void PlantTable (HudState& state)
{
    pl::PlantingHud& hud = state.planting;
    pl::CatalogueState& cat = hud.catalogue;
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    const float rows = float (std::clamp<size_t> (cat.plants.plants.size (), 3, 10));
    if (!ImGui::BeginTable ("##planting-plants", 4, flags, ImVec2 (0, ImGui::GetFrameHeight () * (rows + 1.2f))))
        return;
    ImGui::TableSetupScrollFreeze (0, 1);
    ImGui::TableSetupColumn ("Plant", ImGuiTableColumnFlags_WidthStretch, 2.2f);
    ImGui::TableSetupColumn ("Light", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn ("At", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize () * 3.2f);
    ImGui::TableSetupColumn ("Fits", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableHeadersRow ();
    int shown = 0;
    for (size_t i = 0; i < cat.plants.plants.size (); ++i) {
        const pl::Plant& p = cat.plants.plants[i];
        if (!pl::Matches (p, cat.filter))
            continue;
        const pl::PlantFit fit = FitForPlantsTab (state, p);
        if (cat.zone >= 0 && fit.status == pl::PlantFit::Status::None)
            continue; // a bed is chosen: only plants with compatible area in it
        ++shown;
        ImGui::TableNextRow ();
        ImGui::TableNextColumn ();
        ImGui::PushID (int (i));
        if (ImGui::Selectable (p.DisplayName ().c_str (), cat.selected == int (i), ImGuiSelectableFlags_SpanAllColumns))
            cat.selected = cat.selected == int (i) ? -1 : int (i);
        if (ImGui::IsItemHovered () && !p.localNames.empty ()) {
            std::string names;
            for (const auto& [lang, name] : p.localNames)
                names += lang + ": " + name + "\n";
            ImGui::SetTooltip ("%s", names.c_str ());
        }
        ImGui::PopID ();
        ImGui::TableNextColumn ();
        ImGui::TextUnformatted (LightLetters (p).c_str ());
        ImGui::TableNextColumn ();
        if (fit.layer >= 0)
            ImGui::Text ("%.2f m", fit.layerHeight);
        else
            ImGui::TextDisabled ("%.2f m", fit.wantedHeight);
        ImGui::TableNextColumn ();
        if (fit.status == pl::PlantFit::Status::Unknown) {
            ImGui::TextDisabled ("unknown");
            hudshell::ItemTip ("%s", fit.reason.c_str ());
        }
        else {
            ImGui::TextUnformatted (FitText (fit).c_str ());
            if (std::abs (fit.layerHeight - fit.wantedHeight) > 0.01f)
                hudshell::ItemTip ("Read at the %.2f m layer; this plant's light is read at %.2f m. Trace that height "
                                   "for its own figure.",
                                   fit.layerHeight, fit.wantedHeight);
        }
    }
    if (shown == 0) {
        ImGui::TableNextRow ();
        ImGui::TableNextColumn ();
        ImGui::TextDisabled (cat.plants.plants.empty () ? "no catalogue loaded" : "no plant matches");
    }
    ImGui::EndTable ();
}

void SelectedPlant (HudState& state)
{
    pl::PlantingHud& hud = state.planting;
    pl::CatalogueState& cat = hud.catalogue;
    if (cat.selected < 0 || size_t (cat.selected) >= cat.plants.plants.size ()) {
        hud.highlightMask = 0;
        return;
    }
    const pl::Plant& p = cat.plants.plants[size_t (cat.selected)];
    const pl::PlantFit fit = FitForPlantsTab (state, p);
    ImGui::SeparatorText (p.DisplayName ().c_str ());
    ImGui::Text ("Section: %s   Habit: %s", pl::SectionName (p.section), pl::HabitName (p.habit));
    ImGui::Text ("Light: %s", LightLetters (p).c_str ());
    ImGui::Text ("Mature height: %s   spread: %s", RangeText (p.matureHeightM).c_str (),
                 RangeText (p.matureSpreadM).c_str ());
    if (p.saltTolerance != pl::SaltTolerance::Unknown)
        ImGui::Text ("Salt tolerance: %s", pl::SaltToleranceName (p.saltTolerance));
    if (p.growthIntensity != pl::GrowthIntensity::Unknown)
        ImGui::Text ("Growth: %s", pl::GrowthIntensityName (p.growthIntensity));
    if (!p.moistureClasses.empty ())
        ImGui::Text ("Water: %s", MoistureText (p).c_str ());
    if (!p.plantingLayers.empty ())
        ImGui::Text ("Layer in a bed: %s", LayerText (p).c_str ());
    std::string marks;
    if (p.breeam)
        marks += "BREEAM";
    if (p.ornamentalFlowering)
        marks += std::string (marks.empty () ? "" : ", ") + "ornamental flowering";
    if (p.ornithological)
        marks += std::string (marks.empty () ? "" : ", ") + "for birds";
    if (!marks.empty ())
        ImGui::Text ("Marks: %s", marks.c_str ());
    if (!p.soilTypes.empty ()) {
        std::string soils;
        for (const auto& s : p.soilTypes)
            soils += (soils.empty () ? "" : ", ") + s;
        ImGui::Text ("Ground types: %s", soils.c_str ());
    }
    if (!SitesText (p).empty ())
        ImGui::TextWrapped ("Sites: %s", SitesText (p).c_str ());
    if (!p.floweringMonths.empty ()) {
        std::string months;
        for (int m : p.floweringMonths)
            months += (months.empty () ? "" : ", ") + std::to_string (m);
        ImGui::Text ("Flowering months: %s", months.c_str ());
    }
    if (!p.traits.AnyGiven ())
        ImGui::TextDisabled ("Traits: none filled yet (ecology, competition, roots, canopy, ...).");
    else if (ImGui::TreeNode ("Traits##plantcat")) {
        for (int t = 0; t < pl::kTraitCount; ++t)
            if (p.traits.values[size_t (t)].Given ())
                ImGui::Text ("%s: %s", pl::TraitPath (pl::Trait (t)).c_str (),
                             pl::TraitCsvCell (p.traits, pl::Trait (t)).c_str ());
        ImGui::TreePop ();
    }
    if (fit.status == pl::PlantFit::Status::Unknown)
        ImGui::TextDisabled ("Fit unknown: %s.", fit.reason.c_str ());
    else
        ImGui::Text ("Compatible: %s of %.1f m² measured at %.2f m%s.", FitText (fit).c_str (), fit.evaluatedArea,
                     fit.layerHeight,
                     cat.zone >= 0 ? (", in " + hud.zones.zones[size_t (cat.zone)].name).c_str () : "");
    // Every bed at once, on the plant's own layer.
    if (const auto* rows = ZoneRowsOfLayer (state, fit.layer)) {
        const pl::PlantFit layerFit = pl::FitOf (hud, p);
        std::string beds;
        for (size_t z = 0; z < hud.zones.zones.size (); ++z) {
            if (hud.zones.zones[z].role != pl::ZoneRole::Bed)
                continue;
            const pl::PlantFit in = pl::FitInZone (hud, p, layerFit, (*rows)[z]);
            beds += (beds.empty () ? "" : "; ") + hud.zones.zones[z].name + " " + FitText (in);
        }
        if (!beds.empty ())
            ImGui::TextWrapped ("In beds: %s.", beds.c_str ());
    }
    {
        const int zone = hud.climateState.designZone;
        const std::string& range = p.traits[pl::Trait::Hardiness].text;
        if (zone >= 0) {
            const pl::Hardiness h = pl::HardinessAt (range, zone);
            const std::string z = pl::UsdaName (zone);
            if (h == pl::Hardiness::TooCold)
                ImGui::TextColored (ImVec4 (0.95f, 0.45f, 0.40f, 1.0f), "Not hardy in USDA %s (rated %s).", z.c_str (),
                                    range.c_str ());
            else if (h == pl::Hardiness::Unknown)
                ImGui::TextDisabled ("Hardiness in USDA %s: not known (no USDA or RHS rating).", z.c_str ());
            else
                ImGui::Text ("Hardy in USDA %s (rated %s).", z.c_str (), range.c_str ());
        }
    }
    if (const auto quantity = pl::PlantQuantity (p, fit))
        ImGui::Text ("Quantity: %.0f to %.0f plants at %g to %g per m² over the compatible area.", quantity->min,
                     quantity->max, p.densityPerM2->min, p.densityPerM2->max);
    else if (fit.status == pl::PlantFit::Status::Fits)
        ImGui::TextDisabled ("Quantity: no planting density in the catalogue.");
    hudshell::ItemTip ("Compatible area x the catalogue's plants per m², rounded up. An estimate for the whole "
                       "compatible area, not a planting plan: spacing and packing are not checked.");
    const bool canHighlight = fit.layer >= 0 && p.HasLight ();
    ImGui::BeginDisabled (!canHighlight);
    ImGui::Checkbox ("Highlight where it fits##plantcat", &cat.highlight);
    ImGui::EndDisabled ();
    if (canHighlight)
        hudshell::ItemTip ("Hatch the light classes this plant accepts and dim the others, in the viewer and the "
                           "legend.");
    else if (!p.HasLight ())
        hudshell::ItemTip ("Not available: the catalogue gives this plant no light classes, so where it fits "
                           "is not known.");
    else
        hudshell::ItemTip ("Not available: %s. Run a layer on the Light tab first.", fit.reason.c_str ());
    if (fit.layer >= 0 && fit.layer != hud.shownLayer) {
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Show its layer##plantcat"))
            ShowLayer (state, size_t (fit.layer));
    }
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Add to scheme##plantcat"))
        AddToScheme (hud, p);
    hudshell::ItemTip ("Add this plant to the Scheme tab's mix, to be placed on a measured layer.");
    hud.highlightMask = cat.highlight && canHighlight ? pl::AcceptedClassMask (hud, p) : 0;
}

} // namespace

void DrawPlantsTab (HudState& state)
{
    EnsureCatalogue (state);
    RefreshLayerAreas (state);
    pl::CatalogueState& cat = state.planting.catalogue;
    CatalogueRow (state);
    Filters (cat, state.planting.climateState.designZone, state.planting.zones.zones);
    PlantTable (state);
    SelectedPlant (state);
    ImGui::TextDisabled ("Fits here is by light. Room for a grown crown: the Fit tab. Placing plants: Scheme.");
}

} // namespace geomsrv::archviz::plantinghud
