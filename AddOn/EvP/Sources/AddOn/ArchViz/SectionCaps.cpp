// ArchViz/SectionCaps -- see the header.

#include "ArchViz/SectionCaps.hpp"

#include "Geometry/SliceEngine.hpp"

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace geomsrv {
namespace archviz {

namespace cp = Clipper2Lib;

namespace {

// Clipper2's decimal precision: micrometres, on coordinates made local to each element.
constexpr int kPrecision = 6;
// More strokes than this in one element is a spacing too fine to read; the hatch is refused.
constexpr double kMaxHatchStrokes = 4000.0;

int AxisIndex (SectionAxis axis)
{
    return std::clamp (int (axis), 0, 2);
}

// Clipper paths (element-local) -> closed chains (plane frame) -> triangles appended to `mesh`.
bool AppendPaths (const cp::PathsD& paths, double originX, double originY, double z, SectionCapMesh& mesh)
{
    std::vector<SliceChain> chains;
    chains.reserve (paths.size ());
    for (const cp::PathD& path : paths) {
        if (path.size () < 3)
            continue;
        SliceChain chain;
        chain.closed = true;
        chain.xy.reserve (path.size () * 2);
        for (const cp::PointD& p : path) {
            chain.xy.push_back (p.x + originX);
            chain.xy.push_back (p.y + originY);
        }
        chains.push_back (std::move (chain));
    }
    if (chains.empty ())
        return true;
    double area = 0.0;
    std::string error;
    return AppendIndexedSliceFill (chains, z, mesh.vertices, mesh.indices, area, error);
}

// Parallel strokes over `region`'s bounds, anchored at the frame's absolute origin, clipped to it.
cp::PathsD Hatch (const cp::PathsD& region, double originX, double originY, const SectionCapShape& shape, bool& refused)
{
    refused = false;
    const double spacing = std::max (double (shape.hatchSpacing), 1.0e-3);
    const double half = std::clamp (double (shape.hatchWidth), 1.0e-4, spacing) * 0.5;
    const double angle = double (shape.hatchAngleDegrees) * 3.14159265358979323846 / 180.0;
    const double dx = std::cos (angle), dy = std::sin (angle); // along a stroke
    const double nx = -dy, ny = dx;                            // across the strokes
    double dMin = 1e300, dMax = -1e300, nMin = 1e300, nMax = -1e300;
    for (const cp::PathD& path : region)
        for (const cp::PointD& p : path) {
            const double x = p.x + originX, y = p.y + originY;
            dMin = std::min (dMin, x * dx + y * dy);
            dMax = std::max (dMax, x * dx + y * dy);
            nMin = std::min (nMin, x * nx + y * ny);
            nMax = std::max (nMax, x * nx + y * ny);
        }
    if (dMin > dMax)
        return {};
    const double first = std::floor (nMin / spacing), last = std::ceil (nMax / spacing);
    if (last - first > kMaxHatchStrokes) {
        refused = true;
        return {};
    }
    cp::PathsD strokes;
    for (double k = first; k <= last; k += 1.0) {
        const double c = k * spacing;
        auto at = [&] (double d, double n) {
            return cp::PointD (d * dx + n * nx - originX, d * dy + n * ny - originY);
        };
        strokes.push_back ({ at (dMin - 1.0, c - half), at (dMax + 1.0, c - half), at (dMax + 1.0, c + half),
                             at (dMin - 1.0, c + half) });
    }
    return cp::Intersect (strokes, region, cp::FillRule::NonZero, kPrecision);
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

double SectionCapMeshArea (const SectionCapMesh& mesh)
{
    double total = 0.0;
    for (size_t i = 0; i + 2 < mesh.indices.size (); i += 3) {
        const StorySliceFillVertex& a = mesh.vertices[mesh.indices[i]];
        const StorySliceFillVertex& b = mesh.vertices[mesh.indices[i + 1]];
        const StorySliceFillVertex& c = mesh.vertices[mesh.indices[i + 2]];
        total += 0.5 * std::fabs (double (b.x - a.x) * double (c.y - a.y) - double (c.x - a.x) * double (b.y - a.y));
    }
    return total;
}

SectionCapResult BuildSectionCaps (const SectionCapRequest& request)
{
    SectionCapResult result;
    const int a = AxisIndex (request.axis);
    const double cut = double (request.coordinate);
    // Into the KEPT side: unflipped keeps the low side.
    const double nudgeSign = request.flipped ? 1.0 : -1.0;
    const double width = std::max (double (request.shape.lineWidth), 0.0);

    std::vector<double> local;
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

        // ⚠️ ELEMENT-LOCAL COORDINATES FOR CLIPPER: its integer grid is exact near the origin, and
        // survey coordinates times 10^6 would spend the precision on the offset to the site.
        const double originX = local[0], originY = local[1];
        cp::PathsD closed, open;
        for (const Polyline& loop : loops) {
            const size_t n = loop.PointCount ();
            if (n < 2)
                continue;
            // A closed Polyline repeats its first point; a Clipper path does not.
            const size_t keep = loop.closed && n > 1 ? n - 1 : n;
            cp::PathD path;
            path.reserve (keep);
            for (size_t p = 0; p < keep; ++p)
                path.emplace_back (loop.pts[p * 3 + 0] - originX, loop.pts[p * 3 + 1] - originY);
            if (loop.closed && path.size () >= 3) {
                ++result.counts.closedLoops;
                closed.push_back (std::move (path));
            }
            else {
                ++result.counts.openChains;
                open.push_back (std::move (path));
            }
        }
        if (closed.empty () && open.empty ())
            continue;
        ++result.counts.elementsCut;

        bool ok = true;
        if (!closed.empty ()) {
            // The solid, even-odd: holes and islands resolved once, here.
            const cp::PathsD region = cp::Union (closed, cp::FillRule::EvenOdd, kPrecision);
            result.counts.areaM2 += std::fabs (cp::Area (region));
            // ⚠️ MITRE, NOT ROUND OR SQUARE: a section line's corner is a corner. The limit (2)
            // squares off only spikes sharper than 60 degrees, which would otherwise reach far
            // past the solid they outline.
            const cp::PathsD inner = width > 0.0 ? cp::InflatePaths (region, -width, cp::JoinType::Miter,
                                                                     cp::EndType::Polygon, 2.0, kPrecision)
                                                 : region;
            const cp::PathsD band =
                width > 0.0 ? cp::Difference (region, inner, cp::FillRule::NonZero, kPrecision) : cp::PathsD {};
            ok = AppendPaths (inner, originX, originY, z, result.poche) && ok;
            ok = AppendPaths (band, originX, originY, z, result.line) && ok;
            if (request.shape.hatch && !inner.empty ()) {
                bool refused = false;
                const cp::PathsD strokes = Hatch (inner, originX, originY, request.shape, refused);
                ok = !refused && AppendPaths (strokes, originX, originY, z, result.hatch) && ok;
            }
        }
        if (!open.empty () && width > 0.0) {
            // No inside to inset into: the same width, centred, with butt ends.
            const cp::PathsD stroke =
                cp::InflatePaths (open, width * 0.5, cp::JoinType::Miter, cp::EndType::Butt, 2.0, kPrecision);
            const cp::PathsD merged = cp::Union (stroke, cp::FillRule::NonZero, kPrecision);
            ok = AppendPaths (merged, originX, originY, z, result.line) && ok;
        }
        if (!ok)
            ++result.counts.fillFailures;
    }
    return result;
}

} // namespace archviz
} // namespace geomsrv
