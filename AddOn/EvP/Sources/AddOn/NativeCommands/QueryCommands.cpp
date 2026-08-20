#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/QueryCommands.hpp"
#include "NativeCommands/CommandBase.hpp"

#include "Geometry/MeshStore.hpp"
#include "Geometry/QueryEngine.hpp"
#include "Geometry/SliceEngine.hpp"
#include "Geometry/SpatialQueries.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace geomsrv {

namespace {


// ===========================================================================
// E2 — the geometry data plane, as bus commands.
//
// These read the live snapshot and its cached BVH. Both are immutable and
// mutex-guarded, so NONE of them touch ACAPI and none needs the main thread —
// they override NeedsMainThread() to false and the dispatcher runs them inline.
// That is not a micro-optimisation: a clearance heatmap fires thousands of rays,
// and at ~3ms per gate hop the marshalled version would take minutes.
//
// Everything crosses as FLAT PARALLEL ARRAYS — the shape numpy wants on the Python
// side, and the only shape that survives these volumes. Keep it that way (§E16.0):
// nested records are for element reads, flat arrays for bulk numerics like these.
// ===========================================================================

// Shared setup: the live snapshot plus its query engine, or a populated error.
class QueryCommandBase : public MainThreadCommand {
public:
    bool NeedsMainThread () const override { return false; }

protected:
    static bool Acquire (std::shared_ptr<const Snapshot>& snap,
                         std::shared_ptr<const QueryEngine>& engine,
                         GS::UniString& error)
    {
        snap = MeshStore::Get ().Current ();
        if (snap == nullptr) {
            error = "no snapshot is live — call Tapioca.BuildSnapshot first";
            return false;
        }
        engine = QueryIndexCache::Get ().For (snap);
        if (engine == nullptr) {
            error = "the snapshot has no geometry to query";
            return false;
        }
        return true;
    }

    static bool ReadVec3 (const GS::ObjectState& params, const char* key, double out[3])
    {
        GS::Array<double> v;
        if (!params.Get (key, v) || v.GetSize () != 3)
            return false;
        out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
        return true;
    }

    static GS::UniString GuidOf (const Snapshot& snap, size_t meshIndex)
    {
        return (meshIndex < snap.meshes.size ())
             ? GS::UniString (snap.meshes[meshIndex].guid.c_str (), CC_UTF8)
             : GS::UniString ();
    }

