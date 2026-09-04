#include "SunStudy/SunStudySampler.hpp"

#include <algorithm>
#include <cmath>

namespace evp::sunstudy {
namespace {

Vec3 Sub (const Vec3& a, const Vec3& b)
{
    return Vec3 { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
}

double Dot (const Vec3& a, const Vec3& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 Cross (const Vec3& a, const Vec3& b)
{
    return Vec3 { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}

Vec3 ReadVertex (const double* vertices, uint32_t index)
{
    const double* p = vertices + static_cast<size_t> (index) * 3;
    return Vec3 { p[0], p[1], p[2] };
}

// Integer avalanche. Any hash with good bit mixing would do; what matters is
// that it is a pure function of the indices, so the same cell always jitters
// the same way no matter which thread reaches it.
uint32_t Mix (uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

double Unit (uint32_t h)
{
    return static_cast<double> (h) * (1.0 / 4294967296.0);
}

} // namespace

Vec3 TriangleNormal (const Vec3& a, const Vec3& b, const Vec3& c, double* areaOut)
{
    const Vec3 n = Cross (Sub (b, a), Sub (c, a));
    const double length = std::sqrt (Dot (n, n));
    if (areaOut != nullptr)
        *areaOut = 0.5 * length;
    if (!(length > 0.0))
        return Vec3 { 0.0, 0.0, 0.0 };
    return Vec3 { n[0] / length, n[1] / length, n[2] / length };
}

void CellJitter (uint32_t face, uint32_t u, uint32_t v, double out[2])
{
    const uint32_t seed = Mix (face * 0x9e3779b9u ^ Mix (u * 0x85ebca6bu ^ Mix (v)));
    out[0] = Unit (seed) - 0.5;
    out[1] = Unit (Mix (seed ^ 0xc2b2ae35u)) - 0.5;
}

SampleGrid BuildSampleGrid (const double* vertices, size_t vertexCount, const uint32_t* triangles, size_t faceCount,
                            const uint32_t* groups, const SamplerOptions& options)
{
    SampleGrid grid;
    if (vertices == nullptr || triangles == nullptr || faceCount == 0)
        return grid;
    if (!(options.spacing > 0.0))
        return grid;

    const double jitterScale = std::max (0.0, std::min (1.0, options.jitter));

    // Positions are collected unoffset and lifted at the end, so the offset is
    // applied in exactly one place for every sample.
    std::vector<double> flatPositions;
    std::vector<double> flatNormals;

    if (options.wantLayouts)
        grid.layouts.assign (faceCount, FaceLayout ());

    for (size_t face = 0; face < faceCount; ++face) {
        const uint32_t i0 = triangles[face * 3 + 0];
        const uint32_t i1 = triangles[face * 3 + 1];
        const uint32_t i2 = triangles[face * 3 + 2];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
            ++grid.degenerateFaces;
            continue;
        }

        const Vec3 a = ReadVertex (vertices, i0);
        const Vec3 b = ReadVertex (vertices, i1);
        const Vec3 c = ReadVertex (vertices, i2);

        double area = 0.0;
        const Vec3 normal = TriangleNormal (a, b, c, &area);
        if (!(area > 0.0)) {
            ++grid.degenerateFaces;
            continue;
        }

        // An orthonormal basis IN THE FACE'S OWN PLANE. Gridding in world XY
        // instead would collapse on a vertical wall -- its footprint is a line,
        // so every cell centre would miss and every facade would fall back to a
        // single centroid sample. Facades are most of what a sun study is for.
        const Vec3 edge = Sub (b, a);
        const double edgeLength = std::sqrt (Dot (edge, edge));
        if (!(edgeLength > 0.0)) {
            ++grid.degenerateFaces;
            continue;
        }
        const Vec3 u = Vec3 { edge[0] / edgeLength, edge[1] / edgeLength, edge[2] / edgeLength };
        const Vec3 v = Cross (normal, u);

        const double u0 = 0.0, v0 = 0.0;
        const double u1 = Dot (Sub (b, a), u), v1 = Dot (Sub (b, a), v);
        const double u2 = Dot (Sub (c, a), u), v2 = Dot (Sub (c, a), v);

        const double uLo = std::min (u0, std::min (u1, u2));
        const double uHi = std::max (u0, std::max (u1, u2));
        const double vLo = std::min (v0, std::min (v1, v2));
        const double vHi = std::max (v0, std::max (v1, v2));

        // Barycentric denominator from the 2D form, computed once per face.
        const double d = (v1 - v2) * (u0 - u2) + (u2 - u1) * (v0 - v2);
        const double inverseD = (std::abs (d) > 0.0) ? 1.0 / d : 0.0;

        const size_t before = flatPositions.size ();

        const long uStart = static_cast<long> (std::floor (uLo / options.spacing));
        const long uEnd = static_cast<long> (std::floor (uHi / options.spacing));
        const long vStart = static_cast<long> (std::floor (vLo / options.spacing));
        const long vEnd = static_cast<long> (std::floor (vHi / options.spacing));

        for (long vi = vStart; vi <= vEnd; ++vi) {
            for (long ui = uStart; ui <= uEnd; ++ui) {
                double offset[2] = { 0.0, 0.0 };
                if (jitterScale > 0.0)
                    CellJitter (static_cast<uint32_t> (face), static_cast<uint32_t> (ui), static_cast<uint32_t> (vi),
                                offset);

                const double su = (static_cast<double> (ui) + 0.5 + offset[0] * jitterScale) * options.spacing;
                const double sv = (static_cast<double> (vi) + 0.5 + offset[1] * jitterScale) * options.spacing;

                const double l0 = ((v1 - v2) * (su - u2) + (u2 - u1) * (sv - v2)) * inverseD;
                const double l1 = ((v2 - v0) * (su - u2) + (u0 - u2) * (sv - v2)) * inverseD;
                const double l2 = 1.0 - l0 - l1;
                if (l0 < 0.0 || l1 < 0.0 || l2 < 0.0)
                    continue;

                flatPositions.push_back (a[0] + su * u[0] + sv * v[0]);
                flatPositions.push_back (a[1] + su * u[1] + sv * v[1]);
                flatPositions.push_back (a[2] + su * u[2] + sv * v[2]);
                flatNormals.push_back (normal[0]);
                flatNormals.push_back (normal[1]);
                flatNormals.push_back (normal[2]);
                if (options.wantLayouts) {
                    grid.cellColumns.push_back (static_cast<uint32_t> (ui - uStart));
                    grid.cellRows.push_back (static_cast<uint32_t> (vi - vStart));
                }
            }
        }

        const bool wasGridded = flatPositions.size () > before;

        // See the header: a face below the grid still gets measured.
        if (flatPositions.size () == before) {
            ++grid.undersizedFaces;
            flatPositions.push_back ((a[0] + b[0] + c[0]) / 3.0);
            flatPositions.push_back ((a[1] + b[1] + c[1]) / 3.0);
            flatPositions.push_back ((a[2] + b[2] + c[2]) / 3.0);
            flatNormals.push_back (normal[0]);
            flatNormals.push_back (normal[1]);
            flatNormals.push_back (normal[2]);
            if (options.wantLayouts) {
                // ⚠️ AN UNDERSIZED FACE HAS NO CELL LATTICE, so it gets cell
                // (0, 0) on a layout marked NOT gridded. A consumer that mapped
                // a point into it anyway would read a texel that means nothing;
                // `gridded` is the flag that says to fall back to the face's
                // single sample instead.
                grid.cellColumns.push_back (0u);
                grid.cellRows.push_back (0u);
            }
        }

        const size_t emitted = (flatPositions.size () - before) / 3;
        if (grid.faces.size () + emitted > options.maxSamples)
            return grid;

        if (options.wantLayouts) {
            FaceLayout& layout = grid.layouts[face];
            for (int axis = 0; axis < 3; ++axis) {
                layout.origin[axis] = a[axis];
                layout.uAxis[axis] = u[axis];
                layout.vAxis[axis] = v[axis];
            }
            layout.uStart = uStart;
            layout.vStart = vStart;
            layout.columns = static_cast<uint32_t> (uEnd - uStart + 1);
            layout.rows = static_cast<uint32_t> (vEnd - vStart + 1);
            // A face that fell back to its centroid has cells on paper but no
            // sample in any of them, so it is not gridded.
            layout.gridded = (flatPositions.size () - before) > 3 || wasGridded;
        }

        // The face area is split between its own samples rather than assigned
        // per cell. Cells clipped by an edge carry less than a full cell of
        // surface, so weighting every sample equally would over-count the
        // boundary; dividing the exact face area keeps the weights summing to
        // the true mesh area, which is what every percentage-of-area figure in
        // the report divides by.
        const double weight = area / static_cast<double> (emitted);
        const uint32_t group = (groups != nullptr) ? groups[face] : 0u;
        for (size_t s = 0; s < emitted; ++s) {
            grid.areas.push_back (weight);
            grid.faces.push_back (static_cast<uint32_t> (face));
            grid.groups.push_back (group);
        }
    }

    grid.positions.resize (flatPositions.size ());
    for (size_t i = 0; i < flatPositions.size (); ++i)
        grid.positions[i] = flatPositions[i] + flatNormals[i] * options.normalOffset;
    grid.normals = std::move (flatNormals);
    grid.valid = true;
    return grid;
}

} // namespace evp::sunstudy
