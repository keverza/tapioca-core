// OVERLAY-INVARIANTS.md §§8,11,12: explicit queued native reads only; no ACAPI,
// capture, document mutation or worker launch from ImGui layout or Present.
#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ArchViz/VirtualEditorTerrainCapture.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "NodeGraph/Json.hpp"
#include "Python/ApiDispatcher.hpp"
#include "Python/MainThreadGate.hpp"
#include <set>

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
    if (!Call ("Tapioca.VirtualEditor",
               V::Object ({ { "action", V::String ("terrain") },
                            { "terrainGuid", V::String (guid) },
                            { "pathId", V::String (std::to_string (request->path)) } }),
               result, error)) {
        finish (error);
        return;
    }
    finish ("Topography captured. Edit the design path's profile and cross-section to update the preview.");
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
} // namespace geomsrv::archviz
