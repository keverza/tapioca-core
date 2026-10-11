#ifndef GEOMETRYSERVER_SURFACESOURCEMAP_HPP
#define GEOMETRYSERVER_SURFACESOURCEMAP_HPP

#include "Mesh.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Geometry <-> provenance diagnostic for an extracted Mesh.
//
// A picked polygon (from Archicad's own picker or any external route) arrives
// with a geometry and, maybe, some element-local body/polygon numbers. The
// extracted Mesh records, per source triangle, the ModelerAPI body and polygon
// it was emitted from (Mesh::triSourceBody / triSourcePgon). This helper is the
// ONE place that is allowed to correlate the two, and it is deliberately strict:
// it matches the COMPLETE boundary geometry (outer contour and every hole, after
// collinear cleanup and snapping to a tolerance grid) and refuses -- Invalid /
// NoMatch / Ambiguous -- rather than guessing. It is pure: no ACAPI, no
// ModelerAPI, no clipping engine, no GPU, so it is exercised offline in
// tests/cpp.
//
// ⚠️ AGGREGATES ARE NOT ENOUGH TO IDENTIFY A FACE. A unit square and a
// same-area, same-centroid diamond have equal plane, orientation, area and
// centroid; only their boundaries differ. Resolve never returns Ok on
// plane+area+centroid+normal alone.
//
// ⚠️ IT NEVER CLAIMS A NUMBERING EQUALITY. The ModelerAPI body/polygon indices
// carried here are not the C 3D-database ids and not the picker's ids. A caller
// passes the ids it has as `expected*`; they are checked ONLY after the geometry
// has resolved to exactly one face, and a mismatch is Invalid. When geometry is
// ambiguous the ids are never used to choose a winner.
namespace geomsrv {
namespace surfacemap {

// One (body, polygon) source face, validated at enumeration: all triangles
// coplanar and consistently wound, area finite and positive.
struct SourceFace {
    int32_t sourceBody = 0;          // 1-based ModelerAPI body index (never 0 here)
    int32_t sourcePgon = 0;          // 1-based polygon index within that body
    std::vector<uint32_t> triangles; // SOURCE triangle indices (into Mesh::triangles)
    // Validated triangle geometry, xyz interleaved, exactly 9 doubles per
    // triangle, in the same order as `triangles`. Carried so a face can be
    // re-checked geometrically without the source Mesh.
    std::vector<double> trianglePoints;
    double areaM2 = 0.0;     // sum of the triangle magnitudes (the emitted, same-wound tessellation)
    double centroid[3] = {}; // area-weighted centroid, world metres
    double normal[3] = {};   // unit orientation of the tessellation; {0,0,0} if degenerate
};

enum class MatchStatus {
    Ok,        // exactly one face matched, ids (when supplied) agree
    Invalid,   // the mesh, the query or a tolerance is unusable -- refuse, do not guess
    NoMatch,   // the complete boundary matched no face
    Ambiguous, // the complete boundary matched more than one face -- refuse
};

struct MatchResult {
    MatchStatus status = MatchStatus::Invalid;
    SourceFace face;    // valid only when status == Ok
    std::string reason; // human-readable; never empty
};

// The face to resolve. `points` is the outer contour, xyz interleaved (a closed
// polygon; the last point need not repeat the first). `pieces` adds further
// contours: a HOLE is wound opposite the outer, and a DECOMPOSED CONVEX PART is
// wound the same way; a union boundary is formed from all pieces together, so
// both representations compare equal to the emitted tessellation.
struct FaceQuery {
    std::vector<double> points;              // outer contour (may be empty if `pieces` is not)
    int32_t expectedBody = 0;                // ModelerAPI body id; 0 = unknown; negative = invalid
    int32_t expectedPgon = 0;                // ModelerAPI polygon id; 0 = unknown; negative = invalid
    std::vector<std::vector<double>> pieces; // extra contours (holes / convex parts), xyz interleaved
};

// How close geometry must be to count as a match. Lengths are metres; the
// normal term is a dot product of two unit vectors (1.0 = exactly parallel).
// Matching is SAME-SENSE: a contour wound the other way does not match.
struct MatchTolerance {
    double normalDot = 0.999999;     // ~0.08 degrees
    double planeDistance = 1.0e-4;   // point-to-plane coplanarity and offset
    double vertexTolerance = 1.0e-4; // boundary snap grid and collinear cleanup
};

// Enumerate every (body, polygon) face of `mesh`, deterministically ordered by
// (body, pgon). Returns empty and sets `reason` (when non-null) for a mesh whose
// provenance sidecar is absent or misaligned, whose triangle indexes a missing
// vertex, whose positions are non-finite, or which contains a non-planar,
// inconsistently wound or degenerate face. An empty mesh is valid and yields no
// faces.
std::vector<SourceFace> EnumerateSourceFaces (const Mesh& mesh, std::string* reason = nullptr,
                                              const MatchTolerance& tolerance = {});

// The exact area of the source triangles carrying one (body, polygon): the sum
// of their magnitudes, which is the polygon area for a valid same-wound emitted
// tessellation (holes already handled by the tessellation). False and a reason
// when the mesh is unusable or the face is absent.
bool AnalyticArea (const Mesh& mesh, int32_t sourceBody, int32_t sourcePgon, double* areaM2,
                   std::string* reason = nullptr, const MatchTolerance& tolerance = {});

// Resolve a query against already-enumerated faces (validated by the caller).
MatchResult Resolve (const std::vector<SourceFace>& faces, const FaceQuery& query,
                     const MatchTolerance& tolerance = {});

// Convenience over one mesh: enumerates first, so an unusable mesh reports Invalid.
MatchResult Resolve (const Mesh& mesh, const FaceQuery& query, const MatchTolerance& tolerance = {});

} // namespace surfacemap
} // namespace geomsrv

#endif
