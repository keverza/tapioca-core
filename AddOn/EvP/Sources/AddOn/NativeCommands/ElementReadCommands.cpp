#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ElementReadCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp"   // WalkPolygonRings -- the one polygon walk

#include <algorithm>

namespace geomsrv {

namespace {

// ---------------------------------------------------------------------------
// Tapioca.GetElementInfo { elements:[{elementId:{guid}}] } -> named records.
//
// What a placement script needs about elements it did NOT create: which STORY
// each one is on, and (for objects) its current angle. Tapir's
// GetDetailsOfElements is the equivalent but returns
// NotYetSupportedElementTypeDetails for a Roof — whereas `header.floorInd` is
// available for EVERY element type, because it lives on the header rather than
// in the type-specific union. That is precisely why this exists.
//
// Shape: ONE NESTED RECORD PER ELEMENT (§E16.0), positionally aligned to the
// input — `infoOfElements[i]` describes `guids[i]`. Header-level fields only, so
// there is no type-specific `details` sub-object here (unlike GetElementDetails).
// ---------------------------------------------------------------------------
class GetElementInfoCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetElementInfo"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::ObjectState> elements;
        if (!params.Get ("elements", elements)) {
            return NativeCommandResult::Failure ("need elements=[{elementId:{guid}}]");
        }

        GS::Array<GS::ObjectState> records;

        for (const GS::ObjectState& item : elements) {
            GS::ObjectState elementId;
            GS::UniString guidString;
            if (!item.Get ("elementId", elementId) || !elementId.Get ("guid", guidString)) {
                return NativeCommandResult::Failure ("every element needs elementId.guid");
            }
            API_Element element = {};
            element.header.guid = APIGuidFromString (guidString.ToCStr ().Get ());

            const bool ok = (ACAPI_Element_Get (&element) == NoError);

            GS::ObjectState rec;
            rec.Add ("elementId", elementId);
            rec.Add ("found", ok);

            if (!ok) {
                // A miss still emits a full RECORD — a short list would silently
                // shift every later element's data onto the wrong guid, exactly as
                // a short parallel array used to.
                rec.Add ("type",     GS::UniString ());
                rec.Add ("floorInd", (GS::Int32) 0);
                rec.Add ("angle",    0.0);
                records.Push (rec);
                continue;
            }

            const API_ElemTypeID typeId = element.header.type.typeID;
            rec.Add ("type",     GS::UniString::Printf ("%d", (int) typeId));
            rec.Add ("floorInd", (GS::Int32) element.header.floorInd);
            // Only objects/lamps carry an angle; anything else reports 0.
            rec.Add ("angle",    (typeId == API_ObjectID || typeId == API_LampID)
                                 ? element.object.angle : 0.0);
            records.Push (rec);
        }

        os.Add ("infoOfElements", records);
        os.Add ("count", (GS::Int32) records.GetSize ());
        return os;
    }
};

// Footprint rings of a polygon element (slab/roof/mesh/polyline) from its memo,
// distinct points (each sub-polygon's closing repeat dropped). The OUTER contour goes
// to outerCoords with its vertex count in outerCount; each HOLE ring is appended to
// holeCoords and its vertex count pushed to holeCounts, with nHoles set — so a caller
// can subtract holes for a NET footprint. Element-agnostic (sub-polygon count from the
// pends handle size), so the same helper serves every polygon type. Returns false
// (outer absent/degenerate) with outerCount 0 and nHoles 0.
//
// ⚠️ THE WALK ITSELF IS NOT HERE. Archicad's 1-indexed coords, closing repeat,
// sub-polygon indexing and per-vertex arc lookup live in ONE place —
// CommandUtils' WalkPolygonRings — because the connection-polygon read
// (GetWallPlanOutlines) gets the same handle triple from an API_WallRelation
// rather than a memo, and two copies of that indexing is how one of them goes
// stale. Read its header comment for the convention and for what polylineMode,
// outerClosed and the Z arrays mean; this function is now only the MEMO half:
// which mask to ask for and which handles hold the answer.
//
// PER-VERTEX Z (`outerZ`/`holeZ`, the mesh kind) lives in memo.meshPolyZ, which is why
// passing either array adds APIMemoMask_MeshPolyZ to the mask here.
//
// PIVOT POLYGON (`additionalPolygon`, E15) — a MULTI-PLANE roof carries a second
// polygon: the pivot polygon its planes hinge on, in
// memo.additionalPolyCoords/Pends/Parcs behind APIMemoMask_AdditionalPolygon. It is the
// SAME layout as memo.coords — so it is read by pointing the walk at the other handle
// set, not by writing a second walk. (memo.coords is the roof's own contour for BOTH
// roof classes; only the pivot lives in the additional handles.)
static bool AppendRings (const API_Guid& guid,
                         GS::Array<double>& outerCoords, GS::Array<double>& outerArcs, GS::Int32& outerCount,
                         GS::Array<double>& holeCoords, GS::Array<double>& holeArcs, GS::Array<GS::Int32>& holeCounts,
                         GS::Int32& nHoles,
                         bool polylineMode = false, bool* outerClosed = nullptr,
                         GS::Array<double>* outerZ = nullptr, GS::Array<double>* holeZ = nullptr,
                         bool additionalPolygon = false)
{
    // Set here as well as in the walk, because a memo that fails to load never
    // reaches the walk and must still leave the caller's counters at zero.
    outerCount = 0;
    nHoles     = 0;
    if (outerClosed != nullptr)
        *outerClosed = false;

    const bool   wantZ = (outerZ != nullptr || holeZ != nullptr);
    const UInt64 mask  = additionalPolygon ? APIMemoMask_AdditionalPolygon
                       : wantZ             ? (APIMemoMask_Polygon | APIMemoMask_MeshPolyZ)
                                           : APIMemoMask_Polygon;

    API_ElementMemo memo = {};
    const GSErrCode memoErr = ACAPI_Element_GetMemo (guid, &memo, mask);

    bool ok = false;
    if (memoErr == NoError) {
        // The one thing that differs between the two polygons is WHICH handles hold it.
        PolygonHandles polygon;
        polygon.coords = additionalPolygon ? memo.additionalPolyCoords : memo.coords;
        polygon.pends  = additionalPolygon ? memo.additionalPolyPends  : memo.pends;
        polygon.parcs  = additionalPolygon ? memo.additionalPolyParcs  : memo.parcs;

        // Only a mesh carries this; null for every other polygon element, which is why
        // the Z arrays stay empty rather than filling with zeros.
        const double* polyZ = (wantZ && memo.meshPolyZ != nullptr) ? *memo.meshPolyZ : nullptr;

        ok = WalkPolygonRings (polygon, polyZ,
                               outerCoords, outerArcs, outerCount,
                               holeCoords, holeArcs, holeCounts, nHoles,
                               polylineMode, outerClosed, outerZ, holeZ);
    }
    ACAPI_DisposeElemMemoHdls (&memo);
    return ok;
}

