#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/EditLayers.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include <cmath>
#include <random>
#include <utility>

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

bool StartVirtualEditor (State& state, edit::Document doc, std::string role, edit::SnapContext context, double planeZ,
                         bool planeLocked)
{
    if (!std::isfinite (planeZ))
        return false;       // invalid input cannot replace an existing editor or release its ownership
    StopRoofEditor (state); // one editor owns the locked view at a time
    StopVirtualEditor (state, "opened again");
    auto session = std::make_shared<edithud::Session> (std::move (doc));
    session->token = state.revision + 1;
    session->previousLock = state.editLocked;
    session->role = std::move (role);
    session->context = std::move (context);
    session->plane = edit::WorldXY (planeZ);
    session->plane.locked = planeLocked;
    session->cursorBase = overlayinput::GetStats ().cursorsReasserted;
    state.virtualEditor = std::move (session);
    state.editLocked = true;
    state.shown = true;
    state.hidden.erase (editlayers::kLayer);
    state.host.open = true;
    state.host.selected = std::string (editlayers::kLayer) + "#0";
    ++state.revision;
    const auto& d = state.virtualEditor->edits.Doc ();
    ArchVizLog ("VIRTUAL EDITOR  opened: " + std::to_string (d.entities.size ()) + " objects, " +
                std::to_string (d.paths.size ()) + " paths, " +
                std::to_string (state.virtualEditor->context.points.size ()) + " snap points; the view's lock " +
                (state.virtualEditor->previousLock ? "was already on" : "borrowed"));
    return true;
}

void StopVirtualEditor (State& state, const char* reason)
{
    if (!state.virtualEditor)
        return;
    const auto& s = *state.virtualEditor;
    const uint64_t replaced = overlayinput::GetStats ().cursorsReasserted - s.cursorBase;
    ArchVizLog (std::string ("VIRTUAL EDITOR  closed (") + reason + "): Archicad replaced the cursor " +
                std::to_string (replaced) + " times; revision " + std::to_string (s.edits.Doc ().revision) + ", " +
                std::to_string (s.edits.UndoDepth ()) + " undo steps, " +
                std::to_string (s.edits.Doc ().entities.size ()) + " objects, " +
                std::to_string (s.edits.Doc ().paths.size ()) + " paths; the view's lock returned " +
                (s.previousLock ? "on" : "off"));
    state.editLocked = state.virtualEditor->previousLock;
    state.virtualEditor.reset ();
    ++state.revision;
}

bool OpenVirtualEditorWorkflow (State& state)
{
    if (state.virtualEditor)
        return true;
    edit::Document doc;
    std::array<uint8_t, 16> uuid;
    std::random_device random;
    for (auto& byte : uuid)
        byte = static_cast<uint8_t> (random ());
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
    std::string error;
    if (!tapioca::geometry::StartLandscape (doc, uuid, error))
        return false;
    if (!StartVirtualEditor (state, std::move (doc), {}, {}, 0.0, false))
        return false;
    overlaylayers::Layer layer;
    edithud::Follow (*state.virtualEditor, layer);
    overlaylayers::Set (std::move (layer));
    return true;
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

std::shared_ptr<edithud::Session> TakeVirtualEditor (State& state)
{
    if (!state.virtualEditor)
        return {};
    edithud::detail::CancelViewEdit (*state.virtualEditor, "Surface changed; unfinished input was cancelled.");
    auto& session = *state.virtualEditor;
    session.edits.Cancel ();
    session.drag = edithud::Drag::None;
    session.profileTyping = false;
    session.tracker.Hide ();
    session.drawing.clear ();
    session.drawHandles.clear ();
    session.capturedPlane.reset ();
    session.placed.clear ();
    session.profilePlaced.clear ();
    session.viewIdentity = 0;
    session.viewPlanar.reset ();
    state.editLocked = state.virtualEditor->previousLock;
    return std::exchange (state.virtualEditor, {});
}

void RestoreVirtualEditor (State& state, std::shared_ptr<edithud::Session> session)
{
    if (!session)
        return;
    session->publishedRevision = UINT64_MAX;
    state.virtualEditor = std::move (session);
    state.editLocked = true;
    state.shown = true;
    state.host.open = true;
    state.host.selected = std::string (editlayers::kLayer) + "#0";
    state.hidden.erase (editlayers::kLayer);
    ++state.revision;
    overlaylayers::Layer layer;
    if (edithud::Follow (*state.virtualEditor, layer))
        overlaylayers::Set (std::move (layer));
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
        StopVirtualEditor (state, exists ? "Done or Esc in the editor" : "its layer was removed");
        return clear;
    }
    // Each drag says, as it ends, whether Archicad fought the HUD for the cursor through it.
    const uint64_t replaced = overlayinput::GetStats ().cursorsReasserted;
    if (s.drag != edithud::Drag::None && !s.dragWatched)
        s.dragWatched = true, s.dragCursorBase = replaced;
    else if (s.drag == edithud::Drag::None && s.dragWatched) {
        s.dragWatched = false;
        if (replaced > s.dragCursorBase)
            ArchVizLog ("VIRTUAL EDITOR  drag: Archicad replaced the HUD's cursor " +
                        std::to_string (replaced - s.dragCursorBase) + " times, each put back");
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
