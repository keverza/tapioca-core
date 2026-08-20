#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/RoofCreateCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandUtils.hpp"      // AttributeNameToIndex, ResolveStory

#include <cmath>
#include <vector>

namespace geomsrv {

namespace {

// ---------------------------------------------------------------------------
// EvP.CreateRoof — a roof from a contour + one global pitch, in two modes.
//
//   mode      : "plane"|"poly"   (default "plane")
//     plane — a single tilted plane (API_PlaneRoofID). The polygon lives in the
//             standard coords/pends memo (like a slab); it tilts by `pitch` around
//             `baseLine` (default: the v0->v2 diagonal, NOT an edge — see below).
//             `riseLeft` (default true) chooses which side rises.
//     poly  — a multi-plane hip (API_PolyRoofID): the pivot lives in
//             memo.additionalPoly*, the contour in the standard coords/pends memo,
//             and a single levelData entry gives the uniform pitch. STILL inference-
//             prone (levelData/eaves) — verify separately.
//
//   outline   : double[2*M]   ring x0,y0,x1,y1,...  (distinct pts; auto-closed)
//   arcs      : double[M]?    bulge per segment i (vertex i -> i+1); 0 => straight
//   base      : double        world-Z reference height of the roof
//   floorInd  : int?          owning story (see ResolveStory)
//   pitch     : double        plane angle in DEGREES
//   riseLeft  : bool?         plane mode: side of the pivot line that rises (default true)
//   baseLine  : double[4]?    plane mode: the pivot segment [x1,y1,x2,y2]. It is an
//                             INDEPENDENT input — every roof Archicad itself makes uses
//                             an interior segment, never a contour edge. Default: the
//                             v0->v2 diagonal (v0->v1 for a triangle).
//   planeHeight : double?     poly mode: vertical extent of the level (default 1000 m)
//   overhang  : double?       poly mode: eaves extension outside the pivot (default 0)
//   polyContour : bool?       poly mode: also supply the contour polygon alongside the
//                             pivot (default true — a real poly roof carries both)
//   structure : "basic"|"composite"   (roofs have no profiles; default basic)
//   attrName  : string        building-material (basic) / roof-composite NAME
// ---------------------------------------------------------------------------
class CreateRoofCommand : public WriteCommand {
public:
    GS::String GetName () const override { return "CreateRoof"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<double> outline, arcs;
        if (!params.Get ("outline", outline) || outline.GetSize () < 6 || (outline.GetSize () % 2) != 0) {
            return NativeCommandResult::Failure ("need outline=[x0,y0,x1,y1,...] with >=3 points (even count)");
        }
        const Int32 distinct = static_cast<Int32> (outline.GetSize () / 2);
        const bool  haveArcs = params.Get ("arcs", arcs);
        if (haveArcs && static_cast<Int32> (arcs.GetSize ()) != distinct) {
            return NativeCommandResult::Failure ("arcs must have one bulge per outline vertex (segment i -> i+1)");
        }

        double base = 0.0, pitchDeg = 0.0, overhang = 0.0, planeHeight = 1000.0;
        params.Get ("base", base);
        params.Get ("pitch", pitchDeg);
        params.Get ("overhang", overhang);
        params.Get ("planeHeight", planeHeight);

        GS::Int32  requestedFloor = 0;
        const bool haveFloor = params.Get ("floorInd", requestedFloor);
        short  floorInd = 0;
        double baseOffset = 0.0;
        GS::UniString storyErr;
        if (!ResolveStory (base, haveFloor, requestedFloor, floorInd, baseOffset, storyErr)) {
            return NativeCommandResult::Failure (storyErr);
        }

        GS::UniString mode ("plane");
        params.Get ("mode", mode);
        const bool polyMode = (mode == "poly");

        API_Element element = {};
        element.header.type    = API_RoofID;
        element.roof.roofClass = polyMode ? API_PolyRoofID : API_PlaneRoofID;
        GSErrCode err = ACAPI_Element_GetDefaults (&element, nullptr);
        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", err, polyMode ? "API_RoofID/API_PolyRoofID" : "API_RoofID/API_PlaneRoofID"));
        }
        // GetDefaults may return the project's default roof (a different class); re-assert
        // the class we asked for so the union is read as we filled it.
        element.roof.roofClass = polyMode ? API_PolyRoofID : API_PlaneRoofID;
        // A zero-thickness shell is invalid; the DevKit plane-roof snippet sets one too.
        if (element.roof.shellBase.thickness <= 0.0)
            element.roof.shellBase.thickness = 0.3;

