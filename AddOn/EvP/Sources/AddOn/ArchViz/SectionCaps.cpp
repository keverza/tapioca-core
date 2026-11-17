// ArchViz/SectionCaps -- see the header.

#include "ArchViz/SectionCaps.hpp"

#include "Geometry/SliceEngine.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace geomsrv {
namespace archviz {

namespace {

int AxisIndex (SectionAxis axis)
{
    return std::clamp (int (axis), 0, 2);
}

} // namespace

void ToSectionLocal (SectionAxis axis, const float world[3], double out[3])
{
    // Cyclic: (u, v, w) = (world[a+1], world[a+2], world[a]). Z is the identity.
    const int a = AxisIndex (axis);
    out[0] = double (world[(a + 1) % 3]);
    out[1] = double (world[(a + 2) % 3]);
    out[2] = double (world[a]);
}

void SectionLocalToWorld (SectionAxis axis, float out[16])
{
    // Row-vector (MatrixMath): world = local * M, so row i is where local axis i lands.
    for (int k = 0; k < 16; ++k)
        out[k] = 0.0f;
    out[15] = 1.0f;
    const int a = AxisIndex (axis);
    out[0 * 4 + (a + 1) % 3] = 1.0f; // u -> world[a+1]
    out[1 * 4 + (a + 2) % 3] = 1.0f; // v -> world[a+2]
    out[2 * 4 + a] = 1.0f;           // w -> world[a]
}

SectionCapResult BuildSectionCaps (const SectionCapRequest& request)
{
    SectionCapResult result;
    const int a = AxisIndex (request.axis);
    const double cut = double (request.coordinate);
    // Into the KEPT side: unflipped keeps the low side.
    const double nudgeSign = request.flipped ? 1.0 : -1.0;

    std::vector<double> local;
    std::vector<SliceChain> chains;
    std::vector<SliceChain> closed;
    for (const std::shared_ptr<const SectionMesh>& meshPtr : request.meshes) {
        if (meshPtr == nullptr)
            continue;
        const SectionMesh& mesh = *meshPtr;
        const size_t vertexCount = mesh.positions.size () / 3;
        const size_t triangleCount = mesh.indices.size () / 3;
        if (vertexCount == 0 || triangleCount == 0)
            continue;
        // The float grid at this element's coordinates, four ulps wide, floored at 0.1 mm.
        double magnitude = 0.0;
        for (int k = 0; k < 3; ++k)
            magnitude = std::max (
                { magnitude, std::fabs (double (mesh.boundsMin[k])), std::fabs (double (mesh.boundsMax[k])) });
        const double weld = std::max (1.0e-4, magnitude * 4.0 * 1.1920929e-7);
        if (cut < double (mesh.boundsMin[a]) - weld || cut > double (mesh.boundsMax[a]) + weld)
            continue;

        local.resize (vertexCount * 3);
        for (size_t v = 0; v < vertexCount; ++v)
            ToSectionLocal (request.axis, &mesh.positions[v * 3], &local[v * 3]);
        // ⚠️ A PLANE ON A VERTEX IS TANGENT: a slab cut exactly at its top face has no cross-section,
        // only a ring of degenerate chains. Nudge into the kept side by the weld, as SliceZ nudges.
        double z = cut;
        if (IsTangentToPlane (local.data (), vertexCount, z))
            z += nudgeSign * weld;
        const std::vector<Polyline> loops =
            SliceMesh (local.data (), vertexCount, mesh.indices.data (), triangleCount, z, weld);
        if (loops.empty ())
            continue;

        chains.clear ();
        closed.clear ();
        for (const Polyline& loop : loops) {
            const size_t n = loop.PointCount ();
            if (n < 2)
                continue;
            SliceChain chain;
            chain.closed = loop.closed;
            // A closed Polyline repeats its first point; a SliceChain does not.
            const size_t keep = loop.closed && n > 1 ? n - 1 : n;
            chain.xy.reserve (keep * 2);
            for (size_t p = 0; p < keep; ++p) {
                chain.xy.push_back (loop.pts[p * 3 + 0]);
                chain.xy.push_back (loop.pts[p * 3 + 1]);
            }
            if (chain.closed && chain.Count () >= 3) {
                ++result.counts.closedLoops;
                closed.push_back (chain);
            }
            else {
                chain.closed = false;
                ++result.counts.openChains;
            }
            chains.push_back (std::move (chain));
        }
        if (chains.empty ())
            continue;
        ++result.counts.elementsCut;
        BuildSliceRibbon (chains, float (z), result.outline);
        if (!closed.empty ()) {
            double area = 0.0;
            std::string error;
            if (AppendIndexedSliceFill (closed, z, result.fill, result.fillIndices, area, error))
                result.counts.areaM2 += area;
            else
                ++result.counts.fillFailures;
        }
    }
    return result;
}

} // namespace archviz
} // namespace geomsrv
