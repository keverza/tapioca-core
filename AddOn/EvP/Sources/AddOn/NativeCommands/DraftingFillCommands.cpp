#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/DraftingFillCommands.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "NativeCommands/DraftingDatabaseTarget.hpp"

#include <cmath>
#include <algorithm>
#include <clipper2/clipper.h>

namespace geomsrv {
namespace {

// AC29 hatch polygons are 1-indexed, with a closing repeat. Unlike an open
// polyline, a fill always has that repeat, including when callers omit it.
// Straight contours are validated first; optional signed arcs are attached after
// winding normalization by matching edge endpoints (including reversed holes).
bool BuildFillPolygon (const GS::Array<GS::ObjectState>& outline, const GS::Array<GS::ObjectState>& holes,
                       API_Polygon& polygon, API_ElementMemo& memo, GS::UniString& error)
{
    if (outline.GetSize () < 3 || outline.GetSize () > 4097) {
        error = "fill needs 3..4096 distinct points";
        return false;
    }

    GS::Array<API_Coord> points;
    for (const GS::ObjectState& node : outline) {
        API_Coord point = {};
        if (!ReadFiniteNumber (node, "x", point.x) || !ReadFiniteNumber (node, "y", point.y) ||
            std::abs (point.x) > 1e9 || std::abs (point.y) > 1e9) {
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
        twiceArea += (a.x - points[0].x) * (b.y - points[0].y) - (b.x - points[0].x) * (a.y - points[0].y);
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
    memo.pends = reinterpret_cast<Int32**> (BMAllocateHandle (2 * sizeof (Int32), ALLOCATE_CLEAR, 0));
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
    if (!holes.IsEmpty ()) {
        if (holes.GetSize () > 1024) {
            error = "polygon hole budget exceeded";
            return false;
        }
        // Normalize/validate holes before extending the 1-indexed memo. No clipped
        // or partial hole geometry is substituted for an invalid caller polygon.
        namespace cp = Clipper2Lib;
        cp::PathD outer;
        const API_Coord origin = (*memo.coords)[1];
        for (Int32 i = 1; i < polygon.nCoords; ++i)
            outer.emplace_back ((*memo.coords)[i].x - origin.x, (*memo.coords)[i].y - origin.y);
        cp::PathsD retained;
        Int32 total = polygon.nCoords;
        for (const auto& hole : holes) {
            GS::Array<GS::ObjectState> ring;
            if (!hole.Get ("polygonOutline", ring) || ring.GetSize () < 3 || ring.GetSize () > 4096) {
                error = "fill hole needs 3..4096 points";
                return false;
            }
            cp::PathD path;
            for (const auto& vertex : ring) {
                double x = 0, y = 0;
                if (!ReadFiniteNumber (vertex, "x", x) || !ReadFiniteNumber (vertex, "y", y) || std::abs (x) > 1e9 ||
                    std::abs (y) > 1e9) {
                    error = "fill hole needs finite coordinates";
                    return false;
                }
                path.emplace_back (x - origin.x, y - origin.y);
            }
            if (path.size () > 1 && path.front () == path.back ())
                path.pop_back ();
            const auto simple = cp::Union (cp::PathsD { path }, cp::FillRule::EvenOdd, 6);
            if (simple.size () != 1 || std::abs (cp::Area (path)) <= 1e-12 ||
                std::abs (cp::Area (simple)) < std::abs (cp::Area (path)) - 1e-6 ||
                std::abs (cp::Area (cp::Difference ({ path }, { outer }, cp::FillRule::EvenOdd, 6))) > 1e-6 ||
                std::abs (cp::Area (cp::Intersect ({ path }, retained, cp::FillRule::EvenOdd, 6))) > 1e-6) {
                error = "fill holes must be simple, inside the outer contour and non-overlapping";
                return false;
            }
            total += Int32 (path.size ()) + 1;
            if (total > 100000) {
                error = "fill polygon point budget exceeded";
                return false;
            }
            if (cp::Area (path) > 0)
                std::reverse (path.begin (), path.end ());
            retained.push_back (std::move (path));
        }
        auto coords =
            reinterpret_cast<API_Coord**> (BMAllocateHandle ((total + 1) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
        auto pends = reinterpret_cast<Int32**> (
            BMAllocateHandle (GS::GSSize ((retained.size () + 2) * sizeof (Int32)), ALLOCATE_CLEAR, 0));
        if (!coords || !pends) {
            if (coords)
                BMKillHandle (reinterpret_cast<GSHandle*> (&coords));
            if (pends)
                BMKillHandle (reinterpret_cast<GSHandle*> (&pends));
            error = "out of memory extending fill holes";
            return false;
        }
        for (Int32 i = 1; i <= polygon.nCoords; ++i)
            (*coords)[i] = (*memo.coords)[i];
        (*pends)[1] = polygon.nCoords;
        Int32 at = polygon.nCoords + 1, sub = 2;
        for (const auto& path : retained) {
            const Int32 first = at;
            for (const auto& p : path)
                (*coords)[at++] = { p.x + origin.x, p.y + origin.y };
            (*coords)[at] = (*coords)[first];
            (*pends)[sub++] = at++;
        }
        BMKillHandle (reinterpret_cast<GSHandle*> (&memo.coords));
        BMKillHandle (reinterpret_cast<GSHandle*> (&memo.pends));
        memo.coords = coords;
        memo.pends = pends;
        polygon.nCoords = total;
        polygon.nSubPolys = Int32 (retained.size ()) + 1;
    }
    return true;
}

bool AddFillArcs (const GS::ObjectState& item, API_ElementMemo& memo, Int32& arcCount, GS::UniString& error)
{
    GS::Array<API_PolyArc> arcs;
    GS::Array<GS::ObjectState> contours, holes;
    contours.Push (item);
    item.Get ("holes", holes);
    for (const auto& hole : holes)
        contours.Push (hole);
    Int32 first = 1;
    for (UIndex sub = 0; sub < contours.GetSize (); ++sub) {
        const Int32 last = (*memo.pends)[sub + 1];
        GS::Array<double> angles;
        if (contours[sub].Contains ("arcAngles")) {
            GS::Array<GS::ObjectState> vertices;
            contours[sub].Get ("polygonOutline", vertices);
            if (!contours[sub].Get ("arcAngles", angles) || angles.GetSize () != vertices.GetSize ()) {
                error = "arcAngles must have one signed angle per contour vertex";
                return false;
            }
            for (UIndex i = 0; i < angles.GetSize (); ++i) {
                const double angle = angles[i];
                if (!std::isfinite (angle) || std::abs (angle) > 3.141592653589793) {
                    error = "fill arc angle must be finite and at most pi radians";
                    return false;
                }
                if (std::abs (angle) < 1e-10)
                    continue;
                API_Coord a {}, b {};
                ReadFiniteNumber (vertices[i], "x", a.x);
                ReadFiniteNumber (vertices[i], "y", a.y);
                const auto& next = vertices[(i + 1) % vertices.GetSize ()];
                ReadFiniteNumber (next, "x", b.x);
                ReadFiniteNumber (next, "y", b.y);
                const auto same = [] (const API_Coord& p, const API_Coord& q) {
                    return std::hypot (p.x - q.x, p.y - q.y) <= 0.000002;
                };
                bool found = false;
                for (Int32 edge = first; edge < last; ++edge) {
                    const auto p = (*memo.coords)[edge], q = (*memo.coords)[edge + 1];
                    if ((same (a, p) && same (b, q)) || (same (a, q) && same (b, p))) {
                        API_PolyArc arc {};
                        arc.begIndex = edge;
                        arc.endIndex = edge + 1;
                        arc.arcAngle = same (a, p) ? angle : -angle;
                        arcs.Push (arc);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    error = "fill normalization changed a circular edge; no straight substitute created";
                    return false;
                }
            }
        }
        first = last + 1;
    }
    arcCount = Int32 (arcs.GetSize ());
    if (!arcs.IsEmpty ()) {
        memo.parcs =
            reinterpret_cast<API_PolyArc**> (BMAllocateHandle (arcCount * sizeof (API_PolyArc), ALLOCATE_CLEAR, 0));
        if (!memo.parcs) {
            error = "out of memory allocating fill arcs";
            return false;
        }
        for (UIndex i = 0; i < arcs.GetSize (); ++i)
            (*memo.parcs)[i] = arcs[i];
    }
    return true;
}

class CreateFillsCommand : public WriteCommand {
  public:
    GS::String GetName () const override
    {
        return "CreateFills";
    }

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
            if (!item.Get ("fill", fillName) || fillName.IsEmpty () || !item.Get ("polygonOutline", outline)) {
                rec.Add ("succeeded", false);
                rec.Add ("error",
                         EVP_FAIL ("each fill needs fill attribute name and polygonOutline", "Tapioca.CreateFills"));
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
                                      ? EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", defaultsError, "API_HatchID")
                                      : layerError);
                results.Push (rec);
                continue;
            }
            if (!AttributeNameToIndex (API_FilltypeID, fillName, element.hatch.fillInd)) {
                rec.Add ("succeeded", false);
                rec.Add ("error",
                         EVP_FAIL (GS::UniString ("fill attribute not found: ") + fillName, "Tapioca.CreateFills"));
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
            GS::UniString lineName;
            if (item.Get ("lineType", lineName) &&
                !AttributeNameToIndex (API_LinetypeID, lineName, element.hatch.ltypeInd)) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL ("fill contour line type not found", "Tapioca.CreateFills"));
                results.Push (rec);
                continue;
            }

            GS::Int32 floorInd = 0;
            if (item.Get ("floorInd", floorInd))
                element.header.floorInd = (short) floorInd;
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
            GS::Array<GS::ObjectState> holes;
            item.Get ("holes", holes);
            if (!BuildFillPolygon (outline, holes, element.hatch.poly, memo, polygonError) ||
                !AddFillArcs (item, memo, element.hatch.poly.nArcs, polygonError)) {
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
            }
            else {
                GS::ObjectState elementId;
                elementId.Add ("guid", GS::UniString (APIGuidToString (element.header.guid).ToCStr ()));
                rec.Add ("succeeded", true);
                rec.Add ("elementId", elementId);
                GS::UniString verificationError;
                if (!VerifyCreatedDraftingElement (element.header.guid, API_HatchID, targetDatabaseGuid, rec,
                                                   verificationError))
                    return NativeCommandResult::Failure (verificationError);
                ++created;
            }
            results.Push (rec);
        }

        bool failOnError = false;
        params.Get ("failOnError", failOnError);
        if (failOnError && created != (GS::Int32) fills.GetSize ())
            return NativeCommandResult::Failure (
                EVP_FAIL ("one or more fill elements failed; transaction should roll back", "Tapioca.CreateFills"));
        GS::ObjectState response;
        response.Add ("results", results);
        response.Add ("count", created);
        return response;
    }
};

const NativeCommandRegistration s_draftingFillRegistrations[] = {
    { "CreateFills", &MakeRegisteredNativeCommand<CreateFillsCommand>, false,
      R"json({"type":"object","properties":{"fills":{"type":"array","minItems":1,"items":{"type":"object","properties":{"fill":{"type":"string","minLength":1},"polygonOutline":{"type":"array","minItems":3,"maxItems":4097,"items":{"$ref":"#Point2D"}},"arcAngles":{"type":"array","minItems":3,"maxItems":4097,"items":{"type":"number","minimum":-3.141592653589793,"maximum":3.141592653589793}},"holes":{"type":"array","maxItems":1024,"items":{"type":"object","properties":{"polygonOutline":{"type":"array","minItems":3,"maxItems":4097,"items":{"$ref":"#Point2D"}},"arcAngles":{"type":"array","minItems":3,"maxItems":4097,"items":{"type":"number","minimum":-3.141592653589793,"maximum":3.141592653589793}}},"additionalProperties":false,"required":["polygonOutline"]}},"lineType":{"type":"string","minLength":1},"layer":{"type":"string"},"floorInd":{"type":"integer"},"pen":{"type":"integer","minimum":0,"maximum":255},"fillPen":{"type":"integer","minimum":1,"maximum":255},"fillBGPen":{"type":"integer","minimum":0,"maximum":255}},"additionalProperties":false,"required":["fill","polygonOutline"]}},"databaseAnchorElementId":{"$ref":"#ElementId"},"floorPlan":{"type":"boolean"},"failOnError":{"type":"boolean"}},"additionalProperties":false,"required":["fills"]})json",
      R"json({"type":"object","properties":{"results":{"type":"array","items":{"type":"object","properties":{"succeeded":{"type":"boolean"},"elementId":{"$ref":"#ElementId"},"databaseId":{"$ref":"#ElementId"},"layer":{"type":"string"},"verified":{"type":"boolean"},"error":{"type":"string"}},"additionalProperties":false,"required":["succeeded"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["results","count"]})json" },
};

} // namespace

bool BuildStraightPolygonMemo (const GS::Array<GS::ObjectState>& outline, const GS::Array<GS::ObjectState>& holes,
                               API_Polygon& polygon, API_ElementMemo& memo, GS::UniString& error)
{
    return BuildFillPolygon (outline, holes, polygon, memo, error);
}

NativeCommandRegistrations GetDraftingFillCommandRegistrations ()
{
    return MakeRegistrationView (s_draftingFillRegistrations);
}

} // namespace geomsrv