        // Structure — basic (building material) or composite. No profiles on roofs.
        GS::UniString structure, attrName;
        if (params.Get ("structure", structure) && !structure.IsEmpty ()) {
            params.Get ("attrName", attrName);
            API_AttributeIndex idx;
            if (structure == "basic") {
                element.roof.shellBase.modelElemStructureType = API_BasicStructure;
                if (!attrName.IsEmpty ()) {
                    if (!AttributeNameToIndex (API_BuildingMaterialID, attrName, idx)) {
                        return NativeCommandResult::Failure (GS::UniString::Printf ("building material not found: %T", attrName.ToPrintf ()));
                    }
                    element.roof.shellBase.buildingMaterial = idx;
                }
            } else if (structure == "composite") {
                element.roof.shellBase.modelElemStructureType = API_CompositeStructure;
                if (!AttributeNameToIndex (API_CompWallID, attrName, idx)) {
                    return NativeCommandResult::Failure (GS::UniString::Printf ("composite not found: %T", attrName.ToPrintf ()));
                }
                element.roof.shellBase.composite = idx;
            } else {
                return NativeCommandResult::Failure ("structure must be basic|composite for roofs");
            }
        }

        element.header.floorInd      = floorInd;
        element.roof.shellBase.level = baseOffset;

        // Which segments actually bulge — nArcs must match the parcs we emit.
        GS::Array<Int32> arcSegs;
        if (haveArcs) {
            for (Int32 i = 0; i < distinct; ++i)
                if (std::fabs (arcs[i]) > 1e-12) arcSegs.Push (i);
        }
        const Int32  nArcs    = static_cast<Int32> (arcSegs.GetSize ());
        const Int32  nCoords  = distinct + 1;   // +1: Archicad closes the ring by repeating pt 1
        const double pitchRad = pitchDeg * (3.14159265358979323846 / 180.0);

        // Winding: roofs are stricter than meshes about contour orientation, so let the
        // caller normalise it ("asis" | "cw" | "ccw"). Reversing with arcs would misalign
        // per-segment bulges, so winding is only applied to arc-free rings. ox/oy are the
        // ring vertices in the order we actually emit.
        GS::UniString winding ("asis");
        params.Get ("winding", winding);
        GS::Array<double> ox, oy;
        {
            bool reverse = false;
            if ((winding == "cw" || winding == "ccw") && nArcs == 0) {
                double s2 = 0.0;
                for (Int32 i = 0; i < distinct; ++i) {
                    const Int32 j = (i + 1) % distinct;
                    s2 += outline[i * 2 + 0] * outline[j * 2 + 1] - outline[j * 2 + 0] * outline[i * 2 + 1];
                }
                const bool isCCW = s2 > 0.0;
                reverse = (winding == "ccw") ? !isCCW : isCCW;   // "cw" wants NOT-CCW
            }
            for (Int32 i = 0; i < distinct; ++i) {
                const Int32 k = reverse ? (distinct - 1 - i) : i;
                ox.Push (outline[k * 2 + 0]);
                oy.Push (outline[k * 2 + 1]);
            }
        }

        // A degenerate contour is the one failure Archicad reports uselessly: it answers
        // APIERR_IRREGULARPOLY, which reads like a polygon-topology complaint and sent
        // this investigation chasing memo layouts for days. The actual cause is upstream
        // and mundane — JSON integers. `Get<GS::Array<double>>` yields 0.0 for an Int
        // element, so an outline of [0.0, 0.0, 4, 0.0, 4, 4, 0.0, 4] arrives as four
        // coincident points. The palette no longer emits integers for evp.Float inputs
        // (ControlPalette::JsonReal), but a script can still build a list of Python ints,
        // so catch it here and say what is wrong.
        {
            double area2 = 0.0;
            for (Int32 i = 0; i < distinct; ++i) {
                const Int32 j = (i + 1) % distinct;
                area2 += ox[i] * oy[j] - ox[j] * oy[i];
            }
            if (std::fabs (area2) < 1e-9) {
                return NativeCommandResult::Failure (GS::UniString ("outline encloses no area — the contour is "
                        "degenerate (points coincide or are collinear). If you passed whole "
                        "numbers, send them as REALS: a JSON integer (4) does not survive as "
                        "a double and arrives as 0.0, while 4.0 does."));
            }
        }

