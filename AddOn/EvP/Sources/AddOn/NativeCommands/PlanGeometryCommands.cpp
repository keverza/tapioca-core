#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/PlanGeometryCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp"   // WalkPolygonRings -- the one polygon walk

#include "ArchViz/DiligentViewport.hpp"      // SetPlanAnchors -- the drawing half

#include <vector>

namespace geomsrv {

namespace {

GS::ObjectState Coord2D (double x, double y)
{
    GS::ObjectState c;
    c.Add ("x", x);
    c.Add ("y", y);
    return c;
}

// One ring out of WalkPolygonRings' flat {x,y,x,y,…} + one-arc-angle-per-vertex
// output, from vertex `firstVertex` for `vertexCount` vertices.
void FillRing (const GS::Array<double>& flatCoords, const GS::Array<double>& arcs,
               USize firstVertex, USize vertexCount,
               GS::Array<GS::ObjectState>& outline, GS::Array<double>& outlineArcs)
{
    for (USize v = 0; v < vertexCount; ++v) {
        const USize i = firstVertex + v;
        outline.Push (Coord2D (flatCoords[i * 2], flatCoords[i * 2 + 1]));
        outlineArcs.Push (arcs[i]);
    }
}

// A hole ring carries the SAME two keys as the contour, deliberately, so one
// piece of caller code reads both.
GS::ObjectState RingRecord (const GS::Array<double>& flatCoords, const GS::Array<double>& arcs,
                            USize firstVertex, USize vertexCount)
{
    GS::Array<GS::ObjectState> outline;
    GS::Array<double>          outlineArcs;
    FillRing (flatCoords, arcs, firstVertex, vertexCount, outline, outlineArcs);

    GS::ObjectState ring;
    ring.Add ("outline",     outline);
    ring.Add ("outlineArcs", outlineArcs);
    return ring;
}

// One wall's CONNECTION polygon, in the flat form WalkPolygonRings produces.
//
// Kept raw rather than pre-formatted because it has two consumers that want
// different things from it: GetWallPlanOutlines turns it into wire records, and
// SetPlanAnchors hands the same coordinates straight to the viewer. Formatting
// it here would make the viewer parse a JSON shape it never needed.
struct WallConnectionPolygon {
    bool                 ok = false;
    GSErrCode            err = NoError;
    GS::Array<double>    flatOuter;      // x,y,x,y… world metres, no closing repeat
    GS::Array<double>    outerArcs;      // one signed angle per vertex, 0 = straight
    GS::Array<double>    flatHoles;
    GS::Array<double>    holeArcs;
    GS::Array<GS::Int32> holeCounts;
    GS::Int32            nHoles = 0;
    GS::Int32            conBeg = 0, conEnd = 0, conRef = 0, con = 0, conX = 0;
};

// ⚠️ ROUTE B, AND THE ONLY PLACE IT IS CALLED. See the command comment below for
// why the connection polygon rather than the element's own.
WallConnectionPolygon ReadWallConnectionPolygon (const API_Guid& guid,
                                                 const GS::UniString& guidString)
{
    WallConnectionPolygon result;

    API_WallRelation wallInfo = {};
    result.err = ACAPI_Element_GetRelations (guid, API_ZombieElemID, &wallInfo);
    if (result.err != NoError) {
        EVP_ACAPI_FAIL ("ACAPI_Element_GetRelations", result.err,
                        GS::UniString ("reading the connection polygon of wall ") + guidString);
        return result;
    }

    GS::Int32 outerCount = 0;
    PolygonHandles polygon;
    polygon.coords = wallInfo.coords;
    polygon.pends  = wallInfo.pends;
    polygon.parcs  = wallInfo.parcs;

    result.ok = WalkPolygonRings (polygon, nullptr,
                                  result.flatOuter, result.outerArcs, outerCount,
                                  result.flatHoles, result.holeArcs, result.holeCounts,
                                  result.nHoles);

    result.conBeg = wallInfo.nConBeg;
    result.conEnd = wallInfo.nConEnd;
    result.conRef = wallInfo.nConRef;
    result.con    = wallInfo.nCon;
    result.conX   = wallInfo.nConX;

    // ⚠️ THE HANDLES ARE OURS TO FREE AND THIS IS THE SANCTIONED WAY. ACAPinc.h
    // calls it the recommended disposal "for compatibility reasons, as in a
    // later version the API_WallRelation structure might change" -- so never
    // BMKillHandle these by hand.
    ACAPI_DisposeWallRelationHdls (&wallInfo);
    return result;
}

// Wall shape as a script reads it. Not decoration: it is the one field that says
// whether the memo polygon below was ever expected to exist.
const char* WallShapeName (API_WallTypeID type)
{
    switch (type) {
        case APIWtyp_Normal: return "straight";
        case APIWtyp_Trapez: return "trapezoid";
        case APIWtyp_Poly:   return "polygon";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Tapioca.GetWallPlanOutlines { elements:[{elementId:{guid}}] }
//   -> { count, outlines:[{ elementId, succeeded, error?, wallShape,
//                           outline, outlineArcs, holes, outlineSource,
//                           memoPolygonPresent, memoOutline, memoOutlineArcs,
//                           connectedWalls{…} }] }
//
// A wall's outline AS THE FLOOR PLAN DRAWS IT — which is not its own polygon.
// Archicad TRIMS a wall where it meets other walls, and the trimmed result is
// the CONNECTION POLYGON, reachable only through ACAPI_Element_GetRelations
// (verified against the devkit's own Element_Snippets Do_DumpWall, which walks
// exactly these handles). That is `outline`, and it is the one to draw.
//
// ⚠️ TWO ROUTES EXIST AND THEY ANSWER DIFFERENT QUESTIONS. The element's own
// memo polygon (APIMemoMask_Polygon) is the UNTRIMMED outline, before junctions.
// It is reported alongside as `memoOutline` rather than instead, because the
// difference between the two IS the connection geometry and a caller comparing
// them can see a junction it would otherwise have to infer.
//
// ⚠️ THE MEMO POLYGON IS REPORTED AS PRESENT-OR-NOT RATHER THAN ASSUMED.
// ACAPinc.h says coords/pends/parcs are "required only for walls of type
// APIWtyp_Poly", which reads as "a straight wall has none" — but MEASURED
// 2026-08-12, six straight walls out of six returned a 4-vertex rectangle. That
// remark governs what a CREATE/CHANGE must SUPPLY, not what a GET returns. The
// field stays because the measurement covered straight walls only, and because
// the connection polygon is the one guaranteed to exist for every wall.
//
// The batch is ONE gate hop for every element (CLAUDE.md: never call the gate
// per element). A guid that does not resolve, or resolves to something that is
// not a wall, still emits a positionally-aligned record with succeeded=false
// and a reason in `error`, so a caller can index the list blindly.
// ---------------------------------------------------------------------------
class GetWallPlanOutlinesCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetWallPlanOutlines"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> elements;
        params.Get ("elements", elements);   // the schema already requires this field

        GS::Array<GS::ObjectState> outlines;

        auto missRecord = [] (const GS::UniString& guidString, const GS::UniString& why) {
            GS::ObjectState elementId;
            elementId.Add ("guid", guidString);

            GS::ObjectState rec;
            rec.Add ("elementId", elementId);
            rec.Add ("succeeded", false);
            rec.Add ("error",     why);
            return rec;
        };

        for (const GS::ObjectState& item : elements) {
            GS::ObjectState elementIdIn;
            GS::UniString   guidString;
            if (!item.Get ("elementId", elementIdIn) || !elementIdIn.Get ("guid", guidString)
                || guidString.IsEmpty ()) {
                return NativeCommandResult::Failure ("every element needs elementId.guid");
            }

            API_Element wallElement = {};
            wallElement.header.guid = APIGuidFromString (guidString.ToCStr ().Get ());

            if (const GSErrCode err = ACAPI_Element_Get (&wallElement); err != NoError) {
                outlines.Push (missRecord (guidString,
                    EVP_ACAPI_FAIL ("ACAPI_Element_Get", err,
                                    GS::UniString ("reading wall ") + guidString
                                    + " for its plan outline")));
                continue;
            }
            if (wallElement.header.type.typeID != API_WallID) {
                GS::UniString typeName;
                ACAPI_Element_GetElemTypeName (wallElement.header.type, typeName);
                outlines.Push (missRecord (guidString,
                    EVP_FAIL (GS::UniString ("not a wall: ") + typeName,
                              "reading a wall's plan outline")));
                continue;
            }

            GS::ObjectState elementId;
            elementId.Add ("guid", guidString);

            GS::ObjectState rec;
            rec.Add ("elementId", elementId);
            rec.Add ("succeeded", true);
            rec.Add ("wallShape", GS::UniString (WallShapeName (wallElement.wall.type)));

            AddConnectionPolygon (wallElement.header.guid, guidString, rec);
            AddMemoPolygon (wallElement.header.guid, rec);

            outlines.Push (rec);
        }

        GS::ObjectState os;
        os.Add ("outlines", outlines);
        os.Add ("count", (GS::Int32) outlines.GetSize ());
        return os;
    }

private:
    // Route B, formatted for the wire. The READ is ReadWallConnectionPolygon
    // above; this only turns its arrays into records. The connection counts ride
    // along because they come out of the SAME call -- asking twice would trim
    // twice -- and because they are what makes a surprising outline explainable
    // instead of merely wrong.
    static void AddConnectionPolygon (const API_Guid& guid, const GS::UniString& guidString,
                                      GS::ObjectState& rec)
    {
        const WallConnectionPolygon polygon = ReadWallConnectionPolygon (guid, guidString);

        GS::Array<GS::ObjectState> outline, holes;
        GS::Array<double>          outlineArcs;
        GS::UniString              source ("none");

        if (polygon.ok) {
            FillRing (polygon.flatOuter, polygon.outerArcs, 0,
                      polygon.flatOuter.GetSize () / 2, outline, outlineArcs);

            USize cursor = 0;
            for (GS::Int32 h = 0; h < polygon.nHoles; ++h) {
                const USize count = (USize) polygon.holeCounts[h];
                holes.Push (RingRecord (polygon.flatHoles, polygon.holeArcs, cursor, count));
                cursor += count;
            }
            source = "connectionPolygon";
        }

        GS::ObjectState connected;
        connected.Add ("atBegin",         (GS::Int32) polygon.conBeg);
        connected.Add ("atEnd",           (GS::Int32) polygon.conEnd);
        connected.Add ("toReferenceLine", (GS::Int32) polygon.conRef);
        connected.Add ("onReferenceLine", (GS::Int32) polygon.con);
        connected.Add ("crossing",        (GS::Int32) polygon.conX);
        rec.Add ("connectedWalls", connected);

        rec.Add ("outline",       outline);
        rec.Add ("outlineArcs",   outlineArcs);
        rec.Add ("holes",         holes);
        rec.Add ("outlineSource", source);
    }

    // Route A — the wall's own untrimmed polygon, when it has one.
    static void AddMemoPolygon (const API_Guid& guid, GS::ObjectState& rec)
    {
        GS::Array<GS::ObjectState> memoOutline;
        GS::Array<double>          memoOutlineArcs;
        bool                       present = false;

        API_ElementMemo memo = {};
        if (ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_Polygon) == NoError) {
            GS::Array<double>    flatOuter, flatOuterArcs, flatHole, flatHoleArcs;
            GS::Array<GS::Int32> holeCounts;
            GS::Int32            outerCount = 0, nHoles = 0;

            PolygonHandles polygon;
            polygon.coords = memo.coords;
            polygon.pends  = memo.pends;
            polygon.parcs  = memo.parcs;

            if (WalkPolygonRings (polygon, nullptr,
                                  flatOuter, flatOuterArcs, outerCount,
                                  flatHole, flatHoleArcs, holeCounts, nHoles)) {
                FillRing (flatOuter, flatOuterArcs, 0, (USize) outerCount,
                          memoOutline, memoOutlineArcs);
                present = true;
            }
        }
        ACAPI_DisposeElemMemoHdls (&memo);

        rec.Add ("memoPolygonPresent", present);
        rec.Add ("memoOutline",        memoOutline);
        rec.Add ("memoOutlineArcs",    memoOutlineArcs);
    }
};

// ---------------------------------------------------------------------------
// Tapioca.SetPlanAnchors { elements, enabled, widthPixels?, color?, arcSign?,
//                          planZ? }
//   -> { count, rings, vertices, accepted }
//
// Hands the walls' plan outlines to the viewer, which draws them over the floor
// plan as ANCHORS: lines whose only job is to be compared against the lines
// Archicad itself drew. If ours sit exactly on Archicad's, the analysis layer
// that will be drawn on top of them is in the right place; if they do not, the
// run says so before anyone trusts a heatmap.
//
// ⚠️ IT LIVES IN THE PLAN GEOMETRY DOMAIN, NOT THE ARCHVIZ ONE, and the reason
// is which half is the substance. Handing a vertex list to a running viewport
// is three lines; READING Archicad's 2D plan geometry is the work, and it is
// the same read GetWallPlanOutlines does. Putting this next to the viewer verbs
// would mean either a second connection-polygon reader or a cross-domain
// dependency to reach this one.
//
// ⚠️ THE ANCHORS ARE AN INSTRUMENT, NOT CONTENT. Nothing here may derive an
// outline from the 3D model: that is the "stale 3D snapshot on the plan"
// approach the whole feature exists to replace (PLAT-RE65).
//
// ⚠️ `arcSign` EXISTS BECAUSE THE REPO CONTRADICTS ITSELF ABOUT ARC DIRECTION.
// CommandUtils' polygon walk says a positive arcAngle bulges to the RIGHT of
// the begIndex->endIndex direction; evp.elements.polygon_area adds a positive
// arc's segment area to a counterclockwise shoelace, which is a bulge LEFT.
// Both cannot be right, a curved wall drawn the wrong way is a plausible
// picture, and no header settles it. So it is a knob the user turns until a
// curved wall's anchor lands on Archicad's own arc — exactly how the sun
// override settled the azimuth convention. Straight walls are unaffected.
//
// `planZ` is the height the flat ribbon sits at. It does not matter to a
// top-down orthographic camera, and it is exposed because it does matter to a
// perspective one: the anchors are drawn with the depth test OFF, so a wrong
// planZ cannot hide them, but it can put them behind the eye.
// ---------------------------------------------------------------------------
class SetPlanAnchorsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "SetPlanAnchors"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> elements;
        params.Get ("elements", elements);   // the schema already requires this field

