#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/DraftingFillCommands.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "NativeCommands/DraftingDatabaseTarget.hpp"

#include <cmath>

namespace geomsrv {
namespace {

// AC29 hatch polygons are 1-indexed, with a closing repeat. Unlike an open
// polyline, a fill always has that repeat, including when callers omit it.
// First slice: one straight outer ring, no holes or arc edges. The public
// contract rejects unsupported geometry rather than flattening curved edges.
bool BuildFillPolygon (const GS::Array<GS::ObjectState>& outline, API_Polygon& polygon,
                       API_ElementMemo& memo, GS::UniString& error)
{
    if (outline.GetSize () < 3 || outline.GetSize () > 4097) {
        error = "fill needs 3..4096 distinct points";
        return false;
    }

    GS::Array<API_Coord> points;
    for (const GS::ObjectState& node : outline) {
        API_Coord point = {};
        if (!ReadFiniteNumber (node, "x", point.x) || !ReadFiniteNumber (node, "y", point.y)) {
            error = "every fill vertex needs finite x and y";
            return false;
        }
        points.Push (point);
    }
    if (points.GetSize () > 1 && points[0].x == points.GetLast ().x && points[0].y == points.GetLast ().y)
        points.Pop ();
    const USize count = points.GetSize ();
    if (count < 3 || count > 4096) {
        error = "fill needs 3..4096 distinct points (do not send the closing point twice)";
        return false;
    }

    double twiceArea = 0.0;
    for (UIndex i = 0; i < count; ++i) {
        const API_Coord& a = points[i];
        const API_Coord& b = points[(i + 1) % count];
        if (a.x == b.x && a.y == b.y) {
            error = "fill has adjacent duplicate vertices";
            return false;
        }
        twiceArea += a.x * b.y - b.x * a.y;
    }
    if (!std::isfinite (twiceArea) || std::fabs (twiceArea) < 1e-12) {
        error = "fill contour has zero or invalid area";
        return false;
    }

    polygon.nCoords = (Int32) count + 1;
    polygon.nSubPolys = 1;
    polygon.nArcs = 0;
    memo.coords = reinterpret_cast<API_Coord**> (
        BMAllocateHandle ((polygon.nCoords + 1) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
    memo.pends = reinterpret_cast<Int32**> (
        BMAllocateHandle (2 * sizeof (Int32), ALLOCATE_CLEAR, 0));
    if (memo.coords == nullptr || memo.pends == nullptr) {
        error = "out of memory allocating fill polygon";
        return false; // caller disposes both handles, including partial allocation
    }
    // A positive signed area is CCW, the orientation used by the DevKit's
    // Do_CreateHatch example. Reverse clockwise input without moving vertex 1.
    for (UIndex i = 0; i < count; ++i)
        (*memo.coords)[i + 1] = points[twiceArea > 0.0 ? i : (count - i) % count];
    (*memo.coords)[polygon.nCoords] = (*memo.coords)[1];
    (*memo.pends)[1] = polygon.nCoords;
    return true;
}

class CreateFillsCommand : public WriteCommand {
  public:
    GS::String GetName () const override { return "CreateFills"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> fills;
        if (!params.Get ("fills", fills) || fills.IsEmpty ())
            return NativeCommandResult::Failure (EVP_FAIL ("need non-empty fills array", "Tapioca.CreateFills"));

        GS::UniString targetDatabaseGuid, databaseError;
        AnchoredWorksheetDatabase database;
        if (!database.Activate (params, targetDatabaseGuid, databaseError))
            return NativeCommandResult::Failure (databaseError);

        GS::Array<GS::ObjectState> results;
        GS::Int32 created = 0;
        for (const GS::ObjectState& item : fills) {
            GS::ObjectState rec;
            GS::UniString fillName;
            GS::Array<GS::ObjectState> outline;
            if (!item.Get ("fill", fillName) || fillName.IsEmpty () ||
                !item.Get ("polygonOutline", outline)) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL ("each fill needs fill attribute name and polygonOutline", "Tapioca.CreateFills"));
                results.Push (rec);
                continue;
            }

            API_Element element = {};
            element.header.type = API_HatchID; // the Fill TOOL, not API_FilltypeID (attribute)
            const GSErrCode defaultsError = ACAPI_Element_GetDefaults (&element, nullptr);
            GS::UniString layerError;
            if (defaultsError != NoError || !ResolveLayerParam (item, element.header, layerError)) {
                rec.Add ("succeeded", false);
                rec.Add ("error", defaultsError != NoError
                    ? EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", defaultsError, "API_HatchID") : layerError);
                results.Push (rec);
                continue;
            }
            if (!AttributeNameToIndex (API_FilltypeID, fillName, element.hatch.fillInd)) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL (GS::UniString ("fill attribute not found: ") + fillName, "Tapioca.CreateFills"));
                results.Push (rec);
                continue;
            }
            element.hatch.hatchType = API_FillHatch;
            element.hatch.determination = APIHatch_DraftingFills;
            // The tool defaults can carry RGB/material overrides from the last
            // hatch. Explicit pens and the named fill must win over that state.
            element.hatch.hatchFlags = 0;
            element.hatch.contPen.colorOverridePenIndex = 0;
            element.hatch.fillPen.colorOverridePenIndex = 0;
            element.hatch.showArea = false; // do not inherit a stale area note

            GS::Int32 floorInd = 0;
            if (item.Get ("floorInd", floorInd)) element.header.floorInd = (short) floorInd;
            GS::Int32 pen = 0;
            if (item.Contains ("pen")) {
                if (!item.Get ("pen", pen) || pen < 0 || pen > 255) {
                    rec.Add ("succeeded", false);
                    rec.Add ("error", EVP_FAIL ("contour pen must be 0..255 (0 hides contour)", "Tapioca.CreateFills"));
                    results.Push (rec);
                    continue;
                }
                element.hatch.contPen.penIndex = (short) pen;
            }
            if (item.Contains ("fillPen")) {
                if (!item.Get ("fillPen", pen) || pen < 1 || pen > 255) {
                    rec.Add ("succeeded", false);
                    rec.Add ("error", EVP_FAIL ("fillPen must be 1..255", "Tapioca.CreateFills"));
                    results.Push (rec);
                    continue;
                }
                element.hatch.fillPen.penIndex = (short) pen;
            }
            if (item.Contains ("fillBGPen")) {
                if (!item.Get ("fillBGPen", pen) || pen < 0 || pen > 255) {
                    rec.Add ("succeeded", false);
                    rec.Add ("error", EVP_FAIL ("fillBGPen must be 0..255 (0 is transparent)", "Tapioca.CreateFills"));
                    results.Push (rec);
                    continue;
                }
                element.hatch.fillBGPen = (short) pen;
            }

            API_ElementMemo memo = {};
            GS::UniString polygonError;
            if (!BuildFillPolygon (outline, element.hatch.poly, memo, polygonError)) {
                ACAPI_DisposeElemMemoHdls (&memo);
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL (polygonError, "Tapioca.CreateFills"));
                results.Push (rec);
                continue;
            }

            const GSErrCode createError = ACAPI_Element_Create (&element, &memo);
            ACAPI_DisposeElemMemoHdls (&memo);
            if (createError != NoError) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_ACAPI_FAIL ("ACAPI_Element_Create", createError, "API_HatchID"));
            } else {
                GS::ObjectState elementId;
                elementId.Add ("guid", GS::UniString (APIGuidToString (element.header.guid).ToCStr ()));
                rec.Add ("succeeded", true);
                rec.Add ("elementId", elementId);
                GS::UniString verificationError;
                if (!VerifyCreatedDraftingElement (element.header.guid, API_HatchID, targetDatabaseGuid, rec, verificationError))
                    return NativeCommandResult::Failure (verificationError);
                ++created;
            }
            results.Push (rec);
        }

        bool failOnError = false;
        params.Get ("failOnError", failOnError);
        if (failOnError && created != (GS::Int32) fills.GetSize ())
            return NativeCommandResult::Failure (EVP_FAIL ("one or more fill elements failed; transaction should roll back", "Tapioca.CreateFills"));
        GS::ObjectState response;
        response.Add ("results", results);
        response.Add ("count", created);
        return response;
    }
};

