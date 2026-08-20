#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/TopologyCommands.hpp"
#include "NativeCommands/CommandBase.hpp"

namespace geomsrv {

namespace {


// ===========================================================================
// E6 — Zone/room topology. The two relationships the apartment grouping actually
// depends on, absorbed natively off the Tapir proxy. Both are ordinary reads
// (MainThreadCommand, not IsWrite): they call ACAPI to evaluate element solids /
// grouping, so they take the gate, but change nothing and get no undo scope.
//
// Ported from Tapir's ElementCommands.cpp (GetCollisions / GetConnectedElements),
// against the real AC29 symbols: ACAPI_Element_GetCollisions (APIdefs_Goodies.h
// API_CollisionElem / API_CollisionDetectionSettings) and
// ACAPI_Grouping_GetConnectedElements (ACAPinc.h). GetZoneBoundaries is
// deliberately NOT ported: the apartment grouping does not use it (Apartment
// Graph works off collisions + connected elements precisely because they are
// robust to stale / manually-drawn zones, unlike a boundary query), so a
// ModelerAPI boundary walk would be untested dead code.
//
// Wire format is Tapioca v2 typed element records and named relationship records.
// ===========================================================================

// Element-type non-localized name -> API_ElemTypeID, for connectedElementType.
// The architectural subset the topology/apartment work needs; extend as required.
// Names match Tapir's GetElementTypeFromNonLocalizedName (CommandBase.cpp) so a
// caller can pass the same strings it passed through the proxy. API_ZombieElemID
// signals "unknown", which the caller turns into a structured error.
API_ElemTypeID ElementTypeFromName (const GS::UniString& name)
{
    if (name == "Wall")        return API_WallID;
    if (name == "Column")      return API_ColumnID;
    if (name == "Beam")        return API_BeamID;
    if (name == "Window")      return API_WindowID;
    if (name == "Door")        return API_DoorID;
    if (name == "Object")      return API_ObjectID;
    if (name == "Lamp")        return API_LampID;
    if (name == "Slab")        return API_SlabID;
    if (name == "Roof")        return API_RoofID;
    if (name == "Mesh")        return API_MeshID;
    if (name == "Zone")        return API_ZoneID;
    if (name == "Hatch")       return API_HatchID;
    if (name == "Shell")       return API_ShellID;
    if (name == "Skylight")    return API_SkylightID;
    if (name == "Morph")       return API_MorphID;
    if (name == "Stair")       return API_StairID;
    if (name == "Railing")     return API_RailingID;
    if (name == "CurtainWall") return API_CurtainWallID;
    if (name == "Opening")     return API_OpeningID;
    return API_ZombieElemID;
}

// ---------------------------------------------------------------------------
// Tapioca.GetCollisions { elements1:[...], elements2:[...], volumeTolerance?,
//                     performSurfaceCheck?, surfaceTolerance? }
//   -> { collisions:[{firstElement,secondElement,...}], count }
//
// Which elements in group 1 collide with elements in group 2, tested on element
// SOLIDS (robust to zone-recalculation state — this is exactly why the apartment
// grouping uses collisions rather than a boundary/adjacency query). Defaults
// mirror the ACAPI struct: volume/surface tolerance 0.001, surface check OFF; the
// evp.topology wrapper supplies the apartment-suite defaults. The response has
// one named record per collision pair. `hasClearenceCollision` keeps
// ACAPI's own (mis)spelling so it matches Tapir's payload and the existing dumps.
// ---------------------------------------------------------------------------
class GetCollisionsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetCollisions"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::ObjectState> elements1, elements2;
        if (!params.Get ("elements1", elements1) || !params.Get ("elements2", elements2))
            return NativeCommandResult::Failure ("need elements1 and elements2");

