// ArchViz/OverlayHudOwn -- the HUD's own pages: Stats, Selection and Debug, the engine's
// (OverlayHudEngine.hpp). What they show comes from the view's owner (OverlayHud.hpp
// `OwnPages`); they draw it in every HUD's cards (HudShell.hpp), and Stats adds the callers'
// panels that asked to be cards on it.

#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/HudMassingStats.hpp"

#include <string>

namespace geomsrv {
namespace archviz {
namespace overlayhud {

void Engine::SetOwnPages (OwnPages pages)
{
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
    for (hudmeta::Edit& edit : hudmeta::Editor (own.metadata, *look, ui))
        edits.push_back (std::move (edit));
    for (hudmeta::Edit& edit : edits) {
        changes.push_back ({ "metadata", std::string (), "Selection", edit.id, -1,
                             edit.kind == hudmeta::FieldKind::Toggle ? (edit.on ? 1.0 : 0.0) : edit.number, edit.text,
                             true });
        store->metadataEdits.push_back (std::move (edit));
    }
    if (ImGui::CollapsingHeader ("Story slice editor", ImGuiTreeNodeFlags_DefaultOpen)) {
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
    if (dimensions != store->massingRules.offsetDimensions)
        changes.push_back ({ "massingDimensions",
                             {},
                             "Massing",
                             "offset dimensions",
                             -1,
                             store->massingRules.offsetDimensions ? 1.0 : 0.0,
                             {},
                             true });
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
    const Displays& shown = store->displaysPending ? store->displays : own.displays;
    Displays wanted = shown;
    if (ImGui::Checkbox ("Show slice area text when zoomed in", &wanted.slices.label)) {
        store->displays = wanted;
        store->displaysPending = true;
        changes.push_back ({ "display", {}, "Massing", "slice area text", -1, 1, {}, true });
    }
    if (ImGui::Checkbox ("Show building collapse zone", &store->massingCollapseZone))
        changes.push_back ({ "massingCollapseZone",
                             {},
                             "Massing",
                             "collapse zone",
                             -1,
                             store->massingCollapseZone ? 1.0 : 0.0,
                             {},
                             true });
    bool envelope = LayerShown (*store, massingcalculation::kEnvelopeLayer);
    if (ImGui::Checkbox ("Show massing envelope", &envelope))
        ShowLayer (massingcalculation::kEnvelopeLayer, envelope);
    if (store->massingCollapseZone) {
        ImGui::TextDisabled ("Red hatches: 0.3333 x local vertical building height, unioned.");
        if (!own.massing.collapseNote.empty ())
            ImGui::TextWrapped ("%s", own.massing.collapseNote.c_str ());
    }
}

void Engine::Impl::DebugPage (float ui)
{
    // ⚠️ THE CONSOLE FIRST (the user, 2026-10-03: what to check when something is failing).
    hudconsole::Draw (own.console, *look, store->consoleSeen, store->consoleCleared);
    hudshell::Cards (own.debug, *look, ui);
    // What the HUD itself costs: the owner cannot see it, the engine measures it.
    hudshell::Card hud;
    hud.title = "HUD";
    hud.figures.push_back ({ "Last layout", std::to_string (stats.lastMilliseconds) + " ms" });
    hud.figures.push_back ({ "Layouts", std::to_string (stats.builds) });
    hud.figures.push_back ({ "Atlas versions", std::to_string (stats.atlasVersions) });
    hudshell::Cards ({ hud }, *look, ui);
}

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv
