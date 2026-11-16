// OVERLAY-INVARIANTS.md: ImGui layout/input only; emits events after layout's lock.
#include "OverlayHudEngine.hpp"
#include <set>

namespace geomsrv::archviz::overlayhud {
void Engine::Impl::Criteria (const layers::Panel& panel, const layers::PanelItem& item, size_t index, PanelState& state,
                             float width)
{
    std::set<std::string> current;
    for (const auto& entry : panel.items)
        for (const auto& row : entry.criteria)
            current.insert (row.id);
    for (auto it = state.criteriaText.begin (); it != state.criteriaText.end ();)
        if (!current.contains (it->first))
            it = state.criteriaText.erase (it);
        else
            ++it;
    std::vector<CriteriaEdit> edits;
    bool textActive = false;
    DrawCriteriaTable (item, state.criteriaText, width, edits, textActive);
    if (textActive)
        store->criteriaTextActive = true;
    for (const auto& edit : edits)
        changes.push_back ({ edit.kind, key, panel.title, edit.rowId, int32_t (index), edit.value, edit.text, true });
}
bool CriteriaTextActive (const State& state)
{
    return state.criteriaTextActive && state.shown;
}
} // namespace geomsrv::archviz::overlayhud