const NativeCommandRegistration s_draftingFillRegistrations[] = {
    { "CreateFills", &MakeRegisteredNativeCommand<CreateFillsCommand>, false,
      R"json({"type":"object","properties":{"fills":{"type":"array","minItems":1,"items":{"type":"object","properties":{"fill":{"type":"string","minLength":1},"polygonOutline":{"type":"array","minItems":3,"maxItems":4097,"items":{"$ref":"#Point2D"}},"layer":{"type":"string"},"floorInd":{"type":"integer"},"pen":{"type":"integer","minimum":0,"maximum":255},"fillPen":{"type":"integer","minimum":1,"maximum":255},"fillBGPen":{"type":"integer","minimum":0,"maximum":255}},"additionalProperties":false,"required":["fill","polygonOutline"]}},"databaseAnchorElementId":{"$ref":"#ElementId"},"failOnError":{"type":"boolean"}},"additionalProperties":false,"required":["fills"]})json",
      R"json({"type":"object","properties":{"results":{"type":"array","items":{"type":"object","properties":{"succeeded":{"type":"boolean"},"elementId":{"$ref":"#ElementId"},"databaseId":{"$ref":"#ElementId"},"layer":{"type":"string"},"verified":{"type":"boolean"},"error":{"type":"string"}},"additionalProperties":false,"required":["succeeded"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["results","count"]})json" },
};

} // namespace

NativeCommandRegistrations GetDraftingFillCommandRegistrations ()
{
    return MakeRegistrationView (s_draftingFillRegistrations);
}

} // namespace geomsrv
