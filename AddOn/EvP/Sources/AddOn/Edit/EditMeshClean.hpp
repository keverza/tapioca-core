#ifndef TAPIOCA_EDIT_MESHCLEAN_HPP
#define TAPIOCA_EDIT_MESHCLEAN_HPP

// #139 M0: CleanMesh — the deterministic repair pipeline for one published
// triangle mesh. One pass, fixed step order, no heuristics that depend on
// container order:
//
//   1. Weld   — vertices within `weldTolerance` metres merge into the
//               lowest-index representative (27-cell exact-distance search,
//               tolerance <= 0 disables welding). The representative keeps
//               its position and normal; a merged pair whose normals differ
//               beyond kNormalConflictTolerance is still welded (position
//               wins) and counted in normalConflicts.
//   2. Degenerate — triangles with a repeated (post-weld) vertex index or
//               roundoff-zero area are dissolved.
//   3. Interior — coincidence classes containing BOTH cyclic orientations
//               are zero-thickness internal walls: every member is removed.
//   4. Duplicate — coincident same-orientation extras are removed, the
//               lowest-index member survives.
//   5. Loose — vertices no surviving triangle references are dropped and the
//               index buffer is compacted.
//
// Per-triangle attributes (material) follow the surviving triangle; normals
// follow the surviving representative vertex. Output vertexSource/
// triangleSource map back to the INPUT mesh indices, so a cleaned mesh keeps
// stable full-resolution identity. The pipeline is idempotent: a second run
// on a cleaned mesh changes nothing and reports all zeros. Clean does NOT
// repair non-manifoldness — that is a separately gated operator.

#include "Edit/EditPublishedMesh.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tapioca::geometry {

// Two normals conflict when any component differs by more than this. Matches
// the scale of VertexWeld's kNormalQuantum (~0.006 degrees): far finer than a
// real crease, far coarser than floating-point noise.
inline constexpr double kNormalConflictTolerance = 1.0e-3;

struct MeshCleanOptions {
    double weldTolerance = 1.0e-4; // Metres; <= 0 disables welding.
};

struct MeshCleanReport {
    uint32_t weldedVertices = 0;      // Merged into an earlier representative.
    uint32_t normalConflicts = 0;     // Welded pairs with disagreeing normals.
    uint32_t removedDegenerate = 0;   // Repeated index or roundoff-zero area.
    uint32_t removedInterior = 0;     // Members of both-orientation classes.
    uint32_t removedDuplicate = 0;    // Coincident same-orientation extras.
    uint32_t removedUnreferenced = 0; // Vertices dropped for lack of any face.
    bool IsNoOp () const
    {
        return weldedVertices == 0 && normalConflicts == 0 && removedDegenerate == 0 && removedInterior == 0 &&
               removedDuplicate == 0 && removedUnreferenced == 0;
    }
};

// Runs the pipeline on a VALID published mesh. Fails closed (input validity
// gate, malformed geometry) leaving `output` and `report` untouched. On
// success `output` is a complete published mesh: identity carried from the
// input, definitionHash = the input's content hash, bounds recomputed,
// vertexSource/triangleSource mapping to the input indices.
bool CleanMesh (const PublishedMesh& input, const MeshCleanOptions& options, PublishedMesh& output,
                MeshCleanReport& report, std::string& error);

} // namespace tapioca::geometry

#endif