        bool enabled = true;
        params.Get ("enabled", enabled);

        double widthPixels = 2.0;
        params.Get ("widthPixels", widthPixels);
        double arcSign = 1.0;
        params.Get ("arcSign", arcSign);
        double planZ = 0.0;
        params.Get ("planZ", planZ);

        GS::UniString colorString ("FF3B30C0");
        params.Get ("color", colorString);
        uint32_t rgba = 0xFF3B30C0u;
        {
            // A hex string rather than four numbers: it is what a script writes
            // by hand and what a log line can be compared against by eye.
            const GS::UniString upper = colorString.ToUpperCase ();
            uint32_t parsed = 0;
            bool valid = upper.GetLength () == 8;
            for (UIndex i = 0; valid && i < upper.GetLength (); ++i) {
                const GS::UniChar::Layout c = upper[i];
                const int digit = (c >= '0' && c <= '9') ? int (c - '0')
                                : (c >= 'A' && c <= 'F') ? int (c - 'A') + 10
                                                         : -1;
                if (digit < 0)
                    valid = false;
                else
                    parsed = (parsed << 4) | uint32_t (digit);
            }
            if (!valid)
                return NativeCommandResult::Failure (
                    EVP_FAIL (GS::UniString ("color must be 8 hex digits RRGGBBAA, got \"")
                              + colorString + "\"", "setting the plan anchor colour"));
            rgba = parsed;
        }

