// OVERLAY-INVARIANTS.md §§8,11,12: one explicit queued native write through the command
// dispatcher; no ACAPI, document mutation or worker launch from ImGui layout or Present.
#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ArchViz/VirtualEditorTerrainBake.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorBake.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "NodeGraph/Json.hpp"
#include "Python/ApiDispatcher.hpp"
#include "Python/MainThreadGate.hpp"

namespace geomsrv::archviz {
namespace {
namespace eh = edithud;
namespace js = evp::nodegraph::json;

// What the dispatcher answered, as the panel says it.
std::string Answer (const std::string& text, bool& created)
{
    created = false;
    const auto parsed = js::Parse (text);
    bool ok = false;
    const auto* flag = parsed.value.Find ("ok");
    const auto* data = parsed.value.Find ("data");
    if (parsed.ok && flag && flag->AsBool (ok) && ok && data) {
        std::string guid;
        const auto* id = data->Find ("elementId");
        const auto* value = id ? id->Find ("guid") : nullptr;
        if (value && value->AsString (guid) && !guid.empty ()) {
            created = true;
            return "Archicad Mesh created (" + guid + "). One Undo in Archicad removes it.";
        }
    }
    std::string why;
    if (const auto* error = parsed.value.Find ("error")) {
        if (!error->AsString (why))
            if (const auto* message = error->Find ("message"))
                message->AsString (why);
    }
    return "The Archicad Mesh was not created: " + (why.empty () ? text : why);
}
} // namespace

void FollowTerrainBake (const std::shared_ptr<overlayhud::State>& state)
{
    const auto session = state->virtualEditor;
    if (!session || !session->bake.requested || session->bake.queued)
        return;
    session->bake.queued = true;
    const auto done = [session] (bool created, const std::string& message) {
        eh::FinishTerrainBake (*session, created, message);
        ArchVizLog ("TERRAIN bake " + message);
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    };
    // The plan as it was when Create was pressed: a later edit does not change what is made.
    const std::string params = eh::TerrainBakeRequestJson (*session);
    if (params.empty ()) {
        done (false, "The Archicad Mesh was not created: there is no sound plan to make it from.");
        return;
    }
    GS::UniString error;
    if (!evp::MainThreadGate::Get ().Post (
            [state, session, params, done] {
                if (state != guesttext::HudState () || state->virtualEditor != session) {
                    done (false, "Creating the Archicad Mesh was cancelled: the editor was closed.");
                    return;
                }
                const auto response = evp::DispatchApiCall (GS::UniString ("Tapioca.CreateMesh", CC_UTF8),
                                                            GS::UniString (params.c_str (), CC_UTF8));
                bool created = false;
                const auto message = Answer (response.ToCStr (0, MaxUSize, CC_UTF8).Get (), created);
                done (created, message);
            },
            error))
        done (false,
              "Creating the Archicad Mesh was not queued: " + std::string (error.ToCStr (0, MaxUSize, CC_UTF8).Get ()));
}
} // namespace geomsrv::archviz
