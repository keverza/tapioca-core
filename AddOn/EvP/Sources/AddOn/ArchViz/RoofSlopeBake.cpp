// OVERLAY-INVARIANTS.md §8/§11: never write the model from HUD layout or Present.
#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ArchViz/RoofSlopeBake.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "ArchViz/HudConsole.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "Python/ApiDispatcher.hpp"
#include "Python/MainThreadGate.hpp"

namespace geomsrv::archviz::roofslopebake {
namespace {
namespace js = evp::nodegraph::json;
using V = js::JsonValue;
bool Call (const char* command, const V& params, V& data, std::string& error)
{
    const auto response = evp::DispatchApiCall (GS::UniString (command, CC_UTF8),
                                                GS::UniString (js::Write (params, 0).c_str (), CC_UTF8));
    const std::string text = response.ToCStr (0, MaxUSize, CC_UTF8).Get ();
    const auto parsed = js::Parse (text);
    bool ok = false;
    const auto* verdict = parsed.value.Find ("ok");
    const auto* payload = parsed.value.Find ("data");
    if (parsed.ok && verdict && verdict->AsBool (ok) && ok && payload) {
        data = *payload;
        if (std::string (command) == "Tapioca.CommitTransaction")
            ArchVizLog ("ROOF BAKE committed GUID ledger: " + text);
        return true;
    }
    error = text;
    return false;
}
void Run (const std::shared_ptr<overlayhud::State>& state, const std::shared_ptr<roofhud::Session>& session,
          uint64_t revision)
{
    // A project switch, another command, Cancel or a changed design invalidates
    // this queued request. No late write is permitted against a new session.
    if (state != guesttext::HudState () || state->roofEditor != session || session->close)
        return;
    std::string error;
    auto fail = [&] (const std::string& reason) {
        session->baking = false;
        session->editor.message = "Bake failed; no model changes: " + reason;
        hudconsole::Error ("Roof bake", session->editor.message);
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    };
    if (session->editor.revision != revision || !state->editLocked || !state->shown ||
        !overlayhud::LayerShown (*state, roofslope::kLayer)) {
        fail ("The design changed, was hidden or unlocked before Bake began.");
        return;
    }
    API_Element source {};
    source.header.guid = APIGuidFromString (GS::UniString (session->sourceGuid.c_str (), CC_UTF8).ToCStr ());
    if (ACAPI_Element_Get (&source) != NoError || source.header.type != API_SlabID) {
        fail ("The source slab is missing. Reopen the editor from a slab.");
        return;
    }
    if (uint64_t (source.header.modiStamp) != session->sourceStamp) {
        fail ("The source slab changed after opening the editor. Reopen it before Bake.");
        return;
    }
    const auto names =
        V::Object ({ { "libraryPartNames", V::Array ({ V::String ("Slope Symbol"), V::String ("Slope Symbol 29"),
                                                       V::String ("Slope Symbol 27") }) } });
    V part, existing;
    if (!Call ("Tapioca.GetLibraryPartInfo", names, part, error) ||
        !Call ("Tapioca.FindPlacedObjects", names, existing, error)) {
        fail (error);
        return;
    }
    double length = 0;
    if (const auto* value = part.Find ("sizeA"))
        value->AsDouble (length);
    std::string inherit;
    if (const auto* elements = existing.Find ("elements"); elements && elements->AsArray ())
        if (!elements->AsArray ()->empty ()) {
            const auto* id = elements->AsArray ()->back ().Find ("elementId");
            if (const auto* guid = id ? id->Find ("guid") : nullptr)
                guid->AsString (inherit);
        }
    V request;
    size_t roofs = 0, levels = 0;
    if (!Transaction (session->editor.data, source.header.floorInd, length, inherit, request, roofs, levels, error)) {
        fail (error);
        return;
    }
    ArchVizLog ("ROOF BAKE edited geometry/annotation recipe: " + js::Write (request, 0));
    // Annotation elements need the model's floor-plan database even when Bake
    // was clicked in 3D. Borrow it without changing the visible window; restore
    // on every path, just as the existing native CreateMesh command does.
    API_DatabaseInfo previous {};
    if (ACAPI_Database_GetCurrentDatabase (&previous) != NoError) {
        fail ("Cannot read the current database.");
        return;
    }
    API_DatabaseInfo plan {};
    plan.typeID = APIWind_FloorPlanID;
    const bool switched = previous.typeID != APIWind_FloorPlanID;
    if (switched && ACAPI_Database_ChangeCurrentDatabase (&plan) != NoError) {
        fail ("Cannot borrow the model floor-plan database.");
        return;
    }
    V result;
    struct RestoreDatabase {
        API_DatabaseInfo database;
        bool active;
        ~RestoreDatabase ()
        {
            if (active)
                ACAPI_Database_ChangeCurrentDatabase (&database);
        }
    } restore { previous, switched };
    // Library loading/database work may invoke host callbacks. Validate again at
    // the final write boundary; never restore an old database into a new project.
    if (state != guesttext::HudState ()) {
        restore.active = false;
        fail ("The project changed before model creation.");
        return;
    }
    API_Elem_Head current {};
    current.guid = source.header.guid;
    if (state->roofEditor != session || session->close || session->editor.revision != revision || !state->editLocked ||
        !state->shown || !overlayhud::LayerShown (*state, roofslope::kLayer) ||
        ACAPI_Element_GetHeader (&current) != NoError || uint64_t (current.modiStamp) != session->sourceStamp) {
        fail ("The source slab or edit session changed before model creation.");
        return;
    }
    const bool committed = Call ("Tapioca.CommitTransaction", request, result, error);
    const bool restored = !switched || ACAPI_Database_ChangeCurrentDatabase (&previous) == NoError;
    restore.active = !restored;
    if (!committed) {
        fail (error);
        if (!restored)
            hudconsole::Warning ("Roof bake", "The previous database could not be restored.");
        return;
    }
    session->close = true;
    hudconsole::Note (
        "Roof bake",
        "Created " + std::to_string (roofs) + " plane Roofs, " + std::to_string (roofs) + " slope symbols and " +
            std::to_string (levels) +
            " associative level dimensions in one Undo step. Source slab unchanged. GUID ledger: archviz.log.");
    if (!restored)
        hudconsole::Warning ("Roof bake",
                             "Bake committed, but the previous database could not be restored. Do not retry Bake.");
}
} // namespace
void Follow (const std::shared_ptr<overlayhud::State>& state)
{
    auto session = state->roofEditor;
    if (!session || !session->bakeRequested || session->baking || session->close)
        return;
    session->bakeRequested = false;
    session->baking = true;
    const auto revision = session->editor.revision;
    GS::UniString error;
    if (!evp::MainThreadGate::Get ().Post ([state, session, revision] () { Run (state, session, revision); }, error)) {
        session->baking = false;
        session->editor.message = "Bake was not queued: " + std::string (error.ToCStr (0, MaxUSize, CC_UTF8).Get ());
    }
}
} // namespace geomsrv::archviz::roofslopebake
