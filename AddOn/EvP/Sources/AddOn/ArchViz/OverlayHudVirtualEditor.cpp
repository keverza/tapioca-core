#include "ArchViz/EditLayers.hpp"
#include "ArchViz/OverlayHudEngine.hpp"

// The virtual editor's lifecycle on the HUD's state, in the roof editor's pattern: a
// session borrows the dock's lock and owns only its command-scoped layer, and stopping
// it returns the lock and forgets everything (OVERLAY-INVARIANTS.md §8).
namespace geomsrv::archviz::overlayhud {

void Engine::Impl::VirtualEditActions (const Input& input)
{
    if (!store->virtualEditor || !store->editLocked || !store->shown || !LayerShown (*store, editlayers::kLayer))
        return;
    edithud::Keys (*store->virtualEditor, input, popup || ImGui::GetIO ().WantTextInput);
}

bool StartVirtualEditor (State& state, edit::Document doc, std::string role, edit::SnapContext context)
{
    StopRoofEditor (state); // one editor owns the locked view at a time
    StopVirtualEditor (state);
    auto session = std::make_shared<edithud::Session> (std::move (doc));
    session->token = state.revision + 1;
    session->previousLock = state.editLocked;
    session->role = std::move (role);
    session->context = std::move (context);
    for (const auto& e : session->edits.Doc ().entities)
        if (!e.vertices.empty ()) {
            session->workZ = e.vertices.front ().p.z;
            break;
        }
    state.virtualEditor = std::move (session);
    state.editLocked = true;
    state.shown = true;
    state.hidden.erase (editlayers::kLayer);
    state.host.open = true;
    state.host.selected = std::string (editlayers::kLayer) + "#0";
    ++state.revision;
    return true;
}

void StopVirtualEditor (State& state)
{
    if (!state.virtualEditor)
        return;
    state.editLocked = state.virtualEditor->previousLock;
    state.virtualEditor.reset ();
    ++state.revision;
}

bool VirtualEditorActive (const State& state)
{
    return bool (state.virtualEditor);
}

const edithud::Session* VirtualEditor (const State& state)
{
    return state.virtualEditor.get ();
}

edithud::Session* VirtualEditor (State& state)
{
    return state.virtualEditor.get ();
}

bool VirtualEditorOwnedBy (const State& state, const std::string& command)
{
    return state.virtualEditor && !command.empty () && state.virtualEditor->owner == command;
}

bool FollowVirtualEditor (State& state, overlaylayers::Layer& layer, bool& clear)
{
    clear = false;
    if (!state.virtualEditor)
        return false;
    bool exists = false;
    for (const auto& item : overlaylayers::Layers ())
        exists = exists || item->name == editlayers::kLayer;
    auto& s = *state.virtualEditor;
    if (!exists || s.close) {
        clear = exists;
        StopVirtualEditor (state);
        return clear;
    }
    // Hide, unlock or a view switch cannot leave an unfinished gesture behind.
    if (!state.editLocked || !state.shown || !LayerShown (state, editlayers::kLayer)) {
        if (s.drag != edithud::Drag::None)
            s.edits.Cancel (), s.drag = edithud::Drag::None;
        s.drawing.clear ();
    }
    return edithud::Follow (s, layer);
}

} // namespace geomsrv::archviz::overlayhud
