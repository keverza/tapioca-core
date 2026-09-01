#include "Geometry/Primitives.hpp"

#include <clipper2/clipper.h>

#include <cmath>

namespace geomsrv::engine {
namespace {

void PushVertex (Mesh& mesh, double x, double y, double z, double nx, double ny, double nz)
{
    mesh.vertices.push_back (x);
    mesh.vertices.push_back (y);
    mesh.vertices.push_back (z);
    mesh.normals.push_back (static_cast<float> (nx));
    mesh.normals.push_back (static_cast<float> (ny));
    mesh.normals.push_back (static_cast<float> (nz));
}

void PushTriangle (Mesh& mesh, uint32_t a, uint32_t b, uint32_t c)
{
    mesh.triangles.push_back (a);
    mesh.triangles.push_back (b);
    mesh.triangles.push_back (c);
    // One material and three clear boundary bits per triangle. The wire pass
    // reads triWireEdges to omit internal triangulation edges; a primitive's
    // triangles ARE its faces, so every edge is a real one.
    mesh.triMaterial.push_back (0);
    mesh.triWireEdges.push_back (0x7);
}

void GrowBounds (Mesh& mesh)
{
    // Computed from the vertices rather than from the parameters, so it cannot
    // disagree with the geometry it describes.
    for (std::size_t index = 0; index + 2 < mesh.vertices.size (); index += 3)
        mesh.bounds.Expand (mesh.vertices[index], mesh.vertices[index + 1], mesh.vertices[index + 2]);
}

Vector3 Subtract (const Vector3& left, const Vector3& right)
{
    return { left.x - right.x, left.y - right.y, left.z - right.z };
}

double LengthOf (const Vector3& value)
{
    return std::sqrt (value.x * value.x + value.y * value.y + value.z * value.z);
}

// The outline's own plane, by Newell's method.
//
// Newell rather than "cross the first two edges" because the first two edges of
// a real floor plate are very often collinear or nearly so, and the cross
// product of those is noise: the plane comes out at a random angle and the
// triangulation lands somewhere else entirely. Newell sums over every edge, so
// no single pair can decide the answer.
bool OutlinePlane (const std::vector<Vector3>& outline, Vector3& normal, std::string& error)
{
    Vector3 sum { 0.0, 0.0, 0.0 };
    for (std::size_t index = 0; index < outline.size (); ++index) {
        const Vector3& here = outline[index];
        const Vector3& next = outline[(index + 1) % outline.size ()];
        sum.x += (here.y - next.y) * (here.z + next.z);
        sum.y += (here.z - next.z) * (here.x + next.x);
        sum.z += (here.x - next.x) * (here.y + next.y);
    }
    if (!Unit (sum, normal, error)) {
        error = "the outline is a straight line or has no area, so it has no plane";
        return false;
    }
    return true;
}

// Triangles of a planar outline, as index triples into `outline`.
//
// Clipper2 answers with COORDINATES, not indices, so each triangle corner is
// matched back to the nearest outline vertex. That is exact for the vertices the
// triangulator kept - it does not move them - and the tolerance only has to cover
// the fixed-point round trip Clipper2 does internally.
bool TriangulateOutline (const std::vector<Vector3>& outline, const Vector3& normal, const Vector3& xAxis,
                         const Vector3& yAxis, std::vector<uint32_t>& indices, std::string& error)
{
    (void) normal;
    Clipper2Lib::PathD flat;
    flat.reserve (outline.size ());
    const Vector3& origin = outline.front ();
    for (const Vector3& point : outline) {
        const Vector3 local = Subtract (point, origin);
        flat.push_back ({ Dot (local, xAxis), Dot (local, yAxis) });
    }

    Clipper2Lib::PathsD triangles;
    if (Clipper2Lib::Triangulate (Clipper2Lib::PathsD { flat }, 8, triangles) !=
        Clipper2Lib::TriangulateResult::success) {
        // Self-intersecting outlines are the common cause and the one worth
        // naming: Clipper2 refuses them rather than guessing which loop is the
        // shape, and so does this.
        error = "the outline could not be triangulated - it may cross itself";
        return false;
    }

    indices.clear ();
    for (const Clipper2Lib::PathD& triangle : triangles) {
        if (triangle.size () != 3)
            continue;
        for (const Clipper2Lib::PointD& corner : triangle) {
            std::size_t best = 0;
            double bestDistance = -1.0;
            for (std::size_t index = 0; index < flat.size (); ++index) {
                const double dx = flat[index].x - corner.x;
                const double dy = flat[index].y - corner.y;
                const double distance = dx * dx + dy * dy;
                if (bestDistance < 0.0 || distance < bestDistance) {
                    bestDistance = distance;
                    best = index;
                }
            }
            indices.push_back (static_cast<uint32_t> (best));
        }
    }
    if (indices.empty ()) {
        error = "the outline produced no triangles";
        return false;
    }
    return true;
}

bool Finite (double value)
{
    return std::isfinite (value);
}

} // namespace

bool MakeBox (const Vector3& centre, double width, double depth, double height, Mesh& mesh, std::string& error)
{
    if (!Finite (centre.x) || !Finite (centre.y) || !Finite (centre.z)) {
        error = "the box centre is not a finite point";
        return false;
    }
    if (!Finite (width) || !Finite (depth) || !Finite (height)) {
        error = "a box dimension is not a finite number";
        return false;
    }
    // Refused rather than made positive: a zero or negative extent is far more
    // often a wire into the wrong port than a request for a degenerate solid,
    // and silently building one hides the mistake behind a shape.
    if (width <= 0.0 || depth <= 0.0 || height <= 0.0) {
        error = "a box needs a positive width, depth and height";
        return false;
    }

    mesh = Mesh {};
    const double hx = width * 0.5;
    const double hy = depth * 0.5;
    const double hz = height * 0.5;

    // Six faces, four own vertices each. See the header: shared corners would
    // average three face normals and round the box off.
    struct Face {
        double nx, ny, nz;
        // The four corners, counter-clockwise seen from outside, as signs.
        double sx[4], sy[4], sz[4];
    };
    static const Face faces[6] = {
        // +X                                  -X
        { 1, 0, 0, { 1, 1, 1, 1 }, { -1, 1, 1, -1 }, { -1, -1, 1, 1 } },
        { -1, 0, 0, { -1, -1, -1, -1 }, { 1, -1, -1, 1 }, { -1, -1, 1, 1 } },
        // +Y                                  -Y
        { 0, 1, 0, { 1, -1, -1, 1 }, { 1, 1, 1, 1 }, { -1, -1, 1, 1 } },
        { 0, -1, 0, { -1, 1, 1, -1 }, { -1, -1, -1, -1 }, { -1, -1, 1, 1 } },
        // +Z                                  -Z
        { 0, 0, 1, { -1, 1, 1, -1 }, { -1, -1, 1, 1 }, { 1, 1, 1, 1 } },
        { 0, 0, -1, { -1, -1, 1, 1 }, { -1, 1, 1, -1 }, { -1, -1, -1, -1 } },
    };

    for (const Face& face : faces) {
        const uint32_t base = static_cast<uint32_t> (mesh.VertexCount ());
        for (int corner = 0; corner < 4; ++corner) {
            PushVertex (mesh, centre.x + face.sx[corner] * hx, centre.y + face.sy[corner] * hy,
                        centre.z + face.sz[corner] * hz, face.nx, face.ny, face.nz);
        }
        PushTriangle (mesh, base, base + 1, base + 2);
        PushTriangle (mesh, base, base + 2, base + 3);
    }

    GrowBounds (mesh);
    return true;
}

bool MakeSphere (const Vector3& centre, double radius, int segments, Mesh& mesh, std::string& error)
{
    if (!Finite (centre.x) || !Finite (centre.y) || !Finite (centre.z)) {
        error = "the sphere centre is not a finite point";
        return false;
    }
    if (!Finite (radius) || radius <= 0.0) {
        error = "a sphere needs a positive radius";
        return false;
    }
    // A CEILING, not a clamp, because the cost is quadratic: 256 segments is
    // already 33000 triangles, and a typo of one extra zero would allocate
    // gigabytes inside Archicad before anything could report it.
    if (segments < kMinSphereSegments || segments > kMaxSphereSegments) {
        error = "a sphere needs between " + std::to_string (kMinSphereSegments) + " and " +
                std::to_string (kMaxSphereSegments) + " segments";
        return false;
    }

    mesh = Mesh {};
    const int columns = segments;
    // Half as many bands as columns, because a band spans half the sweep a
    // column does; fewer than two would collapse the sphere into two cones.
    const int rows = columns / 2 < 2 ? 2 : columns / 2;
    const double pi = 3.14159265358979323846;

    // ⚠️ THE SEAM COLUMN IS DUPLICATED - columns + 1 vertices per ring, with the
    // last equal to the first. Wrapping the index instead would share those
    // vertices, which is fine for position and wrong for anything per-vertex that
    // is not periodic; keeping the duplicate is what lets a texture, or a later
    // per-vertex colour, cross the seam without a visible tear.
    for (int row = 0; row <= rows; ++row) {
        const double phi = pi * (static_cast<double> (row) / static_cast<double> (rows));
        const double sinPhi = std::sin (phi);
        const double cosPhi = std::cos (phi);
        for (int column = 0; column <= columns; ++column) {
            const double theta = 2.0 * pi * (static_cast<double> (column) / static_cast<double> (columns));
            // Z up, matching every other mesh here: the poles are on Z.
            const double nx = sinPhi * std::cos (theta);
            const double ny = sinPhi * std::sin (theta);
            const double nz = cosPhi;
            PushVertex (mesh, centre.x + nx * radius, centre.y + ny * radius, centre.z + nz * radius, nx, ny, nz);
        }
    }

    const uint32_t stride = static_cast<uint32_t> (columns + 1);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const uint32_t topLeft = static_cast<uint32_t> (row) * stride + static_cast<uint32_t> (column);
            const uint32_t topRight = topLeft + 1;
            const uint32_t bottomLeft = topLeft + stride;
            const uint32_t bottomRight = bottomLeft + 1;
            // The pole rows degenerate to a point, so their triangles would be
            // zero-area. Skipped rather than emitted: a zero-area triangle has no
            // normal, and a renderer that trusts the normal shades it black.
            if (row != 0)
                PushTriangle (mesh, topLeft, bottomLeft, topRight);
            if (row != rows - 1)
                PushTriangle (mesh, topRight, bottomLeft, bottomRight);
        }
    }

    GrowBounds (mesh);
    return true;
}