// A coordinate as Tapir spells it (§E16.0): an object, not two flat doubles.
// Costs ~2-3x the JSON bytes of [x,y] — irrelevant at element volumes, and it is
// what makes an EvP record and a Tapir record the same thing to a caller.
static GS::ObjectState Coord2D (double x, double y)
{
    GS::ObjectState c;
    c.Add ("x", x);
    c.Add ("y", y);
    return c;
}

static GS::ObjectState Coord3D (double x, double y, double z)
{
    GS::ObjectState c;
    c.Add ("x", x);
    c.Add ("y", y);
    c.Add ("z", z);
    return c;
}

// Nested-record form of the footprint read (§E16.0): the outer contour as
// polygonOutline/polygonArcs, and `holes` as a list of the SAME self-similar
// {polygonOutline, polygonArcs} shape — so a hole and a contour are read by one
// piece of caller code.
//
// This deliberately WRAPS AppendRings instead of re-walking the memo: the
// sub-polygon indexing above is subtle (1-indexed coords, closing repeat, arc
// begIndex lookup) and must not exist twice.
//
// `polylineMode` / `outerClosed` forward to AppendRings — see its header comment for why an
// open polyline must not have its last vertex dropped. `holes` comes back empty in that mode.
//
// `outlineZ` (the mesh kind): pass it and each vertex's elevation comes back alongside the
// arcs — outer contour into `outlineZ`, and each hole gains its own `polygonZ` so a hole ring
// stays self-similar with the contour. Leave it null and the 2D kinds read exactly as before.
// `additionalPolygon` (a poly roof's pivot polygon) forwards to AppendRings unchanged.
static bool ReadRingsNested (const API_Guid& guid,
                             GS::Array<GS::ObjectState>& outline, GS::Array<double>& outlineArcs,
                             GS::Array<GS::ObjectState>& holes,
                             bool polylineMode = false, bool* outerClosed = nullptr,
                             GS::Array<double>* outlineZ = nullptr,
                             bool additionalPolygon = false)
{
    GS::Array<double>    flatOuter, flatOuterArcs, flatHole, flatHoleArcs;
    GS::Array<double>    flatOuterZ, flatHoleZ;
    GS::Array<GS::Int32> holeCounts;
    GS::Int32            outerCount = 0, nHoles = 0;

    const bool wantZ = (outlineZ != nullptr);
    const bool ok = AppendRings (guid, flatOuter, flatOuterArcs, outerCount,
                                 flatHole, flatHoleArcs, holeCounts, nHoles,
                                 polylineMode, outerClosed,
                                 wantZ ? &flatOuterZ : nullptr,
                                 wantZ ? &flatHoleZ  : nullptr,
                                 additionalPolygon);

    for (GS::Int32 i = 0; i < outerCount; ++i) {
        outline.Push (Coord2D (flatOuter[i * 2], flatOuter[i * 2 + 1]));
        outlineArcs.Push (i < (GS::Int32) flatOuterArcs.GetSize () ? flatOuterArcs[i] : 0.0);
    }
    // Copied whole, NOT padded to outerCount: an element with no meshPolyZ leaves this
    // empty, and a caller comparing its length against the outline is how that shows up.
    if (outlineZ != nullptr)
        *outlineZ = flatOuterZ;

    GS::Int32 cursor = 0;   // running vertex index into the flat hole buffers
    for (GS::Int32 h = 0; h < nHoles; ++h) {
        const GS::Int32 c = holeCounts[h];

        GS::Array<GS::ObjectState> ring;
        GS::Array<double>          ringArcs, ringZ;
        for (GS::Int32 i = 0; i < c; ++i) {
            const GS::Int32 v = cursor + i;
            ring.Push (Coord2D (flatHole[v * 2], flatHole[v * 2 + 1]));
            ringArcs.Push (v < (GS::Int32) flatHoleArcs.GetSize () ? flatHoleArcs[v] : 0.0);
            if (wantZ && v < (GS::Int32) flatHoleZ.GetSize ())
                ringZ.Push (flatHoleZ[v]);
        }

        GS::ObjectState hole;
        hole.Add ("polygonOutline", ring);
        hole.Add ("polygonArcs",    ringArcs);
        if (wantZ)
            hole.Add ("polygonZ",   ringZ);
        holes.Push (hole);

        cursor += c;
    }
    return ok;
}

// Cross-section of the FIRST assembly segment of a beam/column: nominalWidth ->
// w, nominalHeight -> h. Tapered/multi-segment members report segment 0 as
// representative; the caller carries nSegments so "there is more" stays visible.
// `beamNotColumn` picks the memo mask + array. Zeros on any read failure.
static void ReadFirstSegmentSection (const API_Guid& guid, bool beamNotColumn,
                                     double& w, double& h)
{
    w = 0.0;
    h = 0.0;
    API_ElementMemo memo = {};
    const UInt64 mask = beamNotColumn ? APIMemoMask_BeamSegment : APIMemoMask_ColumnSegment;
    if (ACAPI_Element_GetMemo (guid, &memo, mask) == NoError) {
        if (beamNotColumn && memo.beamSegments != nullptr) {
            w = memo.beamSegments[0].assemblySegmentData.nominalWidth;
            h = memo.beamSegments[0].assemblySegmentData.nominalHeight;
        } else if (!beamNotColumn && memo.columnSegments != nullptr) {
            w = memo.columnSegments[0].assemblySegmentData.nominalWidth;
            h = memo.columnSegments[0].assemblySegmentData.nominalHeight;
        }
    }
    ACAPI_DisposeElemMemoHdls (&memo);
}

