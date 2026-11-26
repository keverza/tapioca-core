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
    pl::ReadCatalogueFolder (folder, cat);
    for (const auto& name : created)
        cat.status += " Created " + name + ".";
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
    char text[48];
    std::snprintf (text, sizeof (text), "%.2f to %.2f m", r->min, r->max);
    return text;
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
        ImGui::OpenPopup ("Restore the example catalogue?##plantcat");
    hudshell::ItemTip ("Replace both files with the shipped example, after copying them to a dated backup.");
    if (ImGui::BeginPopup ("Restore the example catalogue?##plantcat")) {
        ImGui::TextUnformatted ("plants.json and profiles.json are copied to a dated backup,\nthen replaced with the "
                                "shipped example.");
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
    if (!cat.problems.empty () && ImGui::TreeNodeEx ("Problems##plantcat", ImGuiTreeNodeFlags_DefaultOpen,
                                                     "%d problem(s)", int (cat.problems.size ()))) {
        ImGui::PushStyleColor (ImGuiCol_Text, ImVec4 (0.95f, 0.45f, 0.40f, 1.0f));
        for (const auto& p : cat.problems)
            ImGui::TextWrapped ("%s", p.c_str ());
        ImGui::PopStyleColor ();
        ImGui::TreePop ();
    }
    for (const auto& source : cat.plants.sources)
        if (!source.note.empty ())
            ImGui::TextDisabled ("%s: %s", source.title.empty () ? source.id.c_str () : source.title.c_str (),
                                 source.note.c_str ());
}

void Filters (pl::CatalogueState& cat)
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
        ++shown;
        const pl::PlantFit fit = pl::FitOf (hud, p);
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
    const pl::PlantFit fit = pl::FitOf (hud, p);
    ImGui::SeparatorText (p.DisplayName ().c_str ());
    ImGui::Text ("Habit: %s   Light: %s", pl::HabitName (p.habit), LightLetters (p).c_str ());
    ImGui::Text ("Mature height: %s   spread: %s", RangeText (p.matureHeightM).c_str (),
                 RangeText (p.matureSpreadM).c_str ());
    if (!p.floweringMonths.empty ()) {
        std::string months;
        for (int m : p.floweringMonths)
            months += (months.empty () ? "" : ", ") + std::to_string (m);
        ImGui::Text ("Flowering months: %s", months.c_str ());
    }
    for (const auto& ref : p.sources)
        ImGui::TextDisabled ("Source: %s%s", ref.sourceId.c_str (),
                             ref.page ? (", p. " + std::to_string (ref.page)).c_str () : "");
    if (fit.status == pl::PlantFit::Status::Unknown)
        ImGui::TextDisabled ("Fit unknown: %s.", fit.reason.c_str ());
    else
        ImGui::Text ("Compatible: %s of %.1f m² measured at %.2f m.", FitText (fit).c_str (), fit.evaluatedArea,
                     fit.layerHeight);
    if (const auto quantity = pl::PlantQuantity (p, fit))
        ImGui::Text ("Quantity: %.0f to %.0f plants at %g to %g per m² over the compatible area.", quantity->min,
                     quantity->max, p.densityPerM2->min, p.densityPerM2->max);
    else if (fit.status == pl::PlantFit::Status::Fits)
        ImGui::TextDisabled ("Quantity: no planting density in the catalogue.");
    hudshell::ItemTip ("Compatible area x the catalogue's plants per m², rounded up. An estimate for the whole "
                       "compatible area, not a planting plan: spacing and packing are not checked.");
    ImGui::BeginDisabled (fit.layer < 0);
    ImGui::Checkbox ("Highlight where it fits##plantcat", &cat.highlight);
    ImGui::EndDisabled ();
    hudshell::ItemTip ("Dim the light classes this plant does not accept, in the viewer and the legend.");
    if (fit.layer >= 0 && fit.layer != hud.shownLayer) {
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Show its layer##plantcat"))
            ShowLayer (state, size_t (fit.layer));
    }
    hud.highlightMask = cat.highlight && fit.layer >= 0 ? pl::AcceptedClassMask (hud, p) : 0;
}

} // namespace

void DrawPlantsTab (HudState& state)
{
    EnsureCatalogue (state);
    RefreshLayerAreas (state);
    pl::CatalogueState& cat = state.planting.catalogue;
    CatalogueRow (state);
    Filters (cat);
    PlantTable (state);
    SelectedPlant (state);
    ImGui::Separator ();
    ImGui::BeginDisabled ();
    int fitMode = 0;
    ImGui::RadioButton ("Light##plantingfit", &fitMode, 0);
    ImGui::SameLine ();
    ImGui::RadioButton ("Space##plantingfit", &fitMode, 1);
    ImGui::SameLine ();
    ImGui::RadioButton ("Combined##plantingfit", &fitMode, 2);
    ImGui::EndDisabled ();
    hudshell::ItemTip ("Later (Stage 2): where each plant fits by space for its mature crown, or by both.");
    LaterButton ("Show mature envelope", "Later: the selected plant's mature height and spread at the point.");
}

} // namespace geomsrv::archviz::plantinghud