        API_ElementMemo memo = {};

        // One ring, emitted into whichever memo triple is passed in. A real roof's
        // memo carries a NON-NULL zero-length parcs even with no arcs (RoofDumpProbe:
        // `parcs=0`, not `-1`), so we allocate it unconditionally — that was the last
        // remaining polygon-memo difference from a hand-made roof.
        auto fillRing = [&] (API_Coord**& coordsH, Int32**& pendsH, API_PolyArc**& parcsH) -> bool {
            coordsH = reinterpret_cast<API_Coord**> (
                BMAllocateHandle ((nCoords + 1) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
            pendsH = reinterpret_cast<Int32**> (
                BMAllocateHandle (2 * sizeof (Int32), ALLOCATE_CLEAR, 0));
            parcsH = reinterpret_cast<API_PolyArc**> (
                BMAllocateHandle (nArcs * sizeof (API_PolyArc), ALLOCATE_CLEAR, 0));
            if (coordsH == nullptr || pendsH == nullptr || (nArcs > 0 && parcsH == nullptr))
                return false;

            for (Int32 i = 0; i < distinct; ++i) {
                (*coordsH)[i + 1].x = ox[i];
                (*coordsH)[i + 1].y = oy[i];
            }
            (*coordsH)[nCoords] = (*coordsH)[1];   // close the ring
            (*pendsH)[1] = nCoords;
            for (USize k = 0; k < arcSegs.GetSize (); ++k) {
                const Int32 seg = arcSegs[k];
                (*parcsH)[k].begIndex = seg + 1;
                (*parcsH)[k].endIndex = seg + 2;
                (*parcsH)[k].arcAngle = arcs[seg];
            }
            return true;
        };

        if (polyMode) {
            // Multi-plane (poly) roof. The PIVOT polygon lives in the additionalPoly*
            // memo. The CONTOUR is the open question: we used to leave it empty on the
            // theory that it is derived from the pivot by the offset overhang, but the
            // ACAPI_Element_Create table (ACAPinc.h:3346) reads cumulatively — "The roof
            // polygon is required; ... BESIDES THESE, for Multi-plane roofs the pivot
            // poly is ALSO required" — and RoofDumpProbe found a real poly roof carrying
            // BOTH (contour in coords/pends, pivot in addPolyCoords/addPolyPends).
            // So we now supply both. `polyContour:false` restores the pivot-only shape
            // without a rebuild.
            bool polyContour = true;
            params.Get ("polyContour", polyContour);

            API_PolyRoofData& poly = element.roof.u.polyRoof;
            poly.pivotPolygon.nCoords     = nCoords;
            poly.pivotPolygon.nSubPolys   = 1;
            poly.pivotPolygon.nArcs       = nArcs;
            poly.contourPolygon.nCoords   = polyContour ? nCoords : 0;
            poly.contourPolygon.nSubPolys = polyContour ? 1 : 0;
            poly.contourPolygon.nArcs     = polyContour ? nArcs : 0;

            if (!fillRing (memo.additionalPolyCoords, memo.additionalPolyPends, memo.additionalPolyParcs) ||
                (polyContour && !fillRing (memo.coords, memo.pends, memo.parcs))) {
                ACAPI_DisposeElemMemoHdls (&memo);
                return NativeCommandResult::Failure ("not enough memory to build the roof polygon memo");
            }

            poly.overHangType  = API_OffsetOverhang;
            poly.eavesOverHang = overhang;
            poly.levelNum      = 1;
            poly.levelData[0].levelAngle  = pitchRad;
            poly.levelData[0].levelHeight = planeHeight;

        } else {
            // Single-plane roof: a flat polygon in the standard coords/pends memo (exactly
            // like a slab), tilted by `angle` around a pivot baseLine. The simplest,
            // best-understood roof — the first thing to get landing before multi-plane.
            API_PlaneRoofData& plane = element.roof.u.planeRoof;
            plane.poly.nCoords   = nCoords;
            plane.poly.nSubPolys = 1;
            plane.poly.nArcs     = nArcs;
            plane.angle          = pitchRad;

            if (!fillRing (memo.coords, memo.pends, memo.parcs)) {
                ACAPI_DisposeElemMemoHdls (&memo);
                return NativeCommandResult::Failure ("not enough memory to build the roof polygon memo");
            }

            // The pivot line the plane tilts around. It is an INDEPENDENT input, not a
            // property of the contour: the forum sample uses the v0->v2 diagonal, and all
            // four roofs RoofDumpProbe read back use an interior segment (one of which
            // ends outside the contour entirely). We always used edge v0->v1, which is the
            // one thing 4/4 known-good roofs avoid — plausibly why Archicad answers
            // IRREGULARPOLY (splitting the polygon along an edge yields a zero-area piece).
            // Default is now the diagonal; `baseLine` overrides it outright.
            GS::Array<double> baseLine;
            if (params.Get ("baseLine", baseLine) && baseLine.GetSize () == 4) {
                plane.baseLine.c1.x = baseLine[0];
                plane.baseLine.c1.y = baseLine[1];
                plane.baseLine.c2.x = baseLine[2];
                plane.baseLine.c2.y = baseLine[3];
            } else {
                const Int32 pivotEnd = (distinct >= 4) ? 2 : 1;   // a diagonal when there is one
                plane.baseLine.c1.x = ox[0];
                plane.baseLine.c1.y = oy[0];
                plane.baseLine.c2.x = ox[pivotEnd];
                plane.baseLine.c2.y = oy[pivotEnd];
            }

            // posSign=true raises the plane on the LEFT of the pivot vector; `riseLeft`
            // flips it. (The forum sample never sets it — bisect rung 6.)
            bool riseLeft = true;
            params.Get ("riseLeft", riseLeft);
            plane.posSign = riseLeft;
        }

        // NO undo scope here — see WriteCommand. The caller has one open.
        err = ACAPI_Element_Create (&element, &memo);
        ACAPI_DisposeElemMemoHdls (&memo);

        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_Create", err, GS::UniString::Printf (
                "roof mode=%T, %d outline pts, arcs=%s, floor %d, base %.3f, pitch %.2f deg",
                mode.ToPrintf (), (int) distinct, haveArcs ? "yes" : "no", (int) floorInd, base, pitchDeg)));
        }
        GS::ObjectState elementId;
        elementId.Add ("guid", GS::UniString (APIGuidToString (element.header.guid).ToCStr ()));
        os.Add ("elementId", elementId);
        os.Add ("floorInd", (GS::Int32) floorInd);
        return os;
    }
};