// A mesh's LEVEL LINES (the UI calls them ridges; the API calls them level lines and
// Tapir's schema calls them sublines) into `d`, as `sublines` — one
// {coordinates:[{x,y,z}], vertexIds:[…]} per line, self-similar with the contour's
// coordinate spelling.
//
// ⚠️ The memo layout here is the OPPOSITE convention from coords/pends, and it is the
// open question EvP.CreateMesh was built against a guess about. Read from Tapir's
// shipping pair — GetDetailsOfElements (ElementCommands.cpp, API_MeshID case) and
// BuildMeshSublinesMemoFromGeometry (ExtendedElementCommands.cpp), which read and write
// each other's data — the convention is:
//   meshLevelCoords : 0-INDEXED, exactly nCoords slots, no unused leading element.
//   meshLevelEnds   : 0-INDEXED, exactly nSubLines slots (NOT nSubLines+1), where
//                     ends[i] is the CUMULATIVE, EXCLUSIVE end index — line i spans
//                     coords[ends[i-1] .. ends[i]-1], with ends[-1] taken as 0.
// This reader follows that. It also returns the RAW `levelEnds` array and the element's
// own `nSubLines`/`nLevelCoords`, so the interpretation is EVIDENCE in the dump rather
// than an assumption baked into the reader: if a hand-made mesh's line point counts do
// not match what the user counts on screen, levelEnds says why.
static void AddMeshSublines (const API_Element& element, GS::ObjectState& d)
{
    d.Add ("nSubLines",    (GS::Int32) element.mesh.levelLines.nSubLines);
    d.Add ("nLevelCoords", (GS::Int32) element.mesh.levelLines.nCoords);

    GS::Array<GS::Int32>       levelEnds;
    GS::Array<GS::ObjectState> sublines;

    if (element.mesh.levelLines.nSubLines > 0) {
        API_ElementMemo memo = {};
        if (ACAPI_Element_GetMemo (element.header.guid, &memo, APIMemoMask_MeshLevel) == NoError
            && memo.meshLevelCoords != nullptr && memo.meshLevelEnds != nullptr) {
            const Int32 nEnds   = (Int32) (BMGetHandleSize ((GSHandle) memo.meshLevelEnds)
                                           / sizeof (Int32));
            const Int32 nCoords = (Int32) (BMGetHandleSize ((GSHandle) memo.meshLevelCoords)
                                           / sizeof (API_MeshLevelCoord));
            Int32 cursor = 0;
            for (Int32 i = 0; i < nEnds; ++i) {
                const Int32 end = (*memo.meshLevelEnds)[i];
                levelEnds.Push ((GS::Int32) end);

                GS::Array<GS::ObjectState> coordinates;
                GS::Array<GS::Int32>       vertexIds;
                // Clamped to the real handle size as well as to `end` — a mis-read
                // convention must produce a short line, never an out-of-bounds read.
                for (; cursor < end && cursor < nCoords; ++cursor) {
                    const API_MeshLevelCoord& c = (*memo.meshLevelCoords)[cursor];
                    coordinates.Push (Coord3D (c.c.x, c.c.y, c.c.z));
                    vertexIds.Push ((GS::Int32) c.vertexID);
                }

                GS::ObjectState line;
                line.Add ("coordinates", coordinates);
                line.Add ("vertexIds",   vertexIds);
                sublines.Push (line);
            }
        }
        ACAPI_DisposeElemMemoHdls (&memo);
    }

    d.Add ("levelEnds", levelEnds);      // raw meshLevelEnds — the diagnostic
    d.Add ("sublines",  sublines);
}

// A roof's PIVOT — the hinge its pitch is measured from, and the missing half of the
// roof record (E15). Which fields are real depends on `roofClass`, so that is emitted
// first and the caller branches on it:
//
//   plane (API_PlaneRoofID) — a single-plane roof hinges on a LINE. `baseLine` is the
//     two nodes the user clicked first, `posSign` is which side of it slopes up, and
//     `slantAngle` (already on the record) is the pitch. Those three plus the contour
//     are exactly what Archicad's own input sequence asks for, so they are what a
//     create path has to reproduce.
//   poly (API_PolyRoofID) — a multi-plane roof hinges on a POLYGON, read through the
//     additionalPoly* handles (see AppendRings), and its pitch is PER LEVEL: `levels`
//     carries each level's angle and height, which is why `slantAngle` is 0 here and
//     always has been. `eavesOverHang`/`overHangType` complete the eave geometry.
//
// ⚠️ Every field is emitted for BOTH classes, with zeros/empties on the side that does
// not apply. That keeps one key set per kind — the invariant every probe run checks and
// the offline sweep asserts — and it follows the precedent already set by `slantAngle`
// reporting 0 for a poly roof rather than vanishing. `roofClass` is what says which half
// to read; a field being zero is not evidence of anything on its own.
static void AddRoofClassData (const API_Element& element, GS::ObjectState& d)
{
    const bool poly = (element.roof.roofClass == API_PolyRoofID);
    d.Add ("roofClass", GS::UniString (poly ? "poly" : "plane"));

    GS::ObjectState            baseLine;
    GS::Array<GS::ObjectState> pivotOutline, levels;
    GS::Array<double>          pivotArcs;
    bool   posSign       = false;
    double eavesOverHang = 0.0;
    GS::Int32 overHangType = 0, levelNum = 0;

    if (!poly) {
        const API_PlaneRoofData& pl = element.roof.u.planeRoof;
        posSign = pl.posSign;
        baseLine.Add ("begCoordinate", Coord2D (pl.baseLine.c1.x, pl.baseLine.c1.y));
        baseLine.Add ("endCoordinate", Coord2D (pl.baseLine.c2.x, pl.baseLine.c2.y));
    } else {
        const API_PolyRoofData& pr = element.roof.u.polyRoof;
        eavesOverHang = pr.eavesOverHang;
        overHangType  = (GS::Int32) pr.overHangType;
        // levelData is a FIXED 16-slot array; levelNum says how many are real. Reading
        // past it would report uninitialised angles as roof pitches.
        levelNum = (GS::Int32) pr.levelNum;
        const GS::Int32 nLevels = std::min (levelNum, (GS::Int32) 16);
        for (GS::Int32 i = 0; i < nLevels; ++i) {
            GS::ObjectState level;
            level.Add ("levelAngle",  pr.levelData[i].levelAngle);    // radians
            level.Add ("levelHeight", pr.levelData[i].levelHeight);   // meters
            levels.Push (level);
        }
        GS::Array<GS::ObjectState> holes;    // the pivot polygon's holes are not surfaced
        ReadRingsNested (element.header.guid, pivotOutline, pivotArcs, holes,
                         /*polylineMode*/ false, /*outerClosed*/ nullptr,
                         /*outlineZ*/ nullptr, /*additionalPolygon*/ true);
        // An empty baseLine object would be a second key set; emit the zeroed pair.
        baseLine.Add ("begCoordinate", Coord2D (0.0, 0.0));
        baseLine.Add ("endCoordinate", Coord2D (0.0, 0.0));
    }

    d.Add ("baseLine",      baseLine);        // plane only; zeros for a poly roof
    d.Add ("posSign",       posSign);         // plane only: which side slopes up
    d.Add ("pivotOutline",  pivotOutline);    // poly only; empty for a plane roof
    d.Add ("pivotArcs",     pivotArcs);
    d.Add ("levels",        levels);          // poly only: per-level angle + height
    d.Add ("levelNum",      levelNum);
    d.Add ("eavesOverHang", eavesOverHang);
    d.Add ("overHangType",  overHangType);
}

