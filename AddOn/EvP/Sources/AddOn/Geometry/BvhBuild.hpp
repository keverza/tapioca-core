#ifndef GEOMETRYSERVER_BVHBUILD_HPP
#define GEOMETRYSERVER_BVHBUILD_HPP

// Geometry/BvhBuild — the triangle BVH that QueryEngine's index is built with.
//
// ⚠️ WHY NOT nanort's OWN Build. The pinned nanort (and upstream master as of
// 2026-10-31) bins its SAH with
//     bin_idx = axis * bin_size + idx;  if (bin_idx < bin_size) { ... }
// so only the X axis ever receives counts. The empty Y and Z bins cost NaN,
// so every split is chosen along X alone; where X cannot separate the
// triangles, the fallback cuts Y or Z at 1/64 of the node's extent. Boxes
// are tall and wide, and on regular layouts the tree is a stack of slabs:
// depth 117 on an 806k-triangle model and 184 on a synthetic city, against
// ~25 for a balanced tree. A grazing sun ray then visits thousands of nodes.
// The live study's low-sun steps cost 8x its noon steps, and 40% of its GPU
// rays exhaust their 4,096-node budget (#97).
//
// The output is nanort's node LAYOUT, unchanged, so every traversal --
// nanort's Traverse, QueryAnyHit, RaycastAll and the GPU export -- runs the
// same arithmetic on a better tree:
//   * pre-order: a branch is followed by its whole left subtree;
//   * branch: flag 0, axis = split axis, data[0] = left child (the LOW side
//     along `axis`), data[1] = right child;
//   * leaf: flag 1, data[0] = triangle count, data[1] = first slot in indices;
//   * every node's box is the union of its triangles' boxes, exactly as
//     nanort computes it.
// Every triangle index appears in exactly one leaf, degenerate ones included:
// an index that drops a triangle would turn a shadow into sunlight.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace geomsrv {

struct BvhBuildNode {
    double bmin[3];
    double bmax[3];
    int flag;
    int axis;
    uint32_t data[2];
};

struct BvhBuildResult {
    std::vector<BvhBuildNode> nodes;
    std::vector<uint32_t> indices;
    size_t depth = 0; // deepest leaf, the root being 1
};

// No node is deeper than this. The GPU traversal (SunStudyGpuShader.hpp)
// keeps a 64-entry stack and a pre-order walk holds at most one pending
// sibling per level, so a capped tree can never overflow it. A node that
// reaches the cap becomes a leaf with everything left in it: slower, never
// wrong.
constexpr size_t kBvhMaxDepth = 60;

// `faces` holds 3 * `triangles` vertex indices into `vertices` (xyz doubles).
BvhBuildResult BuildBinnedSahBvh (const double* vertices, const uint32_t* faces, size_t triangles);

} // namespace geomsrv

#endif
