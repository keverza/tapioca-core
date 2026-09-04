#ifndef GEOMETRYSERVER_QUERYENGINE_HPP
#define GEOMETRYSERVER_QUERYENGINE_HPP

#include "Mesh.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

// Narrowphase geometry queries (M5) over a snapshot: raycast, closest-surface-
// point and nearest-element. Backed by a per-snapshot triangle BVH (nanort)
// built from the concatenated triangles of every mesh. All coordinates are
// world-space meters. Pure C++ — runs on HTTP worker threads; no ACAPI.
//
// A QueryEngine is immutable once built and safe for concurrent reads (nanort
// traversal is const/read-only), so many worker threads can raycast the same
// engine in parallel.
namespace geomsrv {

class QueryEngine {
  public:
    // Builds the combined vertex/face arrays and the triangle BVH. Heavy; do it
    // once per snapshot (see QueryIndexCache).
    explicit QueryEngine (std::shared_ptr<const Snapshot> snap);
    ~QueryEngine ();

    QueryEngine (const QueryEngine&) = delete;
    QueryEngine& operator= (const QueryEngine&) = delete;

    uint64_t SnapshotId () const
    {
        return snapshot->id;
    }
    size_t MeshCount () const
    {
        return snapshot->meshes.size ();
    }
    size_t TriangleCount () const
    {
        return triToMesh.size ();
    }

    // Index of a mesh in snapshot->meshes; its guid via MeshGuid.
    const std::string& MeshGuid (size_t idx) const
    {
        return snapshot->meshes[idx].guid;
    }
    int32_t MeshType (size_t idx) const
    {
        return snapshot->meshes[idx].elemType;
    }

    // ---- Single raycast ----------------------------------------------------
    struct RayHit {
        bool hit = false;
        size_t meshIndex = 0; // index into snapshot->meshes (valid iff hit)
        uint32_t tri = 0;     // global triangle index (valid iff hit)
        double t = 0.0;       // distance along dir (dir is normalised first)
        double point[3] = { 0, 0, 0 };
        double normal[3] = { 0, 0, 0 }; // interpolated smooth normal, unit
    };
    RayHit Raycast (const double org[3], const double dir[3], double maxDist) const;

    // ---- Occlusion ("is anything in the way") ------------------------------
    // True when any surface lies on the ray between `tmin` and `tmax`. The
    // shadow-ray query: it answers a yes/no question and returns nothing else.
    //
    // Use this rather than Raycast or RaycastAll wherever only the boolean is
    // wanted. RaycastAll collects EVERY hit, sorts them by t, interpolates a
    // normal per hit and heap-allocates a vector to hold them; a sunlight-hours
    // run asks this question once per (sample x timestep) and none of that work
    // reaches the answer. Raycast is closer but still interpolates a smooth
    // normal for a caller that discards it.
    //
    // ⚠️ IT IS NOT AN ANY-HIT TRAVERSAL, and the name must not be read as one.
    // nanort exposes closest-hit `Traverse` only -- there is no TraverseAny --
    // so this still descends to the nearest intersection within [tmin, tmax].
    // A tight `tmax` is therefore a real optimisation and not merely a filter.
    //
    // `tmin` skips the surface the ray starts on (Jifto's selfHitThreshold,
    // typically 0.001 m); `tmax <= 0` means unbounded. `dir` need not be
    // normalised -- it is normalised here, so tmin/tmax are true distances in
    // metres either way.
    bool Occluded (const double org[3], const double dir[3], double tmin, double tmax) const;

    // ---- All-hits ("pierce") raycast ---------------------------------------
    // Every surface the ray passes through, sorted by t ascending. Back faces are
    // NOT culled, so each solid contributes an entry and an exit hit; use `enter`
    // (normal·dir < 0) to pair them into solid intervals.
    //
    // Same-element hits at the same t are merged (they are triangulation seams —
    // a ray crossing a shared triangle edge is reported by both triangles).
    // Coincident surfaces from DIFFERENT elements are kept as separate hits: we
    // never silently drop an element from the stack.
    struct PierceHit {
        size_t meshIndex = 0;
        uint32_t tri = 0;
        double t = 0.0;
        double point[3] = { 0, 0, 0 };
        double normal[3] = { 0, 0, 0 };
        bool enter = false; // true if the ray is entering the surface
    };
    struct PierceResult {
        std::vector<PierceHit> hits; // sorted by t ascending
        bool truncated = false;      // more hits existed than maxHits
    };
    // maxDist <= 0 -> unbounded. maxHits == 0 -> no cap. If more hits exist than
    // maxHits, the NEAREST maxHits are kept and `truncated` is set (never silent).
    PierceResult RaycastAll (const double org[3], const double dir[3], double maxDist, size_t maxHits) const;

    // ---- Closest surface point --------------------------------------------
    struct ClosestHit {
        bool found = false;
        size_t meshIndex = 0;
        double point[3] = { 0, 0, 0 };
        double dist = 0.0;
    };
    // maxDist <= 0 means unbounded.
    ClosestHit ClosestPoint (const double p[3], double maxDist) const;

    // ---- Nearest elements (ranked by AABB distance) ------------------------
    struct Neighbor {
        size_t meshIndex = 0;
        double dist = 0.0; // point-to-AABB distance (0 if inside)
    };
    std::vector<Neighbor> NearestElement (const double p[3], size_t k) const;

  private:
    struct Impl;

    std::shared_ptr<const Snapshot> snapshot;

    // Concatenated geometry (all meshes) for the BVH.
    std::vector<double> verts;       // xyz interleaved, world meters
    std::vector<float> norms;        // per-vertex smooth normals, aligned to verts
    std::vector<uint32_t> faces;     // global triangle indices into verts
    std::vector<uint32_t> triToMesh; // meshIndex per global triangle

    std::unique_ptr<Impl> impl; // holds the nanort BVH (keeps nanort.h out of this header)

    // Closest point on triangle `globalTri` to p; returns squared distance.
    double ClosestOnTri (const double p[3], uint32_t globalTri, double outPoint[3]) const;

    // Unit surface normal at barycentric (u,v) on global triangle `globalTri`:
    // the interpolated smooth normal, falling back to the geometric face normal
    // when the source mesh carried no normals.
    void SurfaceNormal (uint32_t globalTri, double u, double v, double out[3]) const;
};

// Lazily builds and caches one QueryEngine for the current snapshot id, so
// repeated queries reuse the same BVH. Rebuilds when the snapshot changes.
class QueryIndexCache {
  public:
    static QueryIndexCache& Get ()
    {
        static QueryIndexCache instance;
        return instance;
    }

    // Returns an engine for `snap` (building it under lock on first use), or
    // null if snap is null/empty.
    std::shared_ptr<const QueryEngine> For (const std::shared_ptr<const Snapshot>& snap);

    // Drop the cached BVH (it is the largest single allocation after the mesh
    // data itself). In-flight queries hold their own shared_ptr and are unaffected.
    void Release ()
    {
        std::lock_guard<std::mutex> lock (mtx);
        cached.reset ();
    }

  private:
    QueryIndexCache () = default;

    std::mutex mtx;
    std::shared_ptr<const QueryEngine> cached;
};

} // namespace geomsrv

#endif