bool MakeExtrusion (const std::vector<Vector3>& outline, const Vector3& direction, Mesh& mesh, std::string& error)
{
    if (outline.size () < 3) {
        error = "an extrusion needs an outline of at least three points";
        return false;
    }
    if (!Finite (direction.x) || !Finite (direction.y) || !Finite (direction.z)) {
        error = "the extrusion direction is not a finite vector";
        return false;
    }
    if (LengthOf (direction) <= 0.0) {
        error = "an extrusion needs a direction with a length";
        return false;
    }

    // A repeated last point is an outline that says "closed" by position. Kept
    // out of the wall loop, which closes it itself: leaving it in produces a
    // zero-width quad that shades as a seam.
    std::vector<Vector3> loop = outline;
    if (LengthOf (Subtract (loop.back (), loop.front ())) <= 1e-12)
        loop.pop_back ();
    if (loop.size () < 3) {
        error = "an extrusion needs an outline of at least three distinct points";
        return false;
    }

    Vector3 normal;
    if (!OutlinePlane (loop, normal, error))
        return false;
    Vector3 xAxis;
    if (!Unit (Subtract (loop[1], loop[0]), xAxis, error)) {
        error = "the outline starts with two points in the same place";
        return false;
    }
    Vector3 yAxis;
    if (!Unit (Cross (normal, xAxis), yAxis, error))
        return false;

    std::vector<uint32_t> capIndices;
    if (!TriangulateOutline (loop, normal, xAxis, yAxis, capIndices, error))
        return false;

    mesh = Mesh {};
    const uint32_t count = static_cast<uint32_t> (loop.size ());

    // The near cap, facing away from the sweep, then the far cap facing along it.
    const bool alongNormal = Dot (direction, normal) >= 0.0;
    const Vector3 nearNormal = alongNormal ? Vector3 { -normal.x, -normal.y, -normal.z } : normal;
    const Vector3 farNormal = alongNormal ? normal : Vector3 { -normal.x, -normal.y, -normal.z };

    for (const Vector3& point : loop)
        PushVertex (mesh, point.x, point.y, point.z, nearNormal.x, nearNormal.y, nearNormal.z);
    for (const Vector3& point : loop) {
        PushVertex (mesh, point.x + direction.x, point.y + direction.y, point.z + direction.z, farNormal.x, farNormal.y,
                    farNormal.z);
    }

    for (std::size_t index = 0; index + 2 < capIndices.size (); index += 3) {
        // Reversed on the near cap, so the two ends face opposite ways. See the
        // header: matching winding at both ends is inside-out at one of them.
        PushTriangle (mesh, capIndices[index + 2], capIndices[index + 1], capIndices[index]);
        PushTriangle (mesh, count + capIndices[index], count + capIndices[index + 1], count + capIndices[index + 2]);
    }

    // The walls carry their own vertices, because their normals are horizontal
    // and the caps' are not: sharing would average the two and round the edge off.
    for (uint32_t index = 0; index < count; ++index) {
        const Vector3& a = loop[index];
        const Vector3& b = loop[(index + 1) % count];
        Vector3 edge = Subtract (b, a);
        Vector3 wallNormal;
        if (!Unit (Cross (edge, direction), wallNormal, error))
            continue; // an edge parallel to the sweep has no wall
        if (!alongNormal)
            wallNormal = { -wallNormal.x, -wallNormal.y, -wallNormal.z };

        const uint32_t base = static_cast<uint32_t> (mesh.VertexCount ());
        PushVertex (mesh, a.x, a.y, a.z, wallNormal.x, wallNormal.y, wallNormal.z);
        PushVertex (mesh, b.x, b.y, b.z, wallNormal.x, wallNormal.y, wallNormal.z);
        PushVertex (mesh, b.x + direction.x, b.y + direction.y, b.z + direction.z, wallNormal.x, wallNormal.y,
                    wallNormal.z);
        PushVertex (mesh, a.x + direction.x, a.y + direction.y, a.z + direction.z, wallNormal.x, wallNormal.y,
                    wallNormal.z);
        PushTriangle (mesh, base, base + 1, base + 2);
        PushTriangle (mesh, base, base + 2, base + 3);
    }

    GrowBounds (mesh);
    return true;
}

