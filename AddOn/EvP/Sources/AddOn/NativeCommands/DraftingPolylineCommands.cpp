#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/DraftingPolylineCommands.hpp"

#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "NativeCommands/DraftingDatabaseTarget.hpp"

namespace geomsrv {

namespace {

class ListDraftingPolylinesCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "ListDraftingPolylines";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString targetDatabaseGuid;
        GS::UniString databaseError;
        AnchoredWorksheetDatabase database;
        if (!database.Activate (params, targetDatabaseGuid, databaseError))
            return NativeCommandResult::Failure (databaseError);

        GS::Array<API_Guid> guids;
        if (const GSErrCode err = ACAPI_Element_GetElemList (API_PolyLineID, &guids); err != NoError)
            return NativeCommandResult::Failure ("could not list Polylines in the target worksheet");

        GS::Array<GS::ObjectState> polylines;
        for (const API_Guid& guid : guids) {
            API_Element element = {};
            element.header.guid = guid;
            if (ACAPI_Element_Get (&element) != NoError)
                continue;
            API_Box3D bounds = {};
            if (ACAPI_Element_CalcBounds (&element.header, &bounds) != NoError)
                continue;

            GS::ObjectState item;
            GS::ObjectState elementId;
            elementId.Add ("guid", GS::UniString (APIGuidToString (guid).ToCStr ()));
            item.Add ("elementId", elementId);
            item.Add ("x", bounds.xMin);
            item.Add ("y", bounds.yMin);
            item.Add ("width", bounds.xMax - bounds.xMin);
            item.Add ("height", bounds.yMax - bounds.yMin);
            item.Add ("layer", AttributeIndexToName (API_LayerID, element.header.layer));
            polylines.Push (item);
        }

        GS::ObjectState result;
        GS::ObjectState databaseId;
        databaseId.Add ("guid", targetDatabaseGuid);
        result.Add ("databaseId", databaseId);
        result.Add ("polylines", polylines);
        result.Add ("count", (GS::Int32) polylines.GetSize ());
        return result;
    }
};

class CreateDraftingPolylineCommand : public WriteCommand {
  public:
    GS::String GetName () const override
    {
        return "CreateDraftingPolyline";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> coordinates;
        if (!params.Get ("coordinates", coordinates) || coordinates.GetSize () < 2)
            return NativeCommandResult::Failure ("coordinates needs at least two {x,y} points");

        GS::UniString targetDatabaseGuid;
        GS::UniString databaseError;
        AnchoredWorksheetDatabase database;
        if (!database.Activate (params, targetDatabaseGuid, databaseError))
            return NativeCommandResult::Failure (databaseError);

        API_Element element = {};
        element.header.type = API_PolyLineID;
        if (const GSErrCode err = ACAPI_Element_GetDefaults (&element, nullptr); err != NoError)
            return NativeCommandResult::Failure ("could not read Polyline tool defaults");

        GS::UniString layerError;
        if (!ResolveLayerParam (params, element.header, layerError))
            return NativeCommandResult::Failure (layerError);

        element.polyLine.poly.nCoords = (Int32) coordinates.GetSize ();
        element.polyLine.poly.nSubPolys = 1;
        element.polyLine.poly.nArcs = 0;

        API_ElementMemo memo = {};
        memo.coords = reinterpret_cast<API_Coord**> (
            BMAllocateHandle ((element.polyLine.poly.nCoords + 1) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
        memo.pends = reinterpret_cast<Int32**> (
            BMAllocateHandle ((element.polyLine.poly.nSubPolys + 1) * sizeof (Int32), ALLOCATE_CLEAR, 0));
        if (memo.coords == nullptr || memo.pends == nullptr) {
            ACAPI_DisposeElemMemoHdls (&memo);
            return NativeCommandResult::Failure ("out of memory allocating Polyline coordinates");
        }

        (*memo.pends)[0] = 0;
        (*memo.pends)[1] = element.polyLine.poly.nCoords;
        for (UIndex index = 0; index < coordinates.GetSize (); ++index) {
            double x = 0.0;
            double y = 0.0;
            if (!coordinates[index].Get ("x", x) || !coordinates[index].Get ("y", y)) {
                ACAPI_DisposeElemMemoHdls (&memo);
                return NativeCommandResult::Failure ("every Polyline coordinate needs numeric x and y");
            }
            (*memo.coords)[index + 1] = { x, y };
        }

        const GSErrCode createError = ACAPI_Element_Create (&element, &memo);
        ACAPI_DisposeElemMemoHdls (&memo);
        if (createError != NoError)
            return NativeCommandResult::Failure ("ACAPI_Element_Create failed for the page-boundary Polyline");

        GS::ObjectState result;
        GS::ObjectState elementId;
        elementId.Add ("guid", GS::UniString (APIGuidToString (element.header.guid).ToCStr ()));
        result.Add ("elementId", elementId);
        GS::UniString verificationError;
        if (!VerifyCreatedDraftingElement (element.header.guid, API_PolyLineID, targetDatabaseGuid, result,
                                           verificationError)) {
            return NativeCommandResult::Failure (verificationError);
        }
        return result;
    }
};

const NativeCommandRegistration
    DraftingPolylineCommandRegistrations
        [] = {
            { "ListDraftingPolylines", &MakeRegisteredNativeCommand<ListDraftingPolylinesCommand>, false,
              R"json({"type":"object","properties":{"databaseAnchorElementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["databaseAnchorElementId"]})json",
              R"json({"type":"object","properties":{"databaseId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"polylines":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"x":{"type":"number"},"y":{"type":"number"},"width":{"type":"number"},"height":{"type":"number"},"layer":{"type":"string"}},"additionalProperties":false,"required":["elementId","x","y","width","height","layer"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["databaseId","polylines","count"]})json" },
            { "CreateDraftingPolyline", &MakeRegisteredNativeCommand<CreateDraftingPolylineCommand>,
              false, R"json({"type":"object","properties":{"coordinates":{"type":"array","minItems":2,"items":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}},"additionalProperties":false,"required":["x","y"]}},"layer":{"type":"string"},"databaseAnchorElementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["coordinates"]})json", R"json({"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"databaseId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"layer":{"type":"string"},"verified":{"type":"boolean"}},"additionalProperties":false,"required":["elementId","databaseId","layer","verified"]})json" },
        };

} // namespace

NativeCommandRegistrations GetDraftingPolylineCommandRegistrations ()
{
    return MakeRegistrationView (DraftingPolylineCommandRegistrations);
}

} // namespace geomsrv