// ---------------------------------------------------------------------------
// Tapioca.GetElementDetails { elements:[{elementId:{guid}}] } -> one nested record per element.
//
// Parametric geometry read DIRECTLY from the element (never from the 3D mesh) for
// the types a metrics or annotation command cares about. Each element reports a
// `kind`, and only the fields meaningful for that
// kind appear in its `details` — a new kind costs a branch here and nothing anywhere
// else (§E16.1).
//
//   POLYGON members (slab, roof): horizontal footprint outline + thickness.
//     roof uses shellBase.thickness/level, and the SAME APIMemoMask_Polygon
//     memo.coords gives the contour projected on the horizontal plane (true for
//     single-plane u.planeRoof.poly AND multi-plane roofs).
//     A roof also carries its PIVOT (E15) — see AddRoofClassData: `roofClass` plus
//     `baseLine`/`posSign` for a plane roof, or `pivotOutline`/`levels` for a poly one.
//     Without it a caller can read a roof's outline but not what its pitch is measured
//     FROM, and cannot even tell which of the two kinds of roof it is holding.
//   TERRAIN member (mesh, §E16 Path 2): the same polygon read, plus the two things
//     that make a mesh a surface rather than a footprint — `polygonZ`, one elevation
//     per contour vertex, and `sublines`, the interior level lines. Read because
//     CreateMesh/PullToMesh were write-only: there was no way to inspect what they
//     produced, which makes ridge-index debugging a guess. Now it is a read.
//   AXIS members (wall, beam) + POINT member (column): NATIVE primitives, not a
//     synthesized footprint (locked decision 2026-07-22).
//     wall: begC->endC, thickness (begin), height, base level = bottomOffset.
//     beam: begC->endC, curveAngle (planAngle), top level; section from segment 0.
//     column: origoPos as a point (axis begin==end), height, slant/rotation
//     angles; section from segment 0.
//   CHAIN member (polyline, §E16 Path 1): the same `polygonOutline`/`polygonArcs`
//     spelling as a footprint — deliberately, so one reader serves both — plus
//     `closed`, which says whether the chain wraps back to its first vertex. That
//     flag is not decoration: an OPEN polyline has no closing repeat in the memo,
//     so AppendRings must be told it is reading a polyline or it eats the last
//     vertex. No `holes`: a polyline has one contour.
//     ⚠️ Deviation from Tapir #4: Tapir spells this `details.coordinates`. EvP keeps
//     `polygonOutline` because the record is then self-similar with slab/roof (same
//     caller code, same `_ring()`), and because EvP carries per-vertex arc angles
//     that Tapir's flat list has nowhere to put.
//   POINT members (object, lamp): `pos` as the axis pair (begin == end, exactly as
//     column), `level`, `planAngle` = the rotation, `xRatio`/`yRatio` stretch,
//     `reflected`, and `libraryPartName` — without the name an object record is an
//     anonymous point. API_LampType IS API_ObjectType, so one branch serves both.
//
//   SLOPE / SLANT (`slantAngle`, radians) — each type's tilt option, one angle:
//     roof   = single-plane pitch u.planeRoof.angle (0 for multi-plane; per-plane
//              there). wall = slantAlpha (pi/2 = plumb; slantBeta of a double-slant
//              wall is not surfaced). beam = slantAngle (0 = level). column =
//              slantAngle (pi/2 = plumb). `isSlanted` carries the real bool for
//              beam/column; it is false for wall/roof (they have no such flag —
//              read the angle).
//
// SHAPE — one nested record per element, Tapir-style (§E16.0):
//
//   { count, detailsOfElements: [ {elementId, found, kind, floorInd, value,
//                                      details{…}} ] }
//
// positionally aligned to the input: detailsOfElements[i] describes elements[i]. A
// guid that does not resolve — or resolves to a type this command does not speak —
// still emits a record with found=false, kind="" and an EMPTY details, so a caller
// can index it blindly. That alignment property is the one thing worth keeping from
// the old parallel-array form. A miss also carries `reason`
// ("notFound" | "unsupportedType"), `typeName` and `typeId`, so a bulk read reports
// WHICH types are still unspoken instead of a list of anonymous guids.
//
// `elementId` (the Element Settings "ID" box) is on EVERY record, including the
// misses — it is type-agnostic, so it is the one detail this command can give for
// a type it cannot otherwise speak. Write it back with EvP.SetElementIds.
//
// Only the fields MEANINGFUL for a kind appear in `details`; there is no
// zero-filling, which is what lets a new kind be added without taxing every other
// one. Kind-specific fields live INSIDE `details` and are never promoted to the top
// level (Tapir's discipline — see the §E16.1 warning about record width).
//
//   POLYGON kinds (slab, roof): `polygonOutline` [{x,y}, …] + `polygonArcs` (one
//     angle per vertex, 0 = straight) + `holes` [{polygonOutline, polygonArcs}, …]
//     + `hasHoles`. Holes are self-similar to the contour, so one piece of caller
//     code reads both. See ReadRingsNested / AppendRings for the memo indexing.
//   TERRAIN kind (mesh): the polygon keys above, each ring gaining a `polygonZ`
//     array parallel to its vertices; plus `level`, `skirtLevel`, `skirtType`,
//     `ridges` (the 3D smoothing mode), `sublines`, `levelEnds`, `nSubLines` and
//     `nLevelCoords`. See AddMeshSublines for the level-line memo convention.
//     ⚠️ Deviation from Tapir #5: Tapir's MeshDetails spells the contour
//     `polygonCoordinates` as a list of {x,y,z}. EvP keeps 2D `polygonOutline` and
//     hangs the elevations off it as a parallel `polygonZ` — exactly how `polygonArcs`
//     already rides along — so a mesh contour goes through the same `_ring()`, the
//     same area and the same perimeter code as a slab's. A separate 3D spelling would
//     buy a tidier single array and cost every polygon helper a second code path.
//   AXIS kinds (wall, beam) + POINT kind (column): `begCoordinate` / `endCoordinate`
//     as {x,y,z} — Tapir's WallDetails spelling. z is the member's base/reference
//     elevation, not a true 3D endpoint: these are extrusions, so a caller wanting
//     the top adds `height`. A column is a point, so begin == end == origin.
//   CHAIN kind (polyline): `polygonOutline` + `polygonArcs` + `closed`.
//   POINT kinds (object, lamp): `begCoordinate` == `endCoordinate` == the origin,
//     plus `level`, `planAngle`, `xRatio`, `yRatio`, `reflected`, `libraryPartName`.
//
// Fields verified against AC29 headers: API_WallType begC/endC/thickness/height/
// bottomOffset/slantAlpha (:2206/:2212/:2099/:2081/:2087/:2330); API_BeamType begC/
// endC/curveAngle/level/slantAngle/isSlanted/nSegments (:4713/:4719/:4726/:4695/
// :4783/:4777); API_ColumnType origoPos/height/bottomOffset/slantAngle/isSlanted/
// axisRotationAngle/nSegments (:4211/:4121/:4127/:4262/:4244/:4217); API_PlaneRoofData
// angle (:6577); API_ShellBaseType level/thickness (:6260/:6266); API_AssemblySegmentData
// nominalWidth/nominalHeight (:3763/:3769); segment count via element.{beam,column}
// .nSegments + memo.{beam,column}Segments (Element_Modify_ChangeParameters.cpp:124,
// 3D_Test.c:539); API_MeshType level/skirtLevel/skirt/smoothRidges/levelLines (:7800/
// :7806/:7788/:7776/:7885) with API_MeshLevel nCoords/nSubLines (:7656/:7662),
// API_MeshLevelCoord c/vertexID (:7628/:7634) and APIRidge_AllSharp/AllSmooth/UserSharp
// (:7613-7615); the mesh memo handles are listed for API_MeshID in ACAPinc.h:3116;
// API_PolyLineType poly (:10468) with API_PolyLineID == 20 (:135);
// API_ObjectType pos/angle/level/xRatio/yRatio/reflected/libInd (:5686/:5578/:5584/
// :5590/:5596/:5554/:5674), API_LampType = API_ObjectType (:5713); library-part name
// via ACAPI_LibraryPart_Get (ACAPinc.h:2383, use Plan_Dump.cpp:517-540).
// ---------------------------------------------------------------------------
// libInd -> the library part's document name ("Door 21", "Chair 01"), or empty if the
// index resolves to nothing. This is the ONE field that makes an object record readable:
// without it every object/lamp is an anonymous point. Follows Plan_Dump.cpp:517-540 —
// index in, everything else filled by Archicad, and `location` is allocated by the call
// and OURS to delete. APIERR_MISSINGDEF still fills docu_UName (ACAPinc.h:2374), so a
// placed object whose .gsm has gone missing still reports its name rather than "".
static GS::UniString LibraryPartNameOf (Int32 libInd)
{
    if (libInd <= 0)
        return GS::UniString ();

    API_LibPart libPart = {};
    libPart.index = libInd;
    const GSErrCode err = ACAPI_LibraryPart_Get (&libPart);
    delete libPart.location;
    libPart.location = nullptr;

    if (err != NoError && err != APIERR_MISSINGDEF)
        return GS::UniString ();
    return GS::UniString (libPart.docu_UName);
}

class GetElementDetailsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetElementDetails"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::ObjectState> elements;
        if (!params.Get ("elements", elements)) {
            return NativeCommandResult::Failure ("need elements=[{elementId:{guid}}]");
        }

        GS::Array<GS::ObjectState> records;

        // A guid that does not resolve, or resolves to a type this command does not
        // speak. Still a RECORD, so the list stays positionally aligned to the input;
        // `details` is present but empty so a caller can index it without a branch.
        //
        // `reason` + `typeName` are what make a BULK read actionable. On a whole-floor
        // selection the misses ARE the interesting part — "which types does this command
        // still not speak?" — and a bare guid list cannot answer it (§7.12: the first
        // run of this probe reported three anonymous misses that turned out to be a
        // circle, a spline and a line). `typeName` is the LOCALIZED name from
        // ACAPI_Element_GetElemTypeName (ACAPI_Goodies.h:1278, pattern from
        // APICommon.c:370), so `typeId` carries the stable machine-readable value too.
        //
        // `elementId` — the Element Settings "ID" box, the API's compound info
        // string — rides on EVERY record, hit or miss, because it is the one
        // field that is type-agnostic: it comes from
        // ACAPI_Element_GetElementInfoString, not from any element struct, so it
        // is readable for the types this command does not speak. That is what
        // lets a caller number or key a whole mixed selection off ONE read.
        // Empty when the type has no ID field (APIERR_BADELEMENTTYPE); the
        // read/write pair for it in bulk is EvP.GetElementIds/SetElementIds
        // (NativeCommands/IdentityCommands.cpp).
        auto elementIdOf = [] (const API_Guid& guid) {
            GS::UniString infoString;
            if (ACAPI_Element_GetElementInfoString (&guid, &infoString) != NoError)
                return GS::UniString ();
            return infoString;
        };

        auto missRecord = [&elementIdOf] (const GS::UniString& guid, const char* reason,
                                          const API_ElemType* type) {
            GS::UniString typeName;
            if (type != nullptr)
                ACAPI_Element_GetElemTypeName (*type, typeName);

            GS::ObjectState rec;
            GS::ObjectState elementId;
            elementId.Add ("guid", guid);
            rec.Add ("elementId", elementId);
            rec.Add ("found",     false);
            rec.Add ("kind",      GS::UniString ());
            rec.Add ("floorInd",  (GS::Int32) 0);
            rec.Add ("reason",    GS::UniString (reason));
            rec.Add ("typeName",  typeName);
            rec.Add ("typeId",    (GS::Int32) (type != nullptr ? (int) type->typeID : 0));
            rec.Add ("value",     elementIdOf (APIGuidFromString (guid.ToCStr ().Get ())));
            rec.Add ("details",   GS::ObjectState ());
            return rec;
        };

        // Outer contour + holes into `d`, shared by the polygon kinds. `wantZ` is the
        // mesh's opt-in: it adds a `polygonZ` next to `polygonArcs` on the contour and on
        // every hole, and is left off for slab/roof so their records stay byte-identical.
        auto addPolygon = [] (const API_Guid& guid, GS::ObjectState& d, bool wantZ = false) {
            GS::Array<GS::ObjectState> outline, holes;
            GS::Array<double>          outlineArcs, outlineZ;
            ReadRingsNested (guid, outline, outlineArcs, holes,
                             /*polylineMode*/ false, /*outerClosed*/ nullptr,
                             wantZ ? &outlineZ : nullptr);
            d.Add ("polygonOutline", outline);
            d.Add ("polygonArcs",    outlineArcs);
            if (wantZ)
                d.Add ("polygonZ",   outlineZ);
            d.Add ("holes",          holes);
            d.Add ("hasHoles",       !holes.IsEmpty ());
        };

        for (const GS::ObjectState& item : elements) {
            GS::ObjectState elementId;
            GS::UniString guidString;
            if (!item.Get ("elementId", elementId) || !elementId.Get ("guid", guidString) || guidString.IsEmpty ()) {
                return NativeCommandResult::Failure ("every element needs elementId.guid");
            }
            API_Element element = {};
            element.header.guid = APIGuidFromString (guidString.ToCStr ().Get ());

            if (ACAPI_Element_Get (&element) != NoError) {
                records.Push (missRecord (guidString, "notFound", nullptr));
                continue;
            }

            const API_ElemTypeID typeId = element.header.type.typeID;

            const char* kind = nullptr;
            switch (typeId) {
                case API_SlabID:   kind = "slab";   break;
                case API_RoofID:   kind = "roof";   break;
                case API_MeshID:   kind = "mesh";   break;
                case API_WallID:   kind = "wall";   break;
                case API_BeamID:   kind = "beam";   break;
                case API_ColumnID: kind = "column"; break;
                case API_PolyLineID: kind = "polyline"; break;
                case API_ObjectID: kind = "object"; break;
                case API_LampID:   kind = "lamp";   break;
                // The Fill TOOL's element. ⚠️ Its type is API_HatchID — there is no
                // API_FillID in the AC29 headers at all (grep APIdefs_Elements.h:129).
                // The kind is spelled "fill" because that is what the tool is called
                // in the UI and what a command author will look for.
                case API_HatchID:  kind = "fill";   break;
                default: break;
            }
            if (kind == nullptr) {
                records.Push (missRecord (guidString, "unsupportedType", &element.header.type));
                continue;
            }

            GS::ObjectState d;

            if (typeId == API_SlabID) {
                d.Add ("thickness", element.slab.thickness);
                d.Add ("level",     element.slab.level);
                addPolygon (element.header.guid, d);

            } else if (typeId == API_RoofID) {
                d.Add ("thickness", element.roof.shellBase.thickness);
                d.Add ("level",     element.roof.shellBase.level);
                // Slope = the plane's pitch. Only a single-plane roof has ONE angle;
                // reading u.planeRoof on a multi-plane roof would read the wrong union
                // member, so poly-roofs report 0 (pitch is per-plane there).
                d.Add ("slantAngle", element.roof.roofClass == API_PlaneRoofID
                                     ? element.roof.u.planeRoof.angle : 0.0);
                addPolygon (element.header.guid, d);
                AddRoofClassData (element, d);   // E15 — pivot line / pivot polygon

            } else if (typeId == API_MeshID) {
                // A mesh is a polygon whose vertices each carry an elevation, plus interior
                // level lines. `level` is the base plane (from the story level, same frame
                // as slab/roof `level`); `skirtLevel` is how far the solid drops below it,
                // which is the nearest thing a mesh has to a thickness.
                d.Add ("level",      element.mesh.level);
                d.Add ("skirtLevel", element.mesh.skirtLevel);
                // Tapir's MeshDetails spelling, so the enum reads the same through either
                // add-on. The numbers are the header's ("Skirt: 1 = yes; 2 = no bottom;
                // 3 = no"); anything unexpected falls to the solid case, as Tapir does.
                d.Add ("skirtType",  GS::UniString (element.mesh.skirt == 3 ? "SurfaceOnlyWithoutSkirt"
                                                  : element.mesh.skirt == 2 ? "WithSkirt"
                                                  : "SolidBodyWithSkirt"));
                d.Add ("ridges",     GS::UniString (element.mesh.smoothRidges == APIRidge_AllSharp  ? "AllSharp"
                                                  : element.mesh.smoothRidges == APIRidge_AllSmooth ? "AllSmooth"
                                                  : "UserDefined"));
                addPolygon (element.header.guid, d, /*wantZ*/ true);
                AddMeshSublines (element, d);

            } else if (typeId == API_HatchID) {
                // A Fill is a 2D polygon and nothing else — no thickness, no level,
                // no elevation. It reads through `addPolygon` exactly as a slab does
                // (API_HatchType carries an API_Polygon `poly`), so one piece of
                // caller code walks a slab contour and a fill outline.
                //
                // NO `closed` flag, deliberately: a fill is closed by definition.
                // The polyline branch below needs one because a polyline may be an
                // open chain; asking the same question of a fill has no meaning.
                //
                // The pens come along because they are what a drawing uses to say
                // what a fill MEANS — the same join-key role `pen` plays on the
                // polyline below.
                d.Add ("pen",       (GS::Int32) element.hatch.contPen.penIndex);
                d.Add ("fillPen",   (GS::Int32) element.hatch.fillPen.penIndex);
                // fillBGPen is a bare `short`, not an API_ExtendedPenType like the
                // other two — the struct is not uniform here.
                d.Add ("fillBGPen", (GS::Int32) element.hatch.fillBGPen);
                addPolygon (element.header.guid, d);

            } else if (typeId == API_PolyLineID) {
                // Same spelling as the polygon kinds (`polygonOutline`/`polygonArcs`), so
                // one piece of caller code reads a slab contour and a polyline chain. But
                // it is a CHAIN, not a footprint: `closed` says whether the last vertex
                // joins back to the first, and only then is an area meaningful. No
                // `holes` — a polyline has exactly one contour.
                GS::Array<GS::ObjectState> outline, holes;
                GS::Array<double>          outlineArcs;
                bool                       closed = false;
                ReadRingsNested (element.header.guid, outline, outlineArcs, holes,
                                 /*polylineMode*/ true, &closed);
                d.Add ("polygonOutline", outline);
                d.Add ("polygonArcs",    outlineArcs);
                d.Add ("closed",         closed);
                // The pen is the JOIN KEY for a survey drawing: a breakline and
                // the spot-height label that gives it its elevation are drawn in
                // the same colour, and that is the only thing relating them.
                d.Add ("pen",            (GS::Int32) element.polyLine.linePen.penIndex);

            } else if (typeId == API_ObjectID || typeId == API_LampID) {
                // API_LampType IS API_ObjectType (APIdefs_Elements.h:5713), so one branch
                // serves both and only `kind` distinguishes them.
                const double z = element.object.level;
                d.Add ("level",           z);
                d.Add ("planAngle",       element.object.angle);
                d.Add ("xRatio",          element.object.xRatio);
                d.Add ("yRatio",          element.object.yRatio);
                d.Add ("reflected",       element.object.reflected);
                d.Add ("libraryPartName", LibraryPartNameOf (element.object.libInd));
                // An object is a POINT, so begin == end == origin — exactly as a column.
                d.Add ("begCoordinate", Coord3D (element.object.pos.x, element.object.pos.y, z));
                d.Add ("endCoordinate", Coord3D (element.object.pos.x, element.object.pos.y, z));

            } else if (typeId == API_WallID) {
                const double z = element.wall.bottomOffset;
                d.Add ("thickness",  element.wall.thickness);
                d.Add ("height",     element.wall.height);
                d.Add ("level",      z);
                d.Add ("slantAngle", element.wall.slantAlpha);   // pi/2 for a plumb wall
                d.Add ("begCoordinate", Coord3D (element.wall.begC.x, element.wall.begC.y, z));
                d.Add ("endCoordinate", Coord3D (element.wall.endC.x, element.wall.endC.y, z));

            } else if (typeId == API_BeamID) {
                const double z = element.beam.level;
                double w = 0.0, h = 0.0;
                ReadFirstSegmentSection (element.header.guid, /*beam*/ true, w, h);
                d.Add ("level",         z);
                d.Add ("planAngle",     element.beam.curveAngle);
                d.Add ("slantAngle",    element.beam.slantAngle);   // from horizontal; 0 = level
                d.Add ("isSlanted",     element.beam.isSlanted);
                d.Add ("nSegments",     (GS::Int32) element.beam.nSegments);
                d.Add ("sectionWidth",  w);
                d.Add ("sectionHeight", h);
                d.Add ("begCoordinate", Coord3D (element.beam.begC.x, element.beam.begC.y, z));
                d.Add ("endCoordinate", Coord3D (element.beam.endC.x, element.beam.endC.y, z));

            } else {   // API_ColumnID
                const double z = element.column.bottomOffset;
                double w = 0.0, h = 0.0;
                ReadFirstSegmentSection (element.header.guid, /*beam*/ false, w, h);
                d.Add ("height",        element.column.height);
                d.Add ("level",         z);
                d.Add ("planAngle",     element.column.axisRotationAngle);
                d.Add ("slantAngle",    element.column.slantAngle);  // pi/2 = plumb
                d.Add ("isSlanted",     element.column.isSlanted);
                d.Add ("nSegments",     (GS::Int32) element.column.nSegments);
                d.Add ("sectionWidth",  w);
                d.Add ("sectionHeight", h);
                // A column is a point: begin == end == origin.
                d.Add ("begCoordinate", Coord3D (element.column.origoPos.x, element.column.origoPos.y, z));
                d.Add ("endCoordinate", Coord3D (element.column.origoPos.x, element.column.origoPos.y, z));
            }

            GS::ObjectState rec;
            rec.Add ("elementId", elementId);
            rec.Add ("found",     true);
            rec.Add ("kind",      GS::UniString (kind));
            rec.Add ("floorInd",  (GS::Int32) element.header.floorInd);
            rec.Add ("value",     elementIdOf (element.header.guid));
            rec.Add ("details",   d);
            records.Push (rec);
        }

        os.Add ("detailsOfElements", records);
        os.Add ("count", (GS::Int32) records.GetSize ());
        return os;
    }
};
// ---------------------------------------------------------------------------
// Tapioca.GetLibraryPartInfo { libraryPartNames:[...] } -> which one exists + its size.
//
// A placement script needs the part's OWN default size BEFORE it computes where to
// put anything: the slope symbol is drawn from its origin, so centring it on a
// point (or offsetting it along a fall line) means subtracting half its length.
// Without this the script would have to hard-code a guess that breaks the moment
// the library or the object's default size changes.
// ---------------------------------------------------------------------------
class GetLibraryPartInfoCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetLibraryPartInfo"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        GS::Array<GS::UniString> candidates;
        if (!params.Get ("libraryPartNames", candidates) || candidates.IsEmpty ()) {
            return NativeCommandResult::Failure ("need libraryPartNames=[...] (candidates, tried in order)");
        }
        GS::UniString triedList;
        for (const GS::UniString& name : candidates) {
            API_LibPart candidate = {};
            GS::ucscpy (candidate.docu_UName, name.ToUStr ());
            const GSErrCode searchErr = ACAPI_LibraryPart_Search (&candidate, false, true);
            delete candidate.location;
            if (!triedList.IsEmpty ())
                triedList += ", ";
            triedList += "\"" + name + "\"";
            if (searchErr != NoError)
                continue;
            double a = 0.0, b = 0.0;
            Int32  addParNum = 0;
            API_AddParType** libParams = nullptr;
            const GSErrCode paramErr =
                ACAPI_LibraryPart_GetParams (candidate.index, &a, &b, &addParNum, &libParams);
            if (paramErr == NoError)
                ACAPI_DisposeAddParHdl (&libParams);   // only the sizes were wanted

            os.Add ("libraryPartName", name);
            os.Add ("libInd", (GS::Int32) candidate.index);
            os.Add ("sizeA", a);      // the part's own default X size ("length")
            os.Add ("sizeB", b);      // ... and Y size
            os.Add ("paramCount", (GS::Int32) addParNum);
            return os;
        }

        return NativeCommandResult::Failure (GS::UniString ("No library part found. Tried: " + triedList +
                                             ". Use the name shown in the Object tool's settings dialog."));
    }
};

