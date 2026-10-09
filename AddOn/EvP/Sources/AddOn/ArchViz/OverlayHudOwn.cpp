// ArchViz/OverlayHudOwn -- the HUD's own pages: Stats, Selection and Debug, the engine's
// (OverlayHudEngine.hpp). What they show comes from the view's owner (OverlayHud.hpp
// `OwnPages`); they draw it in every HUD's cards (HudShell.hpp), and Stats adds the callers'
// panels that asked to be cards on it.

#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/HudMassingStats.hpp"

#include <string>
#include <algorithm>

namespace geomsrv {
namespace archviz {
namespace overlayhud {

void Engine::SetOwnPages (OwnPages pages)
{
    impl_->store->floorPlanSnapshots.clear ();
    if (pages.floorPlansKnown)
        for (const auto& plan : pages.floorPlans)
            impl_->store->floorPlanSnapshots[plan.key] = plan;
    else
        for (const auto& preview : pages.buildings)
            impl_->store->floorPlanSnapshots[preview.building.key] = preview.plan;
    if (!pages.buildings.empty () || pages.selection.count == 0) {
        const auto has = [&] (const std::string& key) {
            return std::any_of (pages.buildings.begin (), pages.buildings.end (),
                                [&] (const auto& preview) { return preview.building.key == key; });
        };
        if (!has (impl_->store->highlightedBuilding))
            impl_->store->highlightedBuilding.clear ();
        if (!has (impl_->store->pickedFloorBuilding)) {
            impl_->store->pickedFloorBuilding.clear ();
            impl_->store->floors = {};
        }
        for (auto it = impl_->store->buildingPlans.begin (); it != impl_->store->buildingPlans.end ();)
            if (pages.floorPlansKnown ? !impl_->store->floorPlanSnapshots.contains (it->first) : !has (it->first))
                it = impl_->store->buildingPlans.erase (it);
            else
                ++it;
    }
    impl_->own = std::move (pages);
}

bool Engine::Standalone () const
{
    return impl_->own.standalone;
}

std::string Engine::Impl::TitleOf (const std::string& tabKey, const std::vector<const layers::Panel*>& panels,
                                   const std::vector<std::string>& keys) const
{
    if (tabKey == kStatsKey)
        return "Stats";
    if (tabKey == kSelectionKey)
        return "Selection";
    if (tabKey == hudmassing::kTabKey)
        return "Massing";
    if (tabKey == kDebugKey)
        return "Debug";
    for (size_t i = 0; i < panels.size () && i < keys.size (); ++i)
        if (keys[i] == tabKey)
            return panels[i]->title;
    return "Settings";
}

void Engine::Impl::StatsPage (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys,
                              float ui)
{
    if (own.massingStats)
        nextStatsHover = hudmassingstats::Draw (*own.massingStats, *look, ui);
    const auto figureHover = hudshell::Cards (own.stats, *look, ui);
    if (!figureHover.empty ())
        nextStatsHover = figureHover;
    // ⚠️ A CALLER'S CARD IS ITS PANEL, UNDER ITS TITLE: its items, its controls and the values
    // the user set in them, kept by its key as on a tab of its own.
    for (const size_t i : statsCards) {
        const layers::Panel& panel = *panels[i];
        ImGui::PushID (keys[i].c_str ());
        if (!panel.title.empty ())
            ImGui::SeparatorText (panel.title.c_str ());
        key = keys[i];
        layer = keys[i].substr (0, keys[i].rfind ('#'));
        Items (panel, StateOf (keys[i], panel), ui);
        ImGui::PopID ();
    }
    if (own.stats.empty () && statsCards.empty ())
        ImGui::TextDisabled ("Nothing to report yet");
}

void Engine::Impl::SelectionPage (float ui)
{
    hudshell::SelectionList (own.selection, *look, ui);
    // ⚠️ SAID, NOT WRITTEN: the owner writes an edit from the message loop, in one undo step,
    // and lays the HUD out again with what the elements then hold (SelectionMetadata.hpp).
    if (own.selection.count == 0)
        return;
    std::vector<hudmeta::Edit> edits;
    auto metadata = own.metadata;
    if (!own.buildings.empty ())
        metadata.fields.erase (std::remove_if (metadata.fields.begin (), metadata.fields.end (),
                                               [] (const auto& field) {
                                                   return field.id == "massing.story" ||
                                                          field.id == "massing.function" ||
                                                          field.id == "massing.floorHeight";
                                               }),
                               metadata.fields.end ());
    for (hudmeta::Edit& edit : hudmeta::Editor (metadata, *look, ui))
        edits.push_back (std::move (edit));
    for (hudmeta::Edit& edit : edits) {
        changes.push_back ({ "metadata", std::string (), "Selection", edit.id, -1,
                             edit.kind == hudmeta::FieldKind::Toggle ? (edit.on ? 1.0 : 0.0) : edit.number, edit.text,
                             true });
        store->metadataEdits.push_back (std::move (edit));
    }
    if (ImGui::CollapsingHeader ("Story slice editor", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!own.buildings.empty ()) {
            for (const auto& preview : own.buildings)
                BuildingDiagram (preview, ui);
            return;
        }
        ImGui::TextDisabled ("Story count is calculated from the slices.");
        for (const auto& page : own.storyHeights) {
            ImGui::PushID (page.element.c_str ());
            for (auto& edit : hudmeta::Editor (page, *look, ui)) {
                changes.push_back ({ "metadata", {}, "Selection", edit.id, -1, edit.number, edit.text, true });
                store->metadataEdits.push_back (std::move (edit));
            }
            ImGui::PopID ();
        }
        ImGui::TextDisabled ("Pick floors; right-click to set their function.");
        const hudsection::Run before = store->floors;
        for (auto& edit : hudsection::Diagram (own.section, store->floors, *look, ui, store->massingCoefficients)) {
            changes.push_back ({ "metadata", {}, "Selection", edit.id, -1, edit.number, edit.text, true });
            store->metadataEdits.push_back (std::move (edit));
        }
        if (store->floors != before) {
            const auto& run = store->floors;
            changes.push_back (
                { "floors",
                  {},
                  "Selection",
                  own.section.key,
                  -1,
                  double (run.first),
                  run.Empty () ? std::string () : std::to_string (run.first) + "-" + std::to_string (run.last),
                  true });
        }
    }
}

bool MassingDimensions (const State& state)
{
    return state.massingRules.offsetDimensions;
}

massingareas::Coefficients MassingCoefficients (const State& state)
{
    return state.massingCoefficients;
}

bool SetMassingCoefficients (State& state, const massingareas::Coefficients& coefficients)
{
    if (!massingareas::Valid (coefficients) || massingareas::Same (coefficients, state.massingCoefficients))
        return false;
    state.massingCoefficients = coefficients;
    ++state.revision;
    return true;
}

std::vector<massingareas::NumberEdit> TakeMassingCoefficientNumbers (State& state)
{
    auto numbers = std::move (state.massingCoefficientNumbers);
    state.massingCoefficientNumbers.clear ();
    return numbers;
}

bool AnswerMassingCoefficientNumber (State& state, const massingareas::NumberEdit& edit, double number)
{
    auto wanted = state.massingCoefficients;
    if (!massingareas::Same (edit.before, wanted) || !massingareas::Assign (wanted, edit.key, number))
        return false;
    SetMassingCoefficients (state, wanted);
    return true;
}

void Engine::Impl::MassingPage ()
{
    for (const auto& request : hudmassing::Draw (own.massing)) {
        store->massingRequests.push_back (request);
        changes.push_back ({ "massing", std::string (), "Massing", hudmassing::Role (request.group), -1,
                             double (request.action), hudmassing::Label (request.group), true });
    }
    const bool dimensions = store->massingRules.offsetDimensions;
    const auto parcels = own.massing.parcels.empty () && !own.massing.rules.guid.empty ()
                             ? std::vector<massingrules::Page> { own.massing.rules }
                             : own.massing.parcels;
    for (auto& edit :
         hudmassingrules::DrawSite (parcels, store->massingRules, store->massingSite, own.massing.calculationBusy,
                                    own.massing.calculationNote, own.massing.preview)) {
        changes.push_back (
            { "massingRules", std::string (), "Massing", edit.before.guid, -1, 1.0, "Save assignments", true });
        store->massingRuleEdits.push_back (std::move (edit));
    }
    for (auto& request : store->massingRules.calculations) {
        changes.push_back ({ "massingCalculation", {}, "Massing", "calculate", -1, double (request.action), {}, true });
        store->massingCalculations.push_back (std::move (request));
    }
    store->massingRules.calculations.clear ();
    if (!store->massingRules.numbers.empty ())
        changes.push_back ({ "massingNumber", {}, "Massing", "set", -1, 0, {}, true });
    auto coefficients = store->massingCoefficients;
    if (hudmassingstats::CoefficientInputs (coefficients, store->massingCoefficientNumbers) &&
        SetMassingCoefficients (*store, coefficients))
        changes.push_back ({ "massingCoefficients", {}, "Massing", "area calculations", -1, 1, {}, true });
    if (!store->massingCoefficientNumbers.empty ())
        changes.push_back ({ "massingCoefficientNumber", {}, "Massing", "set", -1, 0, {}, true });
    if (!ImGui::CollapsingHeader ("Preview and Bake options", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::Checkbox ("Show segment and endpoint labels", &store->massingRules.labels);
    ImGui::Checkbox ("Show offset dimensions in overlay", &store->massingRules.offsetDimensions);
    if (dimensions != store->massingRules.offsetDimensions)
        changes.push_back ({ "massingDimensions",
                             {},
                             "Massing",
                             "offset dimensions",
                             -1,
                             store->massingRules.offsetDimensions ? 1.0 : 0.0,
                             {},
                             true });
    const Displays& shown = store->displaysPending ? store->displays : own.displays;
    Displays wanted = shown;
    const auto bake = [this] (massingbake::Kind kind) {
        ImGui::SameLine ();
        ImGui::PushID (int (kind));
        if (ImGui::Button ("Bake...")) {
            store->massingBakes.push_back (kind);
            changes.push_back ({ "massingBake", {}, "Massing", "settings", -1, double (kind), {}, true });
        }
        ImGui::PopID ();
    };
    if (ImGui::Checkbox ("Show story slices", &wanted.massingSlicesOn)) {
        store->displays = wanted;
        store->displaysPending = true;
        ShowLayer (massingslices::kLayer, true);
        changes.push_back ({ "display", {}, "Massing", "story slices", -1, 1, {}, true });
    }
    bake (massingbake::Kind::Slices);
    if (ImGui::Checkbox ("Show slice area text when zoomed in", &wanted.slices.label)) {
        store->displays = wanted;
        store->displaysPending = true;
        changes.push_back ({ "display", {}, "Massing", "slice area text", -1, 1, {}, true });
    }
    if (ImGui::Checkbox ("Show building collapse zone", &store->massingCollapseZone)) {
        ShowLayer ("tapioca.massing.collapseZone", true);
        ShowLayer ("tapioca.massing.collapseZone.terrain", true);
        changes.push_back ({ "massingCollapseZone",
                             {},
                             "Massing",
                             "collapse zone",
                             -1,
                             store->massingCollapseZone ? 1.0 : 0.0,
                             {},
                             true });
    }
    bake (massingbake::Kind::Collapse);
    bool envelope = LayerShown (*store, massingcalculation::kEnvelopeLayer);
    if (ImGui::Checkbox ("Show massing envelope", &envelope))
        ShowLayer (massingcalculation::kEnvelopeLayer, envelope);
    bake (massingbake::Kind::Envelope);
    bool lines = LayerShown (*store, "tapioca.massing.lines");
    if (ImGui::Checkbox ("Show massing lines", &lines))
        ShowLayer ("tapioca.massing.lines", lines);
    if (ImGui::Checkbox ("Show unique buildings", &store->uniqueBuildings))
        changes.push_back (
            { "uniqueBuildings", {}, "Massing", "building IDs", -1, store->uniqueBuildings ? 1.0 : 0.0, {}, true });
    if (ImGui::Checkbox ("Mark larger than 500m2", &store->markLargeFloors))
        changes.push_back (
            { "markLargeFloors", {}, "Massing", "gross > 500 m2", -1, store->markLargeFloors ? 1.0 : 0.0, {}, true });
    if (ImGui::Checkbox ("Highlight headroom below 1.6m", &store->showLowHeadroom))
        changes.push_back (
            { "lowHeadroom", {}, "Massing", "headroom < 1.6 m", -1, store->showLowHeadroom ? 1.0 : 0.0, {}, true });
    const bool stairsChanged = ImGui::Checkbox ("Show proposed stair cores (all floors)", &store->previewStairs);
    const bool unitsChanged = ImGui::Checkbox ("Show unit outlines (all floors)", &store->previewUnits);
    if (stairsChanged || unitsChanged) {
        ShowLayer (buildingplan::kStairsLayer, true);
        ShowLayer (buildingplan::kUnitsLayer, true);
        changes.push_back ({ "massingPlanPreview", {}, "Massing", "floor scheme overlays", -1, 1, {}, true });
    }
    if (store->markLargeFloors)
        ImGui::TextDisabled ("Orange: combined building floor gross area > 500 m2 (shared gross factor).");
    if (store->showLowHeadroom)
        ImGui::TextDisabled ("Hatched gray: excluded from area totals, even when highlight is off.");
    if (store->uniqueBuildings) {
        ImGui::TextDisabled ("Same building ID = same colour. Missing IDs stay separate.");
    }
    if ((store->uniqueBuildings || store->markLargeFloors || store->showLowHeadroom) &&
        !own.massing.inspectionNote.empty ())
        ImGui::TextWrapped ("%s", own.massing.inspectionNote.c_str ());
    if (store->massingCollapseZone) {
        ImGui::TextDisabled ("Red hatches: 0.3333 x slab top height above topography, unioned.");
        if (!own.massing.collapseNote.empty ())
            ImGui::TextWrapped ("%s", own.massing.collapseNote.c_str ());
    }
}

void Engine::Impl::DebugPage (float ui)
{
    // ⚠️ THE CONSOLE FIRST (the user, 2026-10-03: what to check when something is failing).
    if (hudconsole::Draw (own.console, *look, store->consoleSeen, store->consoleCleared)) {
        store->logsRequested = true;
        changes.push_back ({ "logs", {}, "Debug", "open folder", -1, 1, {}, true });
    }
    hudshell::Cards (own.debug, *look, ui);
    // What the HUD itself costs: the owner cannot see it, the engine measures it.
    hudshell::Card hud;
    hud.title = "HUD";
    const auto counts = debugCounters.Observe (huddebug::Now (), { stats.builds, stats.frames });
    hud.figures.push_back ({ "Last layout", std::to_string (stats.lastMilliseconds) + " ms" });
    hud.figures.push_back ({ "Layouts (60 s)", std::to_string (counts[0]) });
    hud.figures.push_back ({ "ImGui frames (60 s)", std::to_string (counts[1]) });
    hud.figures.push_back ({ "Atlas versions", std::to_string (stats.atlasVersions) });
    hud.note = "Activity counts reset every 60 seconds. Lifetime telemetry is unchanged.";
    hudshell::Cards ({ hud }, *look, ui);
}

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv
