#ifndef GEOMETRYSERVER_CLASHENGINE_HPP
#define GEOMETRYSERVER_CLASHENGINE_HPP

#include "Mesh.hpp"

#include <vector>
#include <cstdint>

// Narrowphase clash & clearance (M6) between element meshes of a snapshot.
//   Clash     = do any triangles of A and B intersect? (Möller tri-tri test)
//   Clearance = minimum surface-to-surface distance (0 if they clash).
// Pure C++ over the immutable snapshot — runs on HTTP worker threads, no ACAPI.
namespace geomsrv {

// True if any triangle of A intersects any triangle of B. AABB-pruned.
bool MeshesClash (const Mesh& a, const Mesh& b);

struct Clearance {
    bool   clash = false;         // triangles intersect -> dist == 0
    double dist = 0.0;            // min surface-to-surface distance (meters)
    double pointA[3] = { 0,0,0 }; // witness point on A
    double pointB[3] = { 0,0,0 }; // witness point on B
};

// Minimum distance between the two meshes' surfaces. If maxDist > 0, the search
// may early-out once it can prove the distance exceeds maxDist (dist then >= it,
// witness points unspecified) — use for "are these within X?" screening.
Clearance MeshClearance (const Mesh& a, const Mesh& b, double maxDist = 0.0);

// All clashing/near pairs across the snapshot. gap <= 0 -> only true clashes
// (dist 0). gap > 0 -> every pair whose clearance <= gap (includes clashes).
struct ClashPair {
    size_t i = 0, j = 0;   // indices into snapshot->meshes
    double dist = 0.0;
};
std::vector<ClashPair> ClashAll (const Snapshot& snap, double gap);

} // namespace geomsrv

#endif