// ---------------------------------------------------------------------------
// Tapioca.FindPlacedObjects { libraryPartNames:[...] }
//   -> elements:[{elementId:{guid}}] already using that part.
//
// Finds existing Objects or Lamps so new placements can inherit their style.
// ---------------------------------------------------------------------------
class FindPlacedObjectsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "FindPlacedObjects"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::UniString> candidates;
        if (!params.Get ("libraryPartNames", candidates) || candidates.IsEmpty ()) {
            return NativeCommandResult::Failure ("need libraryPartNames=[...]");
        }

        GS::Array<GS::Int32> wanted;
        GS::UniString        resolvedName;
        for (const GS::UniString& name : candidates) {
            API_LibPart candidate = {};
            GS::ucscpy (candidate.docu_UName, name.ToUStr ());
            if (ACAPI_LibraryPart_Search (&candidate, false, true) == NoError) {
                wanted.Push ((GS::Int32) candidate.index);
                if (resolvedName.IsEmpty ())
                    resolvedName = name;
            }
            delete candidate.location;
        }
        if (wanted.IsEmpty ()) {
            os.Add ("elements", GS::Array<GS::ObjectState> ());
            os.Add ("count", (GS::Int32) 0);
            os.Add ("note", GS::UniString ("no such library part is loaded"));
            return os;
        }

        GS::Array<GS::ObjectState> found;
        GS::Array<API_Guid> objects, lamps;
        if (const GSErrCode listErr = ACAPI_Element_GetElemList (API_ObjectID, &objects); listErr != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_GetElemList", listErr, "API_ObjectID (listing placed library objects)"));
        }
        if (const GSErrCode listErr = ACAPI_Element_GetElemList (API_LampID, &lamps); listErr != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_GetElemList", listErr, "API_LampID (listing placed lamps)"));
        }
        for (const API_Guid& guid : lamps) objects.Push (guid);

        for (const API_Guid& guid : objects) {
            API_Element element = {};
            element.header.guid = guid;
            if (ACAPI_Element_Get (&element) != NoError)
                continue;
            for (GS::Int32 index : wanted) {
                if ((GS::Int32) element.object.libInd == index) {
                    GS::ObjectState elementId, record;
                    elementId.Add ("guid", GS::UniString (APIGuidToString (guid).ToCStr ()));
                    record.Add ("elementId", elementId);
                    found.Push (record);
                    break;
                }
            }
        }

        os.Add ("elements", found);
        os.Add ("count", (GS::Int32) found.GetSize ());
        os.Add ("libraryPartName", resolvedName);
        return os;
    }
};

