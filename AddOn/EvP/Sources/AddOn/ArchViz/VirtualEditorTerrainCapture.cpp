// OVERLAY-INVARIANTS.md §§8,11,12: explicit queued native reads only; no ACAPI,
// capture, document mutation or worker launch from ImGui layout or Present. The one view
// change, FollowFocus (#124), is queued the same way: Archicad's zoom, on the message loop.
#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ArchViz/VirtualEditorTerrainCapture.hpp"
#include "ArchViz/VirtualEditorTerrainBake.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/ViewerEditor.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "NodeGraph/Json.hpp"
#include "Python/ApiDispatcher.hpp"
#include "Python/MainThreadGate.hpp"
#include <set>
#include <utility>

namespace geomsrv::archviz {
namespace {
namespace eh = edithud;
namespace js = evp::nodegraph::json;
using V = js::JsonValue;

bool Call (const char* command, const V& params, V& data, std::string& error)
{
    const auto response = evp::DispatchApiCall (GS::UniString (command, CC_UTF8),
                                                GS::UniString (js::Write (params, 0).c_str (), CC_UTF8));
    const std::string text = response.ToCStr (0, MaxUSize, CC_UTF8).Get ();
    const auto parsed = js::Parse (text);
    bool success = false;
    const auto* ok = parsed.value.Find ("ok");
    const auto* payload = parsed.value.Find ("data");
    if (parsed.ok && ok && ok->AsBool (success) && success && payload) {
        data = *payload;
        return true;
    }
    error = text;
    return false;
}

bool SelectedGuid (std::string& guid, std::string& error)
{
    V data;
    if (!Call ("Tapioca.GetSelection", V::Object ({}), data, error))
        return false;
    const auto* elements = data.Find ("elements");
    if (!elements || !elements->AsArray ()) {
        error = "TerrainCaptureSource: selection response has no elements.";
        return false;
    }
    std::set<std::string> guids;
    for (const auto& element : *elements->AsArray ()) {
        const auto* id = element.Find ("elementId");
        const auto* value = id ? id->Find ("guid") : nullptr;
        std::string text, canonical;
        if (!value || !value->AsString (text)) {
            error = "TerrainCaptureSource: selection contains an invalid element GUID.";
            return false;
        }
        if (text.size () == 38 && text.front () == '{' && text.back () == '}')
            text = text.substr (1, 36);
        if (!tapioca::geometry::CanonicalLandscapeGuid (text, canonical)) {
            error = "TerrainCaptureSource: selection contains an invalid element GUID.";
            return false;
        }
        guids.insert (std::move (canonical));
    }
    if (guids.size () != 1) {
        error = "Select exactly one Archicad Mesh before locking the editor view, then capture it.";
        return false;
    }
    guid = *guids.begin ();
    return true;
}

void Run (const std::shared_ptr<overlayhud::State>& state, const std::shared_ptr<eh::Session>& session,
          const std::shared_ptr<const eh::TerrainCaptureRequest>& request)
{
    const auto log = [&] (const std::string& event) {
        ArchVizLog ("TERRAIN capture " + event + " revision=" + std::to_string (request->revision) +
                    " path=" + std::to_string (request->path));
    };
    log ("dispatch");
    const auto current = [&] {
        return state == guesttext::HudState () && state->virtualEditor == session &&
               eh::TerrainCaptureIsCurrent (*session, request) && state->shown && state->editLocked &&
               overlayhud::LayerShown (*state, editlayers::kLayer);
    };
    const auto finish = [&] (const std::string& message) {
        log ("finish: " + message);
        if (session->terrainCapture != request)
            return;
        session->terrainCapture.reset ();
        session->terrainCaptureQueued = false;
        session->message = message;
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    };
    if (!current ()) {
        finish ("Topography capture cancelled: the editor changed, was hidden or unlocked.");
        return;
    }
    std::string guid = request->terrainGuid, error;
    if (guid.empty () && !SelectedGuid (guid, error)) {
        finish (error);
        return;
    }
    // Reading selection can re-enter host callbacks. Never start against the
    // replacement project/session or a path changed while this action waited.
    if (!current ()) {
        finish ("Topography capture cancelled: the editor changed while reading selection.");
        return;
    }
    V result;
    log ("native-read guid=" + guid);
    // No design path yet: pathId is left out (its schema admits no 0) and the terrain is held.
    auto params = V::Object ({ { "action", V::String ("terrain") }, { "terrainGuid", V::String (guid) } });
    if (request->path)
        params = V::Object ({ { "action", V::String ("terrain") },
                              { "terrainGuid", V::String (guid) },
                              { "pathId", V::String (std::to_string (request->path)) } });
    if (!Call ("Tapioca.VirtualEditor", params, result, error)) {
        finish (error);
        return;
    }
    // Read from the selection, the Mesh is in the workflow now: Archicad's highlight on it would
    // only distract (the user, 2026-10-24), so the selection is cleared.
    if (request->terrainGuid.empty ()) {
        const GSErrCode deselected = ACAPI_Selection_DeselectAll ();
        log ("selection cleared=" + std::to_string (deselected == NoError));
    }
    finish (request->path ? "Terrain set. Edit the design path's profile and cross-section to update the preview."
                          : "Terrain set. Enable a region's Grade/Add/Subtract or draw/select a design path.");
}
} // namespace

void FollowTerrainCapture (const std::shared_ptr<overlayhud::State>& state)
{
    const auto session = state->virtualEditor;
    if (!session || !session->terrainCapture || session->terrainCaptureQueued)
        return;
    const auto request = session->terrainCapture;
    session->terrainCaptureQueued = true;
    ArchVizLog ("TERRAIN capture queue revision=" + std::to_string (request->revision) +
                " path=" + std::to_string (request->path) +
                (request->terrainGuid.empty () ? " source=selection" : " source=bound"));
    GS::UniString error;
    if (!evp::MainThreadGate::Get ().Post ([state, session, request] { Run (state, session, request); }, error)) {
        session->terrainCaptureQueued = false;
        session->terrainCapture.reset ();
        session->message =
            "Topography capture was not queued: " + std::string (error.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        ArchVizLog ("TERRAIN capture queue-refused: " + session->message);
    }
}
void FollowFocus (const std::shared_ptr<overlayhud::State>& state, bool archicadView)
{
    const auto session = state->virtualEditor;
    if (!session || !session->focusRequest || session->focusQueued)
        return;
    const auto request = *std::exchange (session->focusRequest, std::nullopt);
    if (!archicadView) {
        // Plain floats to the render thread; the camera is framed there, never from here.
        vieweredit::RequestFocus ({ float (request.lo.x), float (request.lo.y), float (request.lo.z) },
                                  { float (request.hi.x), float (request.hi.y), float (request.hi.z) });
        ArchVizLog ("VIRTUAL EDITOR focus " + request.what + " (viewer)");
        session->message.clear ();
        return;
    }
    if (!request.planar && !request.onScreen) {
        session->message = "Cannot focus on " + request.what + " from here: it is behind the camera. Turn towards it.";
        return;
    }
    session->focusQueued = true;
    const auto done = [session] (const std::string& message) {
        session->focusQueued = false;
        session->message = message;
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    };
    GS::UniString error;
    if (!evp::MainThreadGate::Get ().Post (
            [state, session, request, done] {
                if (state != guesttext::HudState () || state->virtualEditor != session || !state->shown) {
                    done ({}); // the editor went or was hidden: nothing to frame
                    return;
                }
                GSErrCode err = NoError;
                if (request.planar) {
                    API_Box box {};
                    box.xMin = request.lo.x, box.yMin = request.lo.y;
                    box.xMax = request.hi.x, box.yMax = request.hi.y;
                    err = ACAPI_View_Zoom (&box);
                }
                else {
                    // The 3D window honours only a rectangle, in its pixels (ACAPI_View_Zoom).
                    API_Rect rect {};
                    rect.left = Int32 (request.rect[0]), rect.top = Int32 (request.rect[1]);
                    rect.right = Int32 (request.rect[2]), rect.bottom = Int32 (request.rect[3]);
                    err = ACAPI_View_Zoom (nullptr, &rect);
                }
                ArchVizLog ("VIRTUAL EDITOR focus " + request.what + (request.planar ? " (plan)" : " (3D)") +
                            " err=" + std::to_string (err));
                done (err == NoError ? std::string ()
                                     : "Archicad could not zoom to " + request.what + " (error " +
                                           std::to_string (err) + "); the front window may be another view.");
            },
            error))
        done ("Focus was not queued: " + std::string (error.ToCStr (0, MaxUSize, CC_UTF8).Get ()));
}

void FollowDraftingImport (const std::shared_ptr<overlayhud::State>& state)
{
    FollowTerrainBake (state); // #177: the same message-loop point, for the editor's one native write
    const auto session = state->virtualEditor;
    if (!session || !session->draftingImport || session->draftingImportQueued)
        return;
    session->draftingImportQueued = true;
    const uint64_t revision = session->edits.Doc ().revision;
    const std::string as = session->draftingImportAs.empty () ? "lines" : session->draftingImportAs;
    const auto done = [session] (const std::string& message) {
        session->draftingImport = session->draftingImportQueued = false;
        if (!message.empty ())
            session->message = message;
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    };
    GS::UniString error;
    if (!evp::MainThreadGate::Get ().Post (
            [state, session, revision, as, done] {
                // The editor, its view and its document as they were asked from; else nothing is read.
                if (state != guesttext::HudState () || state->virtualEditor != session || !state->shown ||
                    !state->editLocked || session->edits.Doc ().revision != revision) {
                    done ("Adding Archicad lines was cancelled: the editor changed, was hidden or unlocked.");
                    return;
                }
                // Success or not, the command has put what it added, skipped or refused in the
                // session's message.
                V result;
                std::string failure;
                Call ("Tapioca.VirtualEditor",
                      V::Object ({ { "action", V::String ("import") }, { "importAs", V::String (as) } }), result,
                      failure);
                done ({});
            },
            error))
        done ("Adding Archicad lines was not queued: " + std::string (error.ToCStr (0, MaxUSize, CC_UTF8).Get ()));
}
} // namespace geomsrv::archviz