        std::vector<std::vector<float>> rings;
        std::vector<std::vector<float>> ringArcs;
        GS::Int32 walls = 0;

        for (const GS::ObjectState& item : elements) {
            GS::ObjectState elementIdIn;
            GS::UniString   guidString;
            if (!item.Get ("elementId", elementIdIn) || !elementIdIn.Get ("guid", guidString)
                || guidString.IsEmpty ()) {
                return NativeCommandResult::Failure ("every element needs elementId.guid");
            }

            API_Element wallElement = {};
            wallElement.header.guid = APIGuidFromString (guidString.ToCStr ().Get ());
            // A guid that is not a wall is SKIPPED, not refused: the natural
            // caller is "the current selection", and refusing the whole batch
            // because one door was selected would make the feature unusable.
            // `count` vs the input length is what reports it.
            if (ACAPI_Element_Get (&wallElement) != NoError)
                continue;
            if (wallElement.header.type.typeID != API_WallID)
                continue;

            const WallConnectionPolygon polygon =
                ReadWallConnectionPolygon (wallElement.header.guid, guidString);
            if (!polygon.ok)
                continue;
            ++walls;

            auto pushRing = [&] (const GS::Array<double>& flat, const GS::Array<double>& arcs,
                                 USize first, USize count) {
                std::vector<float> xy, angles;
                xy.reserve (count * 2);
                angles.reserve (count);
                for (USize v = 0; v < count; ++v) {
                    xy.push_back (float (flat[(first + v) * 2]));
                    xy.push_back (float (flat[(first + v) * 2 + 1]));
                    angles.push_back (float (arcs[first + v]));
                }
                rings.push_back (std::move (xy));
                ringArcs.push_back (std::move (angles));
            };

            pushRing (polygon.flatOuter, polygon.outerArcs, 0, polygon.flatOuter.GetSize () / 2);

            // A wall's holes are ordinary (a niche, a recess). They are anchors
            // too: a hole drawn in the wrong place is exactly the kind of
            // register error this layer exists to catch.
            USize cursor = 0;
            for (GS::Int32 h = 0; h < polygon.nHoles; ++h) {
                const USize count = (USize) polygon.holeCounts[h];
                pushRing (polygon.flatHoles, polygon.holeArcs, cursor, count);
                cursor += count;
            }
        }