        API_CollisionDetectionSettings settings = {};
        settings.volumeTolerance     = 0.001;
        settings.performSurfaceCheck = false;
        settings.surfaceTolerance    = 0.001;
        auto readNumber = [&params] (const char* name, double& value) {
            if (params.IsReal (name)) return params.Get (name, value);
            if (params.IsInt (name)) { Int64 v = 0; if (params.Get (name, v)) { value = (double) v; return true; } }
            if (params.IsUInt (name)) { UInt64 v = 0; if (params.Get (name, v)) { value = (double) v; return true; } }
            return false;
        };
        readNumber ("volumeTolerance", settings.volumeTolerance);
        params.Get ("performSurfaceCheck", settings.performSurfaceCheck);
        readNumber ("surfaceTolerance", settings.surfaceTolerance);

        GS::Array<API_Guid> group1, group2;
        for (const GS::ObjectState& item : elements1) {
            GS::ObjectState elementId;
            GS::UniString guid;
            if (!item.Get ("elementId", elementId) || !elementId.Get ("guid", guid) || guid.IsEmpty ())
                return NativeCommandResult::Failure ("every elements1 item needs elementId.guid");
            group1.Push (APIGuidFromString (guid.ToCStr ().Get ()));
        }
        for (const GS::ObjectState& item : elements2) {
            GS::ObjectState elementId;
            GS::UniString guid;
            if (!item.Get ("elementId", elementId) || !elementId.Get ("guid", guid) || guid.IsEmpty ())
                return NativeCommandResult::Failure ("every elements2 item needs elementId.guid");
            group2.Push (APIGuidFromString (guid.ToCStr ().Get ()));
        }

        GS::Array<GS::Pair<API_CollisionElem, API_CollisionElem>> result;
        const GSErrCode err = ACAPI_Element_GetCollisions (group1, group2, result, settings);
        if (err != NoError) {
            return NativeCommandResult::Failure (
                EVP_ACAPI_FAIL ("ACAPI_Element_GetCollisions", err, GS::UniString::Printf ("%u vs %u element(s)", (unsigned) group1.GetSize (), (unsigned) group2.GetSize ())));
        }

        GS::Array<GS::ObjectState> collisions;
        for (const auto& pair : result) {
            GS::ObjectState firstId, firstElement, secondId, secondElement, collision;
            firstId.Add ("guid", GS::UniString (APIGuidToString (pair.first.collidedElemGuid).ToCStr ()));
            firstElement.Add ("elementId", firstId);
            secondId.Add ("guid", GS::UniString (APIGuidToString (pair.second.collidedElemGuid).ToCStr ()));
            secondElement.Add ("elementId", secondId);
            collision.Add ("firstElement", firstElement);
            collision.Add ("secondElement", secondElement);
            collision.Add ("hasBodyCollision", pair.first.hasBodyCollision);
            collision.Add ("hasClearenceCollision", pair.second.hasClearenceCollision);
            collisions.Push (collision);
        }

        os.Add ("collisions", collisions);
        os.Add ("count", (GS::Int32) collisions.GetSize ());
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.GetConnectedElements { elements:[...], connectedElementType:"Door" }
//   -> { connections:[{elementId,succeeded,connectedElements}], count }
//
// For each owner element, return one aligned record containing its connected
// elements. An individual API failure has `succeeded=false` and an empty list.
// An unknown connectedElementType is a hard structured error.
// ---------------------------------------------------------------------------
class GetConnectedElementsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetConnectedElements"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::ObjectState> elements;
        if (!params.Get ("elements", elements))
            return NativeCommandResult::Failure ("need elements=[{elementId:{guid}}]");

        GS::UniString typeName;
        if (!params.Get ("connectedElementType", typeName) || typeName.IsEmpty ())
            return NativeCommandResult::Failure ("need connectedElementType (e.g. \"Wall\", \"Zone\", \"Door\")");
        const API_ElemTypeID typeId = ElementTypeFromName (typeName);
        if (typeId == API_ZombieElemID)
            return NativeCommandResult::Failure (GS::UniString ("unknown connectedElementType: \"" + typeName + "\""));
        const API_ElemType connectedType (typeId);   // implicit generic-variation ctor