bool MakeLoft (const std::vector<Vector3>& from, const std::vector<Vector3>& to, bool closed, Mesh& mesh,
               std::string& error)
{
    if (from.size () < 2 || to.size () < 2) {
        error = "a loft needs two curves of at least two points each";
        return false;
    }
    if (from.size () != to.size ()) {
        error = "a loft needs two curves with the same number of points - use Divide Curve to match them";
        return false;
    }

    mesh = Mesh {};
    const std::size_t count = from.size ();
    const std::size_t spans = closed ? count : count - 1;
    for (std::size_t index = 0; index < spans; ++index) {
        const std::size_t next = (index + 1) % count;
        const Vector3& a = from[index];
        const Vector3& b = from[next];
        const Vector3& c = to[next];
        const Vector3& d = to[index];

        Vector3 quadNormal;
        if (!Unit (Cross (Subtract (b, a), Subtract (d, a)), quadNormal, error)) {
            // A degenerate span - two coincident points on one curve - is skipped
            // rather than failing the loft: it contributes no surface, and one
            // repeated point should not lose the other fifty spans.
            continue;
        }

        const uint32_t base = static_cast<uint32_t> (mesh.VertexCount ());
        PushVertex (mesh, a.x, a.y, a.z, quadNormal.x, quadNormal.y, quadNormal.z);
        PushVertex (mesh, b.x, b.y, b.z, quadNormal.x, quadNormal.y, quadNormal.z);
        PushVertex (mesh, c.x, c.y, c.z, quadNormal.x, quadNormal.y, quadNormal.z);
        PushVertex (mesh, d.x, d.y, d.z, quadNormal.x, quadNormal.y, quadNormal.z);
        PushTriangle (mesh, base, base + 1, base + 2);
        PushTriangle (mesh, base, base + 2, base + 3);
    }

    if (mesh.triangles.empty ()) {
        error = "the two curves produced no surface between them";
        return false;
    }
    GrowBounds (mesh);
    return true;
}

} // namespace geomsrv::engine
