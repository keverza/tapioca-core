#include "ArchViz/OverlayHudEngine.hpp"

namespace geomsrv::archviz::overlayhud {
void Engine::Impl::RoofFonts ()
{
    if (store->roofEditor)
        FontFor ("tapioca:roof-inter");
}
void Engine::Impl::RoofEditActions (const Input& input)
{
    if (!store->roofEditor || !store->editLocked || !store->shown || !LayerShown (*store, roofslope::kLayer))
        return;
    auto& session = *store->roofEditor;
    auto actions = input.editActions;
    if (!session.number && !popup && !ImGui::GetIO ().WantTextInput)
        for (const auto& event : input.keys)
            if (event.down && !event.repeat) {
                if (event.code == 0x2E || event.code == 0x08)
                    roofhud::DeleteSelection (session);
                else if (event.code == 0x1B)
                    actions.push_back (Input::EditAction::Cancel);
                else if (event.ctrl && (event.code == 'Z' || event.code == 'Y'))
                    actions.push_back (event.code == 'Z' ? Input::EditAction::Undo : Input::EditAction::Redo);
            }
    for (const auto action : actions) {
        if (action == Input::EditAction::BlockedModelKey) {
            session.editor.message =
                "Native delete/cut/paste is disabled on the locked roof canvas. Use overlay context actions.";
        }
        else if (action == Input::EditAction::Cancel) {
            if (session.number)
                session.number.reset ();
            else if (session.editor.gesture)
                session.editor.Cancel (), session.drag = 0;
            else
                session.close = true;
        }
        else {
            session.editor.Undo (action == Input::EditAction::Undo);
            session.drag = 0;
        }
    }
}
bool StartRoofEditor (State& state, roofslope::Data data)
{
    StopRoofEditor (state);
    auto session = std::make_shared<roofhud::Session> ();
    session->token = state.revision + 1;
    session->previousLock = state.editLocked;
    session->editor.data = std::move (data);
    for (size_t i = 0; i < session->editor.data.cells.size (); ++i)
        if (session->editor.data.cells[i].drainInside) {
            session->editor.selectedCell = int (i);
            session->editor.selectedNode = session->editor.data.cells[i].drain;
            break;
        }
    session->editor.published = session->editor.revision;
    state.roofEditor = std::move (session);
    state.editLocked = true;
    state.shown = true;
    state.hidden.erase (roofslope::kLayer);
    state.host.open = true;
    state.host.selected = std::string (roofslope::kLayer) + "#0";
    ++state.revision;
    return true;
}
void StopRoofEditor (State& state)
{
    if (!state.roofEditor)
        return;
    state.editLocked = state.roofEditor->previousLock;
    state.roofEditor.reset ();
    ++state.revision;
}
bool RoofEditorActive (const State& state)
{
    return bool (state.roofEditor);
}
const roofslope::Data* RoofEditorData (const State& state)
{
    return state.roofEditor ? &state.roofEditor->editor.data : nullptr;
}
bool FollowRoofEditor (State& state, overlaylayers::Layer& layer, bool& clear)
{
    clear = false;
    if (!state.roofEditor)
        return false;
    bool exists = false;
    for (const auto& item : overlaylayers::Layers ())
        exists = exists || item->name == roofslope::kLayer;
    auto& s = *state.roofEditor;
    if (!exists || s.close) {
        clear = exists;
        StopRoofEditor (state);
        return clear;
    }
    // Hide, unlock or a view switch cannot leave an unfinished manipulation behind.
    if (!state.editLocked || !state.shown || !LayerShown (state, roofslope::kLayer)) {
        s.number.reset ();
        if (s.editor.gesture)
            s.editor.Cancel (), s.drag = 0;
    }
    if (s.editor.published == s.editor.revision)
        return false;
    layer = roofslope::Layer (s.editor.data);
    s.editor.published = s.editor.revision;
    return true;
}
} // namespace geomsrv::archviz::overlayhud
