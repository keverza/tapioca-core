// ArchViz/OverlayHudOwn -- the HUD's own pages: Stats, Selection and Debug, the engine's
// (OverlayHudEngine.hpp). What they show comes from the view's owner (OverlayHud.hpp
// `OwnPages`); they draw it in every HUD's cards (HudShell.hpp), and Stats adds the callers'
// panels that asked to be cards on it.

#include "ArchViz/OverlayHudEngine.hpp"

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
    hudshell::Cards (own.stats, *look, ui);
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
}

void Engine::Impl::MassingPage ()
{
    for (const auto& request : hudmassing::Draw (own.massing)) {
        store->massingRequests.push_back (request);
        changes.push_back ({ "massing", std::string (), "Massing", hudmassing::Role (request.group), -1,
                             double (request.action), hudmassing::Label (request.group), true });
    }
    for (auto& edit : hudmassingrules::Draw (own.massing.rules, store->massingRules, own.massing.calculationBusy,
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
    if (ImGui::CollapsingHeader ("Story slice heights", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled ("Set floor-to-floor height and first story in Selection.");
        ImGui::TextDisabled ("Pick floors; right-click to set their function.");
        const hudsection::Run before = store->floors;
        for (auto& edit : hudsection::Diagram (own.section, store->floors, *look, overlayhud::FontScaleOf (*store))) {
            changes.push_back ({ "metadata", {}, "Massing", edit.id, -1, edit.number, edit.text, true });
            store->metadataEdits.push_back (std::move (edit));
        }
        if (store->floors != before) {
            const auto& run = store->floors;
            changes.push_back (
                { "floors",
                  {},
                  "Massing",
                  own.section.key,
                  -1,
                  double (run.first),
                  run.Empty () ? std::string () : std::to_string (run.first) + "-" + std::to_string (run.last),
                  true });
        }
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