const NativeCommandRegistration RoofCreateCommandRegistrations[] = {
    { "CreateRoof", &MakeRegisteredNativeCommand<CreateRoofCommand>, false,
      R"json({"type":"object","properties":{"outline":{"type":"array","minItems":6,"items":{"type":"number"}},"arcs":{"type":"array","items":{"type":"number"}},"base":{"type":"number"},"floorInd":{"type":"integer"},"pitch":{"type":"number"},"mode":{"type":"string","enum":["plane","poly"]},"riseLeft":{"type":"boolean"},"baseLine":{"type":"array","minItems":4,"maxItems":4,"items":{"type":"number"}},"planeHeight":{"type":"number"},"overhang":{"type":"number"},"polyContour":{"type":"boolean"},"structure":{"type":"string","enum":["basic","composite"]},"attrName":{"type":"string"},"winding":{"type":"string","enum":["asis","cw","ccw"]}},"additionalProperties":false,"required":["outline"]})json",
      R"json({"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"floorInd":{"type":"integer"}},"additionalProperties":false,"required":["elementId","floorInd"]})json" },
};

}   // namespace

NativeCommandRegistrations GetRoofCreateCommandRegistrations ()
{
    return MakeRegistrationView (RoofCreateCommandRegistrations);
}

} // namespace geomsrv