        archviz::DiligentViewport::Get ().SetPlanAnchors (
            rings, ringArcs, enabled, float (widthPixels), rgba, float (arcSign), float (planZ));

        size_t vertices = 0;
        for (const std::vector<float>& ring : rings)
            vertices += (ring.size () / 2) * 6;   // 6 vertices per closing segment

        GS::ObjectState os;
        os.Add ("count",    walls);
        os.Add ("rings",    (GS::Int32) rings.size ());
        os.Add ("vertices", (GS::Int32) vertices);
        // ⚠️ NOT "it is on screen". The viewport may not be running at all, and
        // this command deliberately does not start one: `accepted` says the
        // geometry was handed over, and DiligentViewportState's planAnchor*
        // fields say what became of it.
        os.Add ("accepted", archviz::DiligentViewport::Get ().IsRunning ());
        return os;
    }
};

constexpr const char kGetWallPlanOutlinesInput[] = R"json({
    "type":"object",
    "properties":{
        "elements":{"$ref":"#Elements"}
    },
    "additionalProperties":false,
    "required":["elements"]
})json";

// `outline`/`outlineArcs` are index-aligned: one arc angle per vertex, the signed
// angle in radians of the edge LEAVING that vertex, 0 for a straight edge. A hole
// ring repeats the same two keys so it reads like a contour.
constexpr const char kGetWallPlanOutlinesOutput[] = R"json({
    "type":"object",
    "properties":{
        "outlines":{"type":"array","items":{
            "type":"object",
            "properties":{
                "elementId":{"$ref":"#ElementId"},
                "succeeded":{"type":"boolean"},
                "error":{"type":"string"},
                "wallShape":{"type":"string","enum":["straight","trapezoid","polygon","unknown"]},
                "outline":{"type":"array","items":{"$ref":"#Point2D"}},
                "outlineArcs":{"type":"array","items":{"type":"number"}},
                "holes":{"type":"array","items":{
                    "type":"object",
                    "properties":{
                        "outline":{"type":"array","items":{"$ref":"#Point2D"}},
                        "outlineArcs":{"type":"array","items":{"type":"number"}}
                    },
                    "additionalProperties":false,
                    "required":["outline","outlineArcs"]
                }},
                "outlineSource":{"type":"string","enum":["connectionPolygon","none"]},
                "memoPolygonPresent":{"type":"boolean"},
                "memoOutline":{"type":"array","items":{"$ref":"#Point2D"}},
                "memoOutlineArcs":{"type":"array","items":{"type":"number"}},
                "connectedWalls":{
                    "type":"object",
                    "properties":{
                        "atBegin":{"type":"integer"},
                        "atEnd":{"type":"integer"},
                        "toReferenceLine":{"type":"integer"},
                        "onReferenceLine":{"type":"integer"},
                        "crossing":{"type":"integer"}
                    },
                    "additionalProperties":false,
                    "required":["atBegin","atEnd","toReferenceLine","onReferenceLine","crossing"]
                }
            },
            "additionalProperties":false,
            "required":["elementId","succeeded"]
        }},
        "count":{"type":"integer","minimum":0}
    },
    "additionalProperties":false,
    "required":["outlines","count"]
})json";