const NativeCommandRegistration kElementReadCommandRegistrations[] = {
    { "GetElementInfo", &MakeRegisteredNativeCommand<GetElementInfoCommand>, false,
      R"json({"type":"object","properties":{"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}}},"additionalProperties":false,"required":["elements"]})json",
      R"json({"type":"object","properties":{"infoOfElements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"found":{"type":"boolean"},"type":{"type":"string"},"floorInd":{"type":"integer"},"angle":{"type":"number"}},"additionalProperties":false,"required":["elementId","found","type","floorInd","angle"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["infoOfElements","count"]})json" },
    { "GetElementDetails",  &MakeRegisteredNativeCommand<GetElementDetailsCommand>, false,
      R"json({"type":"object","properties":{"elements":{"$ref":"#Elements"}},"additionalProperties":false,"required":["elements"]})json",
      R"json({"oneOf":[{"type":"object","properties":{"detailsOfElements":{"type":"array","items":{"type":"object","properties":{"elementId":{"$ref":"#ElementId"},"found":{"type":"boolean"},"kind":{"type":"string","enum":["","slab","roof","mesh","wall","beam","column","polyline","object","lamp","fill"]},"floorInd":{"type":"integer"},"reason":{"type":"string","enum":["notFound","unsupportedType"]},"typeName":{"type":"string"},"typeId":{"type":"integer"},"value":{"type":"string"},"details":{"type":"object","properties":{"thickness":{"type":"number"},"height":{"type":"number"},"level":{"type":"number"},"slantAngle":{"type":"number"},"planAngle":{"type":"number"},"isSlanted":{"type":"boolean"},"nSegments":{"type":"integer"},"sectionWidth":{"type":"number"},"sectionHeight":{"type":"number"},"begCoordinate":{"$ref":"#Point3D"},"endCoordinate":{"$ref":"#Point3D"},"polygonOutline":{"type":"array","items":{"$ref":"#Point2D"}},"polygonArcs":{"type":"array","items":{"type":"number"}},"polygonZ":{"type":"array","items":{"type":"number"}},"holes":{"type":"array","items":{"type":"object","properties":{"polygonOutline":{"type":"array","items":{"$ref":"#Point2D"}},"polygonArcs":{"type":"array","items":{"type":"number"}},"polygonZ":{"type":"array","items":{"type":"number"}}},"additionalProperties":false,"required":["polygonOutline","polygonArcs"]}},"hasHoles":{"type":"boolean"},"closed":{"type":"boolean"},"pen":{"type":"integer"},"fillPen":{"type":"integer"},"fillBGPen":{"type":"integer"},"xRatio":{"type":"number"},"yRatio":{"type":"number"},"reflected":{"type":"boolean"},"libraryPartName":{"type":"string"},"skirtLevel":{"type":"number"},"skirtType":{"type":"string","enum":["SurfaceOnlyWithoutSkirt","WithSkirt","SolidBodyWithSkirt"]},"ridges":{"type":"string","enum":["AllSharp","AllSmooth","UserDefined"]},"nSubLines":{"type":"integer"},"nLevelCoords":{"type":"integer"},"levelEnds":{"type":"array","items":{"type":"integer"}},"sublines":{"type":"array","items":{"type":"object","properties":{"coordinates":{"type":"array","items":{"$ref":"#Point3D"}},"vertexIds":{"type":"array","items":{"type":"integer"}}},"additionalProperties":false,"required":["coordinates","vertexIds"]}},"roofClass":{"type":"string","enum":["plane","poly"]},"baseLine":{"type":"object","properties":{"begCoordinate":{"$ref":"#Point2D"},"endCoordinate":{"$ref":"#Point2D"}},"additionalProperties":false,"required":["begCoordinate","endCoordinate"]},"posSign":{"type":"boolean"},"pivotOutline":{"type":"array","items":{"$ref":"#Point2D"}},"pivotArcs":{"type":"array","items":{"type":"number"}},"levels":{"type":"array","items":{"type":"object","properties":{"levelAngle":{"type":"number"},"levelHeight":{"type":"number"}},"additionalProperties":false,"required":["levelAngle","levelHeight"]}},"levelNum":{"type":"integer"},"eavesOverHang":{"type":"number"},"overHangType":{"type":"integer"}},"additionalProperties":false}},"additionalProperties":false,"required":["elementId","found","kind","floorInd","value","details"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["detailsOfElements","count"]},{"type":"object","properties":{"ok":{"const":false},"error":{"type":"string"}},"additionalProperties":false,"required":["ok","error"]}]})json" },
    { "GetLibraryPartInfo", &MakeRegisteredNativeCommand<GetLibraryPartInfoCommand>, false,
      R"json({"type":"object","properties":{"libraryPartNames":{"type":"array","minItems":1,"items":{"type":"string","minLength":1}}},"additionalProperties":false,"required":["libraryPartNames"]})json",
      R"json({"type":"object","properties":{"libraryPartName":{"type":"string"},"libInd":{"type":"integer"},"sizeA":{"type":"number"},"sizeB":{"type":"number"},"paramCount":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["libraryPartName","libInd","sizeA","sizeB","paramCount"]})json" },
    { "FindPlacedObjects", &MakeRegisteredNativeCommand<FindPlacedObjectsCommand>, false,
      R"json({"type":"object","properties":{"libraryPartNames":{"type":"array","minItems":1,"items":{"type":"string","minLength":1}}},"additionalProperties":false,"required":["libraryPartNames"]})json",
      R"json({"type":"object","properties":{"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}},"count":{"type":"integer","minimum":0},"libraryPartName":{"type":"string"},"note":{"type":"string"}},"additionalProperties":false,"required":["elements","count"]})json" }
};

}   // namespace

NativeCommandRegistrations GetElementReadCommandRegistrations () { return MakeRegistrationView (kElementReadCommandRegistrations); }

} // namespace geomsrv