        GS::Array<GS::ObjectState> connections;
        for (const GS::ObjectState& item : elements) {
            GS::ObjectState elementId;
            GS::UniString guidString;
            if (!item.Get ("elementId", elementId) || !elementId.Get ("guid", guidString) || guidString.IsEmpty ())
                return NativeCommandResult::Failure ("every element needs elementId.guid");
            const API_Guid owner = APIGuidFromString (guidString.ToCStr ().Get ());

            GS::Array<API_Guid> connected;
            GS::Array<GS::ObjectState> connectedElements;
            const bool succeeded = (ACAPI_Grouping_GetConnectedElements (owner, connectedType, &connected) == NoError);
            if (succeeded) {
                for (const API_Guid& c : connected) {
                    GS::ObjectState connectedId, connectedElement;
                    connectedId.Add ("guid", GS::UniString (APIGuidToString (c).ToCStr ()));
                    connectedElement.Add ("elementId", connectedId);
                    connectedElements.Push (connectedElement);
                }
            }
            GS::ObjectState connection;
            connection.Add ("elementId", elementId);
            connection.Add ("succeeded", succeeded);
            connection.Add ("connectedElements", connectedElements);
            connections.Push (connection);
        }

        os.Add ("connections", connections);
        os.Add ("count", (GS::Int32) connections.GetSize ());
        return os;
    }
};

const NativeCommandRegistration commandRegistrations[] = {
    { "GetCollisions", &MakeRegisteredNativeCommand<GetCollisionsCommand>, false,
      R"json({"type":"object","properties":{"elements1":{"$ref":"#Elements"},"elements2":{"$ref":"#Elements"},"volumeTolerance":{"type":"number","minimum":0},"performSurfaceCheck":{"type":"boolean"},"surfaceTolerance":{"type":"number","minimum":0}},"additionalProperties":false,"required":["elements1","elements2"]})json",
      R"json({"oneOf":[{"type":"object","properties":{"collisions":{"type":"array","items":{"type":"object","properties":{"firstElement":{"$ref":"#Element"},"secondElement":{"$ref":"#Element"},"hasBodyCollision":{"type":"boolean"},"hasClearenceCollision":{"type":"boolean"}},"additionalProperties":false,"required":["firstElement","secondElement","hasBodyCollision","hasClearenceCollision"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["collisions","count"]},{"type":"object","properties":{"ok":{"const":false},"error":{"type":"string"}},"additionalProperties":false,"required":["ok","error"]}]})json" },
    { "GetConnectedElements", &MakeRegisteredNativeCommand<GetConnectedElementsCommand>, false,
      R"json({"type":"object","properties":{"elements":{"$ref":"#Elements"},"connectedElementType":{"type":"string","enum":["Wall","Column","Beam","Window","Door","Object","Lamp","Slab","Roof","Mesh","Zone","Hatch","Shell","Skylight","Morph","Stair","Railing","CurtainWall","Opening"]}},"additionalProperties":false,"required":["elements","connectedElementType"]})json",
      R"json({"oneOf":[{"type":"object","properties":{"connections":{"type":"array","items":{"type":"object","properties":{"elementId":{"$ref":"#ElementId"},"succeeded":{"type":"boolean"},"connectedElements":{"$ref":"#Elements"}},"additionalProperties":false,"required":["elementId","succeeded","connectedElements"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["connections","count"]},{"type":"object","properties":{"ok":{"const":false},"error":{"type":"string"}},"additionalProperties":false,"required":["ok","error"]}]})json" },
};

}   // namespace

NativeCommandRegistrations GetTopologyCommandRegistrations ()
{
    return MakeRegistrationView (commandRegistrations);
}

} // namespace geomsrv
