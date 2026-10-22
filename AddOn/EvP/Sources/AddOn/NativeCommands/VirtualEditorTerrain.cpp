#include "APIEnvir.h"
#include "ACAPinc.h"
#include "NativeCommands/VirtualEditorTerrain.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "Geometry/GeometryExtractor.hpp"
#include <Model.hpp>
#include <charconv>

// OVERLAY-INVARIANTS.md §§5,8,12: detached main-thread capture only; worker
// surface/design computation and shared editor layer, no renderer/selection writes.
namespace geomsrv {
bool StartNativeTerrainDesign (archviz::edithud::Session& session, const GS::ObjectState& params, std::string& error)
{
#if !defined(TAPIOCA_GEOMETRY_RUST)
    (void) session;
    (void) params;
    error = "Terrain design requires the Rust geometry build.";
    return false;
#else
    namespace eh = archviz::edithud;
    GS::UniString terrainText, pathText;
    std::string canonical;
    if (!params.Get ("terrainGuid", terrainText) || !params.Get ("pathId", pathText) ||
        !tapioca::geometry::CanonicalLandscapeGuid (terrainText.ToCStr (0, MaxUSize, CC_UTF8).Get (), canonical)) {
        error = "Supply a terrain Mesh GUID and an authored landscape pathId.";
        return false;
    }
    const std::string pathValue = pathText.ToCStr (0, MaxUSize, CC_UTF8).Get ();
    edit::Id path = 0;
    const auto parsed = std::from_chars (pathValue.data (), pathValue.data () + pathValue.size (), path);
    if (parsed.ec != std::errc {} || parsed.ptr != pathValue.data () + pathValue.size () || path == 0 ||
        !session.edits.Doc ().landscape || !edit::FindPath (session.edits.Doc (), path) ||
        eh::GizmoModeLocked (session) || session.edits.InGesture () || !session.drawing.empty ()) {
        error = "Finish the current input and choose an existing landscape path.";
        return false;
    }
    const API_Guid guid = APIGuidFromString (canonical.c_str ());
    API_Elem_Head head {};
    head.guid = guid;
    if (ACAPI_Element_GetHeader (&head) != NoError || head.type.typeID != API_MeshID) {
        error = "TerrainCaptureSource: terrainGuid must name an existing Archicad Mesh.";
        return false;
    }
    ModelerAPI::Model model;
    if (!AcquireCurrentModel (model)) {
        error = "TerrainCaptureSource: the current model is unavailable.";
        return false;
    }
    const auto index = model.GetElementIndex (APIGuid2GSGuid (guid));
    auto mesh = std::make_shared<Mesh> ();
    if (!index || !ExtractElementAt (model, *index, *mesh)) {
        error = "TerrainCaptureSource: the requested Mesh has no extractable geometry in the current model.";
        return false;
    }
    const auto stamp = head.modiStamp;
    // Called only by the editor's main-thread poll/status/draw preparation.
    // It never enters the job, and remains valid across viewer/overlay switches.
    auto current = [guid, stamp] {
        API_Elem_Head now {};
        now.guid = guid;
        return ACAPI_Element_GetHeader (&now) == NoError && now.type.typeID == API_MeshID && now.modiStamp == stamp;
    };
    if (!eh::StartTerrainDesign (session, eh::TerrainDesignBaseline { std::move (mesh), true, std::move (current) },
                                 path)) {
        error = session.message;
        return false;
    }
    eh::PollTerrainDesign (session);
    error.clear ();
    return true;
#endif
}

GS::ObjectState VirtualTerrainStatus (archviz::edithud::Session& session)
{
    // A status request is also a main-thread completion point when both views
    // are closed. Poll touches only derived cache state, never document history.
    archviz::edithud::PollTerrainDesign (session);
    const auto status = archviz::edithud::GetTerrainDesignStatus (session);
    GS::ObjectState result;
    result.Add ("active", status.active);
    result.Add ("pending", status.pending);
    result.Add ("current", status.current);
    result.Add ("built", status.built);
    result.Add ("revision", static_cast<GS::Int64> (status.revision));
    result.Add ("vertices", static_cast<GS::Int64> (status.vertices));
    result.Add ("triangles", static_cast<GS::Int64> (status.triangles));
    result.Add ("conflicts", static_cast<GS::Int64> (status.conflicts));
    result.Add ("cut", status.cut);
    result.Add ("fill", status.fill);
    result.Add ("error", GS::UniString (status.error.c_str (), CC_UTF8));
    return result;
}
} // namespace geomsrv