    static GS::ObjectState ElementIdOf (const Snapshot& snap, size_t meshIndex)
    {
        GS::ObjectState elementId;
        elementId.Add ("guid", GuidOf (snap, meshIndex));
        return elementId;
    }
};

// Tapioca.Raycast { origin:[x,y,z], direction:[x,y,z], maxDist? } -> first surface hit.
class RaycastCommand : public QueryCommandBase {
public:
    GS::String GetName () const override { return "Raycast"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        std::shared_ptr<const Snapshot>    snap;
        std::shared_ptr<const QueryEngine> engine;
        GS::UniString error;
        if (!Acquire (snap, engine, error))
            return NativeCommandResult::Failure (error);

        double origin[3], direction[3];
        if (!ReadVec3 (params, "origin", origin) || !ReadVec3 (params, "direction", direction)) {
            return NativeCommandResult::Failure ("need origin=[x,y,z] and direction=[x,y,z]");
        }
        double maxDist = 0.0;
        params.Get ("maxDist", maxDist);

        const QueryEngine::RayHit hit = engine->Raycast (origin, direction, maxDist);
        os.Add ("hit", hit.hit);
        if (hit.hit) {
            os.Add ("t", hit.t);
            os.Add ("elementId", ElementIdOf (*snap, hit.meshIndex));
            os.Add ("elemType", (GS::Int32) snap->meshes[hit.meshIndex].elemType);
            os.Add ("point", GS::Array<double> { hit.point[0], hit.point[1], hit.point[2] });
            os.Add ("normal", GS::Array<double> { hit.normal[0], hit.normal[1], hit.normal[2] });
        }
        return os;
    }
};

// Tapioca.SliceZ { z, types?:[int], elements?:[{elementId:{guid}}], weld?, nudge? }
//   -> per-loop horizontal cross-section polylines at world height z.
//
// Thin wrapper over SliceEngine::SliceZ (pure C++ over the immutable snapshot,
// so gate-free like the raycasts). This is the geometry the story-slice overlay
// feature draws: each loop is one closed (or open) ring of a wall/slab/etc. at
// the cut plane. Flat parallel arrays, same convention as RaycastAll:
//   coords          flat xyz of every loop's points, concatenated
//   loopPointCounts points in each loop -> how to split `coords`
//   loopClosed      did the ring close (T-junctions can leave it open)
//   loopGuids       source element GUID per loop
//   loopElemTypes   ModelerAPI element type per loop
// zUsed/nudged report the tangency lift (slicing exactly at floor level).
class SliceZCommand : public QueryCommandBase {
public:
    GS::String GetName () const override { return "SliceZ"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        std::shared_ptr<const Snapshot>    snap;
        std::shared_ptr<const QueryEngine> engine;   // unused, but proves geometry exists
        GS::UniString error;
        if (!Acquire (snap, engine, error))
            return NativeCommandResult::Failure (error);

        double z = 0.0;
        if (!params.Get ("z", z)) {
            return NativeCommandResult::Failure ("need z (world height of the cut plane, meters)");
        }

        std::vector<int32_t> types;
        GS::Array<GS::Int32> typesIn;
        if (params.Get ("types", typesIn))
            for (GS::Int32 t : typesIn) types.push_back ((int32_t) t);

        std::vector<std::string> guids;
        GS::Array<GS::ObjectState> elementsIn;
        if (params.Get ("elements", elementsIn)) {
            for (const GS::ObjectState& element : elementsIn) {
                GS::ObjectState elementId;
                GS::UniString guid;
                if (!element.Get ("elementId", elementId) || !elementId.Get ("guid", guid)) {
                    return NativeCommandResult::Failure ("every element filter needs elementId.guid");
                }
                guids.push_back (std::string (guid.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ()));
            }
        }

        double weld  = 1e-6;   params.Get ("weld", weld);
        bool   nudge = true;   params.Get ("nudge", nudge);

        const SliceResult res = SliceZ (*snap, z, types, guids, weld, nudge);

        GS::Array<double>        coords;
        GS::Array<GS::Int32>     loopPointCounts, loopElemTypes;
        GS::Array<bool>          loopClosed;
        GS::Array<GS::UniString> loopGuids;
        for (const ElementSlice& el : res.elements) {
            const GS::UniString guid (el.guid.c_str (), CC_UTF8);
            for (const Polyline& loop : el.loops) {
                loopPointCounts.Push ((GS::Int32) loop.PointCount ());
                loopClosed.Push (loop.closed);
                loopGuids.Push (guid);
                loopElemTypes.Push ((GS::Int32) el.elemType);
                for (double c : loop.pts) coords.Push (c);   // already interleaved xyz
            }
        }

        os.Add ("zUsed", res.zUsed);
        os.Add ("nudged", res.nudged);
        os.Add ("loopCount", (GS::Int32) loopPointCounts.GetSize ());
        os.Add ("coords", coords);
        os.Add ("loopPointCounts", loopPointCounts);
        os.Add ("loopClosed", loopClosed);
        os.Add ("loopGuids", loopGuids);
        os.Add ("loopElemTypes", loopElemTypes);
        return os;
    }
};

// Tapioca.RaycastAll { origin, direction, maxDist?, maxHits? } -> the whole pierce
// stack, sorted by t. `enter` is what lets a caller pair hits into solid spans.
class RaycastAllCommand : public QueryCommandBase {
public:
    GS::String GetName () const override { return "RaycastAll"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        std::shared_ptr<const Snapshot>    snap;
        std::shared_ptr<const QueryEngine> engine;
        GS::UniString error;
        if (!Acquire (snap, engine, error))
            return NativeCommandResult::Failure (error);

        double origin[3], direction[3];
        if (!ReadVec3 (params, "origin", origin) || !ReadVec3 (params, "direction", direction)) {
            return NativeCommandResult::Failure ("need origin=[x,y,z] and direction=[x,y,z]");
        }
        double    maxDist = 0.0;
        GS::Int32 maxHits = 0;
        params.Get ("maxDist", maxDist);
        params.Get ("maxHits", maxHits);

        const QueryEngine::PierceResult result =
            engine->RaycastAll (origin, direction, maxDist, (size_t) GS::Max ((GS::Int32) 0, maxHits));

        GS::Array<double>        ts, points, normals;
        GS::Array<GS::UniString> guids;
        GS::Array<bool>          enters;
        GS::Array<GS::Int32>     types;
        for (const QueryEngine::PierceHit& hit : result.hits) {
            ts.Push (hit.t);
            enters.Push (hit.enter);
            guids.Push (GuidOf (*snap, hit.meshIndex));
            types.Push ((GS::Int32) snap->meshes[hit.meshIndex].elemType);
            points.Push (hit.point[0]);  points.Push (hit.point[1]);  points.Push (hit.point[2]);
            normals.Push (hit.normal[0]); normals.Push (hit.normal[1]); normals.Push (hit.normal[2]);
        }

        os.Add ("count", (GS::Int32) ts.GetSize ());
        os.Add ("t", ts);
        os.Add ("enter", enters);
        os.Add ("guids", guids);
        os.Add ("elemTypes", types);
        os.Add ("points", points);     // flat xyz
        os.Add ("normals", normals);   // flat xyz
        // NEVER silent: surfaces beyond the cap are missing entirely, and a
        // clearance read off a truncated stack is wrong rather than approximate.
        os.Add ("truncated", result.truncated);
        return os;
    }
};

// Tapioca.RaycastAllBatch { origins:[flat xyz...], directions:[flat xyz...],
//                       maxDist?, maxHits? }
// One call, N rays — a heatmap is thousands of rays and per-ray envelopes would
// dominate the runtime. `hitCounts` says how to split the flat arrays per ray.
class RaycastAllBatchCommand : public QueryCommandBase {
public:
    GS::String GetName () const override { return "RaycastAllBatch"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        std::shared_ptr<const Snapshot>    snap;
        std::shared_ptr<const QueryEngine> engine;
        GS::UniString error;
        if (!Acquire (snap, engine, error))
            return NativeCommandResult::Failure (error);

        GS::Array<double> origins, directions;
        if (!params.Get ("origins", origins) || !params.Get ("directions", directions) ||
            origins.GetSize () % 3 != 0 || origins.GetSize () != directions.GetSize ()) {
            return NativeCommandResult::Failure ("need origins and directions as equal-length flat [x,y,z,...] arrays");
        }
        double    maxDist = 0.0;
        GS::Int32 maxHits = 0;
        params.Get ("maxDist", maxDist);
        params.Get ("maxHits", maxHits);

        const UIndex rayCount = origins.GetSize () / 3;

        GS::Array<double>        ts, points, normals;
        GS::Array<GS::UniString> guids;
        GS::Array<bool>          enters;
        GS::Array<GS::Int32>     hitCounts;
        GS::Array<bool>          truncatedPerRay;

        for (UIndex r = 0; r < rayCount; ++r) {
            const double origin[3]    = { origins[r * 3], origins[r * 3 + 1], origins[r * 3 + 2] };
            const double direction[3] = { directions[r * 3], directions[r * 3 + 1], directions[r * 3 + 2] };

            const QueryEngine::PierceResult result =
                engine->RaycastAll (origin, direction, maxDist, (size_t) GS::Max ((GS::Int32) 0, maxHits));

            hitCounts.Push ((GS::Int32) result.hits.size ());
            truncatedPerRay.Push (result.truncated);
            for (const QueryEngine::PierceHit& hit : result.hits) {
                ts.Push (hit.t);
                enters.Push (hit.enter);
                guids.Push (GuidOf (*snap, hit.meshIndex));
                points.Push (hit.point[0]);  points.Push (hit.point[1]);  points.Push (hit.point[2]);
                normals.Push (hit.normal[0]); normals.Push (hit.normal[1]); normals.Push (hit.normal[2]);
            }
        }

        os.Add ("rayCount", (GS::Int32) rayCount);
        os.Add ("hitCounts", hitCounts);     // split the flat arrays with this
        os.Add ("t", ts);
        os.Add ("enter", enters);
        os.Add ("guids", guids);
        os.Add ("points", points);
        os.Add ("normals", normals);
        os.Add ("truncated", truncatedPerRay);
        return os;
    }
};

// Tapioca.ClosestPoint { point:[x,y,z], maxDist? }
class ClosestPointCommand : public QueryCommandBase {
public:
    GS::String GetName () const override { return "ClosestPoint"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        std::shared_ptr<const Snapshot>    snap;
        std::shared_ptr<const QueryEngine> engine;
        GS::UniString error;
        if (!Acquire (snap, engine, error))
            return NativeCommandResult::Failure (error);

        double point[3];
        if (!ReadVec3 (params, "point", point)) {
            return NativeCommandResult::Failure ("need point=[x,y,z]");
        }
        double maxDist = 0.0;
        params.Get ("maxDist", maxDist);

        const QueryEngine::ClosestHit hit = engine->ClosestPoint (point, maxDist);
        os.Add ("found", hit.found);
        if (hit.found) {
            os.Add ("dist", hit.dist);
            os.Add ("elementId", ElementIdOf (*snap, hit.meshIndex));
            os.Add ("point", GS::Array<double> { hit.point[0], hit.point[1], hit.point[2] });
        }
        return os;
    }
};

// Tapioca.NearestElements { point:[x,y,z], k? } — ranked by AABB distance.
class NearestElementsCommand : public QueryCommandBase {
public:
    GS::String GetName () const override { return "NearestElements"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        std::shared_ptr<const Snapshot>    snap;
        std::shared_ptr<const QueryEngine> engine;
        GS::UniString error;
        if (!Acquire (snap, engine, error))
            return NativeCommandResult::Failure (error);

        double point[3];
        if (!ReadVec3 (params, "point", point)) {
            return NativeCommandResult::Failure ("need point=[x,y,z]");
        }
        GS::Int32 k = 5;
        params.Get ("k", k);

        GS::Array<GS::ObjectState> elements;
        for (const QueryEngine::Neighbor& n : engine->NearestElement (point, (size_t) GS::Max ((GS::Int32) 1, k))) {
            GS::ObjectState element;
            element.Add ("elementId", ElementIdOf (*snap, n.meshIndex));
            element.Add ("distance", n.dist);
            elements.Push (element);
        }
        os.Add ("elements", elements);
        os.Add ("count", (GS::Int32) elements.GetSize ());
        return os;
    }
};

// Tapioca.Query { shape:"box"|"sphere"|"polygon", ... } — broadphase by AABB.
// One command rather than three: the shapes differ only in their parameters, and
// a single name keeps the Python wrappers thin.
class QueryCommand : public QueryCommandBase {
public:
    GS::String GetName () const override { return "Query"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        const auto snap = MeshStore::Get ().Current ();
        if (snap == nullptr)
            return NativeCommandResult::Failure ("no snapshot is live — call Tapioca.BuildSnapshot first");

        GS::UniString shape;
        params.Get ("shape", shape);
        std::vector<std::string> found;

        if (shape == "box") {
            double mn[3], mx[3];
            if (!ReadVec3 (params, "min", mn) || !ReadVec3 (params, "max", mx)) {
                return NativeCommandResult::Failure ("box needs min=[x,y,z] and max=[x,y,z]");
            }
            found = QueryBox (*snap, mn, mx);

        } else if (shape == "sphere") {
            double centre[3];
            double radius = 0.0;
            if (!ReadVec3 (params, "center", centre) || !params.Get ("radius", radius)) {
                return NativeCommandResult::Failure ("sphere needs center=[x,y,z] and radius");
            }
            found = QuerySphere (*snap, centre, radius);

        } else if (shape == "polygon") {
            GS::Array<double> poly;
            double zmin = 0.0, zmax = 0.0;
            if (!params.Get ("polygon", poly) || poly.GetSize () < 6 || poly.GetSize () % 2 != 0) {
                return NativeCommandResult::Failure ("polygon needs polygon=[x0,y0,x1,y1,...] with >=3 points");
            }
            params.Get ("zmin", zmin);
            params.Get ("zmax", zmax);
            std::vector<double> xy (poly.GetSize ());
            for (UIndex i = 0; i < poly.GetSize (); ++i)
                xy[i] = poly[i];
            found = QueryPolygon (*snap, xy, zmin, zmax);

        } else {
            return NativeCommandResult::Failure ("shape must be \"box\", \"sphere\" or \"polygon\"");
        }

        GS::Array<GS::ObjectState> elements;
        for (const std::string& guid : found) {
            GS::ObjectState elementId, element;
            elementId.Add ("guid", GS::UniString (guid.c_str (), CC_UTF8));
            element.Add ("elementId", elementId);
            elements.Push (element);
        }

        os.Add ("elements", elements);
        os.Add ("count", (GS::Int32) elements.GetSize ());
        return os;
    }
};

const NativeCommandRegistration kQueryCommandRegistrations[] = {
    { "Raycast", &MakeRegisteredNativeCommand<RaycastCommand>, false,
      R"json({"type":"object","properties":{"origin":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"direction":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"maxDist":{"type":"number","minimum":0}},"additionalProperties":false,"required":["origin","direction"]})json",
      R"json({"type":"object","properties":{"hit":{"type":"boolean"},"t":{"type":"number"},"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"elemType":{"type":"integer"},"point":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"normal":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"additionalProperties":false,"required":["hit"]})json" },
    { "SliceZ", &MakeRegisteredNativeCommand<SliceZCommand>, false,
      R"json({"type":"object","properties":{"z":{"type":"number"},"types":{"type":"array","items":{"type":"integer"}},"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}},"weld":{"type":"number","exclusiveMinimum":0},"nudge":{"type":"boolean"}},"additionalProperties":false,"required":["z"]})json",
      R"json({"type":"object","properties":{"zUsed":{"type":"number"},"nudged":{"type":"boolean"},"loopCount":{"type":"integer","minimum":0},"coords":{"type":"array","description":"Packed XYZ coordinates: stride 3, loops concatenated in loopPointCounts order.","items":{"type":"number"}},"loopPointCounts":{"type":"array","description":"Point count for each loop; sum(values)*3 equals coords length.","items":{"type":"integer","minimum":0}},"loopClosed":{"type":"array","description":"One closure flag per loop, indexed with loopPointCounts.","items":{"type":"boolean"}},"loopGuids":{"type":"array","description":"Packed element identity correspondence: one GUID per loop, indexed with loopPointCounts.","items":{"type":"string"}},"loopElemTypes":{"type":"array","description":"One ModelerAPI element type per loop, indexed with loopPointCounts.","items":{"type":"integer"}}},"additionalProperties":false,"required":["zUsed","nudged","loopCount","coords","loopPointCounts","loopClosed","loopGuids","loopElemTypes"]})json" },
    { "RaycastAll", &MakeRegisteredNativeCommand<RaycastAllCommand>, false,
      R"json({"type":"object","properties":{"origin":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"direction":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"maxDist":{"type":"number","minimum":0},"maxHits":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["origin","direction"]})json",
      R"json({"type":"object","properties":{"count":{"type":"integer","minimum":0},"t":{"type":"array","description":"Hit distance per index.","items":{"type":"number"}},"enter":{"type":"array","description":"Solid-entry flag per hit index.","items":{"type":"boolean"}},"guids":{"type":"array","description":"Packed element identity correspondence: GUID per hit index.","items":{"type":"string"}},"elemTypes":{"type":"array","description":"ModelerAPI element type per hit index.","items":{"type":"integer"}},"points":{"type":"array","description":"Packed hit XYZ coordinates: stride 3 in hit-index order.","items":{"type":"number"}},"normals":{"type":"array","description":"Packed normal XYZ vectors: stride 3 in hit-index order.","items":{"type":"number"}},"truncated":{"type":"boolean"}},"additionalProperties":false,"required":["count","t","enter","guids","elemTypes","points","normals","truncated"]})json" },
    { "RaycastAllBatch", &MakeRegisteredNativeCommand<RaycastAllBatchCommand>, false,
      R"json({"type":"object","properties":{"origins":{"type":"array","description":"Packed ray origins with XYZ stride 3.","items":{"type":"number"}},"directions":{"type":"array","description":"Packed ray directions with XYZ stride 3; length equals origins.","items":{"type":"number"}},"maxDist":{"type":"number","minimum":0},"maxHits":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["origins","directions"]})json",
      R"json({"type":"object","properties":{"rayCount":{"type":"integer","minimum":0},"hitCounts":{"type":"array","description":"Hit count per ray; sum(values) defines all hit-array lengths.","items":{"type":"integer","minimum":0}},"t":{"type":"array","description":"Hit distances concatenated in ray order.","items":{"type":"number"}},"enter":{"type":"array","description":"Solid-entry flags in concatenated hit order.","items":{"type":"boolean"}},"guids":{"type":"array","description":"Packed element identity correspondence: GUIDs in concatenated hit order.","items":{"type":"string"}},"points":{"type":"array","description":"Packed hit XYZ coordinates: stride 3 in concatenated hit order.","items":{"type":"number"}},"normals":{"type":"array","description":"Packed normal XYZ vectors: stride 3 in concatenated hit order.","items":{"type":"number"}},"truncated":{"type":"array","description":"One truncation flag per ray.","items":{"type":"boolean"}}},"additionalProperties":false,"required":["rayCount","hitCounts","t","enter","guids","points","normals","truncated"]})json" },
    { "ClosestPoint", &MakeRegisteredNativeCommand<ClosestPointCommand>, false,
      R"json({"type":"object","properties":{"point":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"maxDist":{"type":"number","minimum":0}},"additionalProperties":false,"required":["point"]})json",
      R"json({"type":"object","properties":{"found":{"type":"boolean"},"dist":{"type":"number"},"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"point":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"additionalProperties":false,"required":["found"]})json" },
    { "NearestElements", &MakeRegisteredNativeCommand<NearestElementsCommand>, false,
      R"json({"type":"object","properties":{"point":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"k":{"type":"integer","minimum":1}},"additionalProperties":false,"required":["point"]})json",
      R"json({"type":"object","properties":{"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"distance":{"type":"number","minimum":0}},"additionalProperties":false,"required":["elementId","distance"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["elements","count"]})json" },
    { "Query", &MakeRegisteredNativeCommand<QueryCommand>, false,
      R"json({"type":"object","properties":{"shape":{"type":"string","enum":["box","sphere","polygon"]},"min":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"max":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"center":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"radius":{"type":"number","minimum":0},"polygon":{"type":"array","description":"Packed XY polygon coordinates with stride 2 and at least three points.","items":{"type":"number"},"minItems":6},"zmin":{"type":"number"},"zmax":{"type":"number"}},"additionalProperties":false,"required":["shape"],"oneOf":[{"properties":{"shape":{"const":"box"}},"required":["min","max"]},{"properties":{"shape":{"const":"sphere"}},"required":["center","radius"]},{"properties":{"shape":{"const":"polygon"}},"required":["polygon"]}]})json",
      R"json({"type":"object","properties":{"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["elements","count"]})json" }
};

}   // namespace

NativeCommandRegistrations GetQueryCommandRegistrations ()
{
    return MakeRegistrationView (kQueryCommandRegistrations);
}

} // namespace geomsrv
