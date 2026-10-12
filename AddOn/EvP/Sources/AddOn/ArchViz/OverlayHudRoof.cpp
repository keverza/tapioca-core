#include "ArchViz/OverlayHudEngine.hpp"

namespace geomsrv::archviz::overlayhud {
void Engine::Impl::RoofFonts ()
{
    if (store->roofEditor)
        FontFor ("tapioca:roof-heading");
}
void Engine::Impl::RoofEditActions (const Input& input)
{
    if (!store->roofEditor)
        return;
    auto& session = *store->roofEditor;
    for (const auto action : input.editActions) {
        if (action == Input::EditAction::BlockedModelKey) {
            session.editor.message =
                "Native delete/cut/paste is disabled on the locked roof canvas. Use overlay context actions.";
        }
        else if (action == Input::EditAction::Cancel) {
            if (session.editor.gesture)
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
    if ((!state.editLocked || !state.shown || !LayerShown (state, roofslope::kLayer)) && s.editor.gesture)
        s.editor.Cancel (), s.drag = 0;
    if (s.editor.published == s.editor.revision)
        return false;
    layer = roofslope::Layer (s.editor.data);
    s.editor.published = s.editor.revision;
    return true;
}
std::optional<roofhud::NumberEdit> TakeRoofNumber (State& state)
{
    if (!state.roofEditor)
        return {};
    auto out = std::move (state.roofEditor->number);
    state.roofEditor->number.reset ();
    return out;
}
bool AnswerRoofNumber (State& state, const roofhud::NumberEdit& edit, double value)
{
    return state.roofEditor && roofhud::Answer (*state.roofEditor, edit, value);
}
} // namespace geomsrv::archviz::overlayhud