constexpr const char kSetPlanAnchorsInput[] = R"json({
    "type":"object",
    "properties":{
        "elements":{"$ref":"#Elements"},
        "enabled":{"type":"boolean"},
        "widthPixels":{"type":"number","minimum":0.5,"maximum":16},
        "color":{"type":"string","minLength":8,"maxLength":8},
        "arcSign":{"type":"number","enum":[-1,1]},
        "planZ":{"type":"number"}
    },
    "additionalProperties":false,
    "required":["elements","enabled"]
})json";

constexpr const char kSetPlanAnchorsOutput[] = R"json({
    "type":"object",
    "properties":{
        "count":{"type":"integer","minimum":0},
        "rings":{"type":"integer","minimum":0},
        "vertices":{"type":"integer","minimum":0},
        "accepted":{"type":"boolean"}
    },
    "additionalProperties":false,
    "required":["count","rings","vertices","accepted"]
})json";

const NativeCommandRegistration kPlanGeometryCommandRegistrations[] = {
    { "GetWallPlanOutlines", &MakeRegisteredNativeCommand<GetWallPlanOutlinesCommand>, false,
      kGetWallPlanOutlinesInput, kGetWallPlanOutlinesOutput },
    { "SetPlanAnchors", &MakeRegisteredNativeCommand<SetPlanAnchorsCommand>, false,
      kSetPlanAnchorsInput, kSetPlanAnchorsOutput },
};

}   // namespace

NativeCommandRegistrations GetPlanGeometryCommandRegistrations ()
{
    return MakeRegistrationView (kPlanGeometryCommandRegistrations);
}

}   // namespace geomsrv
