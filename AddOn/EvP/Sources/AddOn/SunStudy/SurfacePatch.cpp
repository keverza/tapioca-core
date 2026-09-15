#include "SunStudy/SurfacePatch.hpp"

#include "SunStudy/SunStudySampler.hpp" // TriangleNormal

#include <algorithm>
#include <cmath>
#include <map>

namespace evp::sunstudy {

namespace {

Vec3 VertexAt (const double* vertices, uint32_t index)
{
    return Vec3 { vertices[index * 3 + 0], vertices[index * 3 + 1], vertices[index * 3 + 2] };
}

double Dot (const Vec3& a, const Vec3& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 Sub (const Vec3& a, const Vec3& b)
{
    return Vec3 { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
}

Vec3 Cross (const Vec3& a, const Vec3& b)
{
    return Vec3 { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}

bool Normalise (Vec3& v)
{
    const double length = std::sqrt (Dot (v, v));
    if (!(length > 1e-12))
        return false;
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
    return true;
}

// An undirected edge, as the SOURCE vertex pair. ⚠️ ORDERED SO THE TWO
// TRIANGLES SHARING IT PRODUCE THE SAME KEY: a triangle walks its edges in
// winding order, so neighbours see (a,b) and (b,a) for the same edge.
std::pair<uint32_t, uint32_t> EdgeKey (uint32_t a, uint32_t b)
{
    return a < b ? std::make_pair (a, b) : std::make_pair (b, a);
}

struct Facet {
    Vec3 normal;
    Vec3 centroid;
    double area = 0.0;
    uint32_t group = 0;
    bool usable = false;
};

// Same plane, same side, same element.
bool SameSurface (const Facet& a, const Facet& b, double cosTolerance, double planeToleranceM)
{
    if (a.group != b.group)
        return false;
    // ⚠️ THE DOT PRODUCT IS NOT ABS'D. Two faces of a thin slab are antiparallel
    // and are two surfaces, not one -- a patch that merged them would grid the
    // front and back of a wall into a single domain and average them together.
    if (Dot (a.normal, b.normal) < cosTolerance)
        return false;
    // Parallel is not coplanar: the neighbour's centroid must lie ON this plane.
    const double offset = std::fabs (Dot (Sub (b.centroid, a.centroid), a.normal));
    return offset <= planeToleranceM;
}

// ---- convex polygon clipping, in the patch's 2D frame ---------------------
//
// Sutherland-Hodgman against the four half-planes of an axis-aligned cell. A
// triangle is convex and a rectangle is convex, so the intersection is a convex
// polygon of at most seven vertices and the algorithm is exact -- no general
// polygon boolean, no winding rules, no special cases.
using Poly2D = std::vector<double>; // x,y pairs

void ClipHalfPlane (const Poly2D& in, Poly2D& out, int axis, double limit, bool keepBelow)
{
    out.clear ();
    const size_t corners = in.size () / 2;
    if (corners == 0)
        return;
    const auto inside = [&] (size_t i) {
        const double value = in[i * 2 + axis];
        return keepBelow ? value <= limit : value >= limit;
    };
    for (size_t i = 0; i < corners; ++i) {
        const size_t j = (i + 1) % corners;
        const bool a = inside (i);
        const bool b = inside (j);
        if (a) {
            out.push_back (in[i * 2 + 0]);
            out.push_back (in[i * 2 + 1]);
        }
        if (a == b)
            continue;
        // The edge crosses the boundary: emit the crossing point.
        const double ax = in[i * 2 + 0], ay = in[i * 2 + 1];
        const double bx = in[j * 2 + 0], by = in[j * 2 + 1];
        const double denominator = (axis == 0 ? (bx - ax) : (by - ay));
        if (std::fabs (denominator) < 1e-15)
            continue;
        const double t = (limit - (axis == 0 ? ax : ay)) / denominator;
        out.push_back (ax + (bx - ax) * t);
        out.push_back (ay + (by - ay) * t);
    }
}

// Signed area and centroid of a simple polygon. ⚠️ THE SIGN IS TAKEN OUT WITH
// fabs AT THE END, and it has to be: the source triangles' winding is whatever
// Archicad's tessellation produced, and a patch containing both orientations
// would otherwise have pieces that SUBTRACT from its own area.
double PolygonAreaCentroid (const Poly2D& poly, double& cx, double& cy)
{
    cx = cy = 0.0;
    const size_t corners = poly.size () / 2;
    if (corners < 3)
        return 0.0;
    double twiceArea = 0.0;
    for (size_t i = 0; i < corners; ++i) {
        const size_t j = (i + 1) % corners;
        const double xi = poly[i * 2 + 0], yi = poly[i * 2 + 1];
        const double xj = poly[j * 2 + 0], yj = poly[j * 2 + 1];
        const double cross = xi * yj - xj * yi;
        twiceArea += cross;
        cx += (xi + xj) * cross;
        cy += (yi + yj) * cross;
    }
    if (std::fabs (twiceArea) < 1e-18)
        return 0.0;
    cx /= (3.0 * twiceArea);
    cy /= (3.0 * twiceArea);
    return std::fabs (twiceArea) * 0.5;
}

// The area of `triangle` inside the cell square, and its centroid. Returns 0
// when they do not overlap.
double ClipTriangleToCell (const double t[6], double minU, double minV, double maxU, double maxV, double& cx,
                           double& cy)
{
    Poly2D a { t[0], t[1], t[2], t[3], t[4], t[5] };
    Poly2D b;
    ClipHalfPlane (a, b, 0, minU, false);
    ClipHalfPlane (b, a, 0, maxU, true);
    ClipHalfPlane (a, b, 1, minV, false);
    ClipHalfPlane (b, a, 1, maxV, true);
    return PolygonAreaCentroid (a, cx, cy);
}

// Barycentric point-in-triangle, in the patch's 2D frame.
bool PointInTriangle2D (double px, double py, const double t[6])
{
    const double x0 = t[0], y0 = t[1], x1 = t[2], y1 = t[3], x2 = t[4], y2 = t[5];
    const double denominator = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (std::fabs (denominator) < 1e-15)
        return false; // degenerate in this projection
    const double a = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / denominator;
    const double b = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / denominator;
    const double c = 1.0 - a - b;
    // ⚠️ THE EPSILON IS INCLUSIVE AND IT MATTERS ON A SHARED EDGE. A cell centre
    // that lands exactly on the diagonal of a triangulated quad belongs to the
    // patch either way; excluding it would punch a line of holes down the middle
    // of a flat surface -- which is the very artefact patches exist to remove.
    const double epsilon = -1e-9;
    return a >= epsilon && b >= epsilon && c >= epsilon;
}

} // namespace

std::vector<SurfacePatch> BuildSurfacePatches (const double* vertices, size_t vertexCount, const uint32_t* triangles,
                                               size_t faceCount, const uint32_t* groups,
                                               const SurfacePatchOptions& options)
{
    std::vector<SurfacePatch> patches;
    if (vertices == nullptr || triangles == nullptr || faceCount == 0 || vertexCount == 0)
        return patches;

    const double cosTolerance = std::cos (options.coplanarDegrees * 3.14159265358979323846 / 180.0);

    // ---- one pass for the per-triangle facts ------------------------------
    std::vector<Facet> facets (faceCount);
    for (size_t face = 0; face < faceCount; ++face) {
        const uint32_t ia = triangles[face * 3 + 0];
        const uint32_t ib = triangles[face * 3 + 1];
        const uint32_t ic = triangles[face * 3 + 2];
        if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount)
            continue;
        const Vec3 a = VertexAt (vertices, ia);
        const Vec3 b = VertexAt (vertices, ib);
        const Vec3 c = VertexAt (vertices, ic);
        double area = 0.0;
        const Vec3 normal = TriangleNormal (a, b, c, &area);
        if (!(area > 0.0))
            continue; // a sliver; see the header
        Facet& facet = facets[face];
        facet.normal = normal;
        facet.centroid = Vec3 { (a[0] + b[0] + c[0]) / 3.0, (a[1] + b[1] + c[1]) / 3.0, (a[2] + b[2] + c[2]) / 3.0 };
        facet.area = area;
        facet.group = groups != nullptr ? groups[face] : 0;
        facet.usable = true;
    }

    // ---- edge -> the triangles that use it --------------------------------
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> edges;
    for (size_t face = 0; face < faceCount; ++face) {
        if (!facets[face].usable)
            continue;
        for (int corner = 0; corner < 3; ++corner) {
            const uint32_t a = triangles[face * 3 + corner];
            const uint32_t b = triangles[face * 3 + (corner + 1) % 3];
            edges[EdgeKey (a, b)].push_back (static_cast<uint32_t> (face));
        }
    }

    // ---- flood fill ---------------------------------------------------------
    // ⚠️ ASCENDING FROM THE LOWEST UNASSIGNED FACE, AND THE STACK IS SORTED ON
    // OUTPUT. Determinism is a requirement, not a nicety: the atlas packs by
    // patch, so a patch set that came out in a different order would repack the
    // atlas and silently invalidate every texture coordinate already handed out.
    std::vector<bool> taken (faceCount, false);
    std::vector<uint32_t> stack;
    for (size_t seed = 0; seed < faceCount; ++seed) {
        if (taken[seed] || !facets[seed].usable)
            continue;

        SurfacePatch patch;
        patch.group = facets[seed].group;
        taken[seed] = true;
        stack.clear ();
        stack.push_back (static_cast<uint32_t> (seed));

        while (!stack.empty ()) {
            const uint32_t face = stack.back ();
            stack.pop_back ();
            patch.triangles.push_back (face);

            for (int corner = 0; corner < 3; ++corner) {
                const uint32_t a = triangles[face * 3 + corner];
                const uint32_t b = triangles[face * 3 + (corner + 1) % 3];
                const auto found = edges.find (EdgeKey (a, b));
                if (found == edges.end ())
                    continue;
                for (const uint32_t neighbour : found->second) {
                    if (taken[neighbour])
                        continue;
                    // ⚠️ COMPARED AGAINST THE SEED, NOT AGAINST THE NEIGHBOUR WE
                    // ARRIVED FROM. Chaining "close enough to my neighbour" walks
                    // a tessellated curve one degree at a time and ends up
                    // calling a cylinder one flat patch.
                    if (!SameSurface (facets[seed], facets[neighbour], cosTolerance, options.planeToleranceM))
                        continue;
                    taken[neighbour] = true;
                    stack.push_back (neighbour);
                }
            }
        }

        std::sort (patch.triangles.begin (), patch.triangles.end ());

        // ---- the patch's own frame ------------------------------------------
        patch.normal[0] = facets[seed].normal[0];
        patch.normal[1] = facets[seed].normal[1];
        patch.normal[2] = facets[seed].normal[2];
        for (const uint32_t face : patch.triangles)
            patch.area += facets[face].area;

        // ⚠️ u RUNS ALONG THE FIRST EDGE OF THE LOWEST-NUMBERED TRIANGLE. It has
        // to come from the GEOMETRY rather than from a world axis: a wall's grid
        // should run along the wall, and a world-X basis would lay a diagonal
        // lattice across every facade that is not axis-aligned. Lowest-numbered
        // for determinism.
        const uint32_t first = patch.triangles.front ();
        const Vec3 a = VertexAt (vertices, triangles[first * 3 + 0]);
        const Vec3 b = VertexAt (vertices, triangles[first * 3 + 1]);
        const Vec3 normal = facets[seed].normal;
        Vec3 uAxis = Sub (b, a);
        // Project the edge into the plane, then normalise.
        const double along = Dot (uAxis, normal);
        uAxis = Vec3 { uAxis[0] - normal[0] * along, uAxis[1] - normal[1] * along, uAxis[2] - normal[2] * along };
        if (!Normalise (uAxis)) {
            // The first edge was parallel to the normal, which a non-degenerate
            // triangle cannot manage -- but a fallback beats a NaN basis.
            uAxis = std::fabs (normal[2]) < 0.9 ? Vec3 { 0.0, 0.0, 1.0 } : Vec3 { 1.0, 0.0, 0.0 };
            Vec3 projected = Sub (uAxis, Vec3 { normal[0] * Dot (uAxis, normal), normal[1] * Dot (uAxis, normal),
                                                normal[2] * Dot (uAxis, normal) });
            Normalise (projected);
            uAxis = projected;
        }
        Vec3 vAxis = Cross (normal, uAxis);
        Normalise (vAxis);

        // ---- origin at the patch's minimum corner ---------------------------
        // Every patch coordinate is then >= 0, which is what lets the lattice be
        // indexed from 0 with no negative-cell special case.
        double minU = 0.0, minV = 0.0, maxU = 0.0, maxV = 0.0;
        bool haveBounds = false;
        for (const uint32_t face : patch.triangles) {
            for (int corner = 0; corner < 3; ++corner) {
                const Vec3 p = VertexAt (vertices, triangles[face * 3 + corner]);
                const Vec3 relative = Sub (p, a);
                const double u = Dot (relative, uAxis);
                const double v = Dot (relative, vAxis);
                if (!haveBounds) {
                    minU = maxU = u;
                    minV = maxV = v;
                    haveBounds = true;
                }
                else {
                    minU = std::min (minU, u);
                    maxU = std::max (maxU, u);
                    minV = std::min (minV, v);
                    maxV = std::max (maxV, v);
                }
            }
        }
        patch.origin[0] = a[0] + uAxis[0] * minU + vAxis[0] * minV;
        patch.origin[1] = a[1] + uAxis[1] * minU + vAxis[1] * minV;
        patch.origin[2] = a[2] + uAxis[2] * minU + vAxis[2] * minV;
        patch.uExtent = maxU - minU;
        patch.vExtent = maxV - minV;
        patch.uAxis[0] = uAxis[0];
        patch.uAxis[1] = uAxis[1];
        patch.uAxis[2] = uAxis[2];
        patch.vAxis[0] = vAxis[0];
        patch.vAxis[1] = vAxis[1];
        patch.vAxis[2] = vAxis[2];

        patches.push_back (std::move (patch));
    }

    return patches;
}

PatchGrid BuildPatchGrid (const SurfacePatch& patch, const double* vertices, const uint32_t* triangles, double spacing)
{
    PatchGrid grid;
    if (vertices == nullptr || triangles == nullptr || patch.triangles.empty () || !(spacing > 0.0))
        return grid;

    const Vec3 origin { patch.origin[0], patch.origin[1], patch.origin[2] };
    const Vec3 uAxis { patch.uAxis[0], patch.uAxis[1], patch.uAxis[2] };
    const Vec3 vAxis { patch.vAxis[0], patch.vAxis[1], patch.vAxis[2] };

    // The patch's triangles, once, in patch 2D. Reused by every cell test.
    std::vector<double> flat;
    flat.reserve (patch.triangles.size () * 6);
    for (const uint32_t face : patch.triangles) {
        for (int corner = 0; corner < 3; ++corner) {
            const Vec3 p = VertexAt (vertices, triangles[face * 3 + corner]);
            const Vec3 relative = Sub (p, origin);
            flat.push_back (Dot (relative, uAxis));
            flat.push_back (Dot (relative, vAxis));
        }
    }

    // Still used by the sub-grid fallback below.
    const auto inside = [&flat] (double u, double v) {
        for (size_t t = 0; t + 5 < flat.size (); t += 6) {
            if (PointInTriangle2D (u, v, &flat[t]))
                return true;
        }
        return false;
    };

    grid.columns = static_cast<uint32_t> (std::max (1.0, std::ceil (patch.uExtent / spacing)));
    grid.rows = static_cast<uint32_t> (std::max (1.0, std::ceil (patch.vExtent / spacing)));

    const auto worldOf = [&] (double u, double v, double out[3]) {
        out[0] = origin[0] + uAxis[0] * u + vAxis[0] * v;
        out[1] = origin[1] + uAxis[1] * u + vAxis[1] * v;
        out[2] = origin[2] + uAxis[2] * u + vAxis[2] * v;
    };

    const double full = spacing * spacing;
    for (uint32_t row = 0; row < grid.rows; ++row) {
        for (uint32_t column = 0; column < grid.columns; ++column) {
            const double minU = column * spacing;
            const double minV = row * spacing;
            const double maxU = minU + spacing;
            const double maxV = minV + spacing;

            // ---- sum the pieces of every triangle that reaches this cell -----
            //
            // ⚠️ SUMMED, NOT UNIONED, AND THAT IS ONLY CORRECT BECAUSE THE SOURCE
            // TRIANGLES PARTITION THE SURFACE. They meet edge to edge and do not
            // overlap, so their clipped areas add with no double-counting -- which
            // is exactly what makes a union polygon unnecessary here.
            double area = 0.0;
            double weightedU = 0.0;
            double weightedV = 0.0;
            for (size_t t = 0; t + 5 < flat.size (); t += 6) {
                double cx = 0.0, cy = 0.0;
                const double piece = ClipTriangleToCell (&flat[t], minU, minV, maxU, maxV, cx, cy);
                if (piece <= 0.0)
                    continue;
                area += piece;
                weightedU += cx * piece;
                weightedV += cy * piece;
            }
            // ⚠️ AN UNCOVERED CELL EMITS NOTHING AT ALL. Emitting it with area 0
            // would put a sample in mid-air carrying no weight, which every later
            // total would have to remember to skip.
            if (!(area > 1e-12))
                continue;

            PatchCell cell;
            cell.column = column;
            cell.row = row;
            cell.area = area;
            // ⚠️ THE AREA-WEIGHTED CENTROID OF THE COVERED PART, NOT THE MIDDLE OF
            // THE SQUARE. The middle of a cell that is nine-tenths outside the
            // surface is a point in mid-air; a ray fired from there measures the
            // sunlight somewhere the building is not.
            const double sampleU = weightedU / area;
            const double sampleV = weightedV / area;
            worldOf (sampleU, sampleV, cell.centre);
            // Interior to within rounding: a cell the boundary does not cut.
            cell.interior = area >= full * (1.0 - 1e-9);
            grid.cells.push_back (cell);
        }
    }

    // ⚠️ A PATCH BELOW THE GRID EMITS ITS CENTROID RATHER THAN NOTHING. See the
    // header: coverage is a correctness property, and a sub-grid mullion dropped
    // from the study leaves every total looking plausible.
    if (grid.cells.empty ()) {
        double centroidU = 0.0;
        double centroidV = 0.0;
        size_t corners = 0;
        for (size_t t = 0; t + 1 < flat.size (); t += 2) {
            centroidU += flat[t];
            centroidV += flat[t + 1];
            ++corners;
        }
        if (corners > 0) {
            centroidU /= double (corners);
            centroidV /= double (corners);
            PatchCell cell;
            cell.column = 0;
            cell.row = 0;
            worldOf (centroidU, centroidV, cell.centre);
            cell.area = patch.area;
            cell.interior = false;
            grid.cells.push_back (cell);
            grid.fromCentroid = true;
        }
    }

    return grid;
}

} // namespace evp::sunstudy
