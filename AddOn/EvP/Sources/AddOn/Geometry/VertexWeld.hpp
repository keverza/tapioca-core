#ifndef GEOMETRYSERVER_VERTEXWELD_HPP
#define GEOMETRYSERVER_VERTEXWELD_HPP

#include "Mesh.hpp"

#include <cstdint>
#include <map>

// Vertex welding and normal construction. NO ACAPI — this is the part of
// GeometryExtractor that is pure math, split out so it can be unit-tested
// offline (tests/cpp/test_normals.cpp). GeometryExtractor.cpp stays thin glue
// over the DevKit, as tests/cpp/CMakeLists.txt requires.
//
// ---------------------------------------------------------------------------
// WHY THIS EXISTS
//
// ModelerAPI hands out ONE shared vertex list per body: a box is 8 vertices
// referenced by 6 faces. Averaging adjacent face normals into those 8 vertices
// gives every corner a diagonal normal, and interpolating those across a face
// shades the box like a sphere — smeared blobs, and an N.L term that goes
// negative while the face is still lit. That was the original behaviour and it
// was wrong for every hard-edged surface, which in architecture is nearly all
// of them.
//
// The fix is not a better averaging heuristic. Archicad already knows the true
// normal at every polygon corner — ConvexPolygon::GetNormalVectorByVertex() —
// smooth across a tessellated cylinder, hard across a box edge. We just have
// to stop throwing it away. Because one shared vertex now carries a different
// normal per adjacent face, the vertex has to be split: that is what the
// welder below does, merging only corners that agree on BOTH source vertex and
// normal, so a cylinder stays welded and a box edge does not.
// ---------------------------------------------------------------------------

namespace geomsrv {

// Normals are quantised before comparison so two corners Archicad reports as
// "the same" survive floating-point noise and still share a vertex. 1e-4 is
// ~0.006 degrees — far finer than any tessellation, far coarser than noise.
constexpr double kNormalQuantum = 1.0e4;

struct WeldKey {
    int32_t srcVertex;
    int32_t nx, ny, nz;

    bool operator< (const WeldKey& o) const
    {
        if (srcVertex != o.srcVertex) return srcVertex < o.srcVertex;
        if (nx != o.nx)               return nx < o.nx;
        if (ny != o.ny)               return ny < o.ny;
        return nz < o.nz;
    }
};

// Appends deduplicated (position, normal) pairs to a Mesh and hands back the
// index to use in the triangle list. One instance per body — source vertex
// indices are body-local, so Reset() between bodies is mandatory.
class VertexWelder {
public:
    explicit VertexWelder (Mesh& mesh) : mesh_ (mesh) {}

    // Reset the dedup table when moving to the next body. Does NOT clear the
    // mesh — indices already emitted stay valid.
    void Reset () { map_.clear (); }

    // Returns the mesh vertex index for this corner, creating it if the
    // (source vertex, normal) pair has not been seen in this body yet.
    // The normal is expected pre-normalized; it is stored as given.
    uint32_t Add (int32_t srcVertex,
                  double x, double y, double z,
                  double nx, double ny, double nz);

    size_t UniqueCount () const { return map_.size (); }

private:
    Mesh&                    mesh_;
    std::map<WeldKey, uint32_t> map_;
};

// Normalize in place. Returns false (and leaves the vector untouched) if the
// input is too short to have a direction — a degenerate polygon corner.
bool NormalizeVector (double& x, double& y, double& z);

// Geometric normal of a triangle, unnormalized (length = 2 * area). Used as
// the substitute when Archicad reports a degenerate corner normal.
void TriangleNormal (const double* a, const double* b, const double* c,
                     double& nx, double& ny, double& nz);

// Area-weighted average of adjacent face normals, welded across every shared
// vertex. This SMOOTHS HARD EDGES and is wrong for box-like geometry — it
// survives only as the fallback for meshes where Archicad gave us nothing
// usable, and as the thing test_normals.cpp pins the bug against.
void ComputeSmoothNormals (Mesh& mesh);

// True if every normal in the mesh is unit length within tolerance and the
// count matches the vertex count. Used to decide whether to fall back.
bool NormalsAreUsable (const Mesh& mesh);

} // namespace geomsrv

#endif
