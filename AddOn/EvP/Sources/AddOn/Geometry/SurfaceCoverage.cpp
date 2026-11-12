#include "SurfaceCoverage.hpp"
#include "SliceEngine.hpp"
#include "PlanarTriangulation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace geomsrv::surfacecoverage {
namespace {
V Sub (const V& a, const V& b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
V Scale (const V& v, double scale)
{
    return { v.x * scale, v.y * scale, v.z * scale };
}
} // namespace

Clipper2Lib::PointD Frame::Project (const V& point) const
{
    const V delta = Sub (point, origin);
    return { engine::Dot (delta, u), engine::Dot (delta, v) };
}

V Frame::Lift (const Clipper2Lib::PointD& point) const
{
    return { origin.x + point.x * u.x + point.y * v.x, origin.y + point.x * u.y + point.y * v.y,
             origin.z + point.x * u.z + point.y * v.z };
}

bool MakeFrame (const V& a, const V& b, const V& c, Frame& frame)
{
    const V edge = Sub (b, a), cross = engine::Cross (edge, Sub (c, a));
    const double length = std::sqrt (engine::Dot (cross, cross));
    const double edgeLength = std::sqrt (engine::Dot (edge, edge));
    if (!std::isfinite (length) || !std::isfinite (edgeLength) || length <= 1e-12 || edgeLength <= 1e-12)
        return false;
    frame.origin = a;
    frame.normal = Scale (cross, 1 / length);
    frame.u = Scale (edge, 1 / edgeLength);
    frame.v = engine::Cross (frame.normal, frame.u);
    return true;
}

bool Boolean (Clipper2Lib::ClipType operation, const Clipper2Lib::PathsD& subject, const Clipper2Lib::PathsD& clip,
              Clipper2Lib::PathsD& paths, std::string& error, Clipper2Lib::FillRule fill)
{
    Clipper2Lib::ClipperD engine (kPrecision);
    engine.AddSubject (subject);
    engine.AddClip (clip);
    if (!engine.Execute (operation, fill, paths) || engine.ErrorCode ()) {
        paths.clear ();
        error = "Planar surface boolean failed; no partial result reported.";
        return false;
    }
    return true;
}

namespace {
namespace cp = Clipper2Lib;

// One outer ring and its holes, triangulated on the integer lattice; the
// triangles appended in lattice units, counter-clockwise.
bool CoverPolygon (const cp::PolyPath64& outer, std::vector<std::array<double, 6>>& out)
{
    std::vector<std::vector<double>> xy;
    const auto ring = [&xy] (const cp::Path64& path) {
        auto& flat = xy.emplace_back ();
        flat.reserve (path.size () * 2);
        for (const auto& point : path)
            flat.insert (flat.end (), { double (point.x), double (point.y) });
    };
    ring (outer.Polygon ());
    for (const auto& hole : outer)
        ring (hole->Polygon ());
    std::vector<engine::PlanarRing> rings;
    for (const auto& flat : xy)
        rings.push_back ({ flat.data (), flat.size () / 2 });
    engine::PlanarTriangulation cover;
    std::string why;
    if (!engine::TriangulatePlanar (rings, false, cover, why) || cover.cover == engine::PlanarCover::Approximate)
        return false;
    std::vector<double> points;
    for (const auto& flat : xy)
        points.insert (points.end (), flat.begin (), flat.end ());
    points.insert (points.end (), cover.generatedXY.begin (), cover.generatedXY.end ());
    for (size_t t = 0; t + 2 < cover.triangles.size (); t += 3) {
        std::array<double, 6> triangle;
        for (size_t c = 0; c < 3; ++c) {
            const size_t index = cover.triangles[t + c];
            if (2 * index + 1 >= points.size ())
                return false;
            triangle[2 * c] = points[2 * index];
            triangle[2 * c + 1] = points[2 * index + 1];
        }
        const double area = (triangle[2] - triangle[0]) * (triangle[5] - triangle[1]) -
                            (triangle[4] - triangle[0]) * (triangle[3] - triangle[1]);
        if (area == 0.0)
            continue; // an empty triangle covers nothing
        if (area < 0.0)
            std::swap (triangle[2], triangle[4]), std::swap (triangle[3], triangle[5]);
        out.push_back (triangle);
    }
    return true;
}

// Every polygon of a ring tree: outer rings, their holes, islands in holes.
bool CoverTree (const cp::PolyPath64& node, std::vector<std::array<double, 6>>& out)
{
    for (const auto& child : node) {
        if (!child->IsHole () && !CoverPolygon (*child, out))
            return false;
        if (!CoverTree (*child, out))
            return false;
    }
    return true;
}
} // namespace

bool TriangulateOnGrid (const Clipper2Lib::PathsD& loops, double grid, Clipper2Lib::PathsD& triangles,
                        std::string& error)
{
    triangles.clear ();
    error.clear ();
    if (!std::isfinite (grid) || grid <= 0) {
        error = "Invalid fragment triangulation grid.";
        return false;
    }
    // ClipperD's lattice is binary: keep its exact integer boundary, expanded
    // fourfold so a one-grid strip is still a strip of real triangles.
    constexpr double expansion = 4;
    int scaleError = 0;
    const cp::Paths64 boundary = cp::ScalePaths<int64_t, double> (loops, expansion / grid, scaleError);
    if (scaleError) {
        error = "Exposed fragment triangulation failed; no partial result.";
        return false;
    }
    // ⚠️ NOT CLIPPER2'S TRIANGULATOR (#168). Clipper2 2.0.1's Triangulate never
    // returns on some won-lit fragments (a loop with nearly collinear runs; the
    // captured case is a regression test), and a call that never returns cannot
    // be cancelled: the solar cut hung for good. Clipper2 stays the boolean
    // authority and gives the ring hierarchy; the pinned planar triangulator
    // (earstrip, bounded, already behind the patch triangulation) covers each
    // outer ring with its holes, on the same lattice.
    //
    // REPAIRED ONLY WHEN REFUSED. Nearly coincident shadow edges leave
    // hairline spikes -- a loop running out and back a few micrometres apart.
    // A refused fragment is simplified by a growing tolerance (a tenth of a
    // micrometre up to half a millimetre, in model metres whatever the grid)
    // until it covers; one such surface used to blank the whole won-lit result.
    std::vector<std::array<double, 6>> covered;
    bool done = false;
    for (double metres = 0.0; !done && metres <= 5.0e-4; metres = metres == 0.0 ? 1.0e-7 : metres * 8.0) {
        cp::Paths64 rings = cp::Union (boundary, cp::FillRule::NonZero);
        if (metres > 0.0)
            rings = cp::Union (cp::SimplifyPaths (rings, (std::max) (1.0, metres * expansion / grid), true),
                               cp::FillRule::NonZero);
        if (rings.empty ())
            return true; // nothing but hairlines: nothing to fill
        cp::Clipper64 clipper;
        clipper.AddSubject (rings);
        cp::PolyTree64 tree;
        if (!clipper.Execute (cp::ClipType::Union, cp::FillRule::NonZero, tree))
            continue;
        covered.clear ();
        done = CoverTree (tree, covered) && !covered.empty ();
    }
    if (!done) {
        error = "Exposed fragment triangulation failed; no partial result.";
        return false;
    }
    const double unit = grid / expansion;
    triangles.reserve (covered.size ());
    for (const auto& t : covered)
        triangles.push_back (
            { { t[0] * unit, t[1] * unit }, { t[2] * unit, t[3] * unit }, { t[4] * unit, t[5] * unit } });
    return true;
}

bool Section (const Mesh& body, const Frame& frame, Clipper2Lib::PathsD& paths, std::string& error)
{
    namespace cp = Clipper2Lib;
    paths.clear ();
    error.clear ();
    std::vector<double> local;
    local.reserve (body.vertices.size ());
    double low = std::numeric_limits<double>::infinity (), high = -low;
    for (size_t i = 0; i < body.VertexCount (); ++i) {
        const V delta =
            Sub ({ body.vertices[i * 3], body.vertices[i * 3 + 1], body.vertices[i * 3 + 2] }, frame.origin);
        double z = engine::Dot (delta, frame.normal);
        if (std::abs (z) <= kPlaneTolerance)
            z = -kPlaneTolerance;
        low = std::min (low, z);
        high = std::max (high, z);
        local.insert (local.end (), { engine::Dot (delta, frame.u), engine::Dot (delta, frame.v), z });
    }
    if (low >= 0 || high <= 0)
        return true;
    cp::PathsD section;
    for (const auto& loop : SliceMesh (local.data (), body.VertexCount (), body.triangles.data (),
                                       body.TriangleCount (), 0, kPlaneTolerance)) {
        if (!loop.closed) {
            error = "Open surface cover cut; no partial result reported.";
            return false;
        }
        cp::PathD path;
        for (size_t i = 0; i + 1 < loop.PointCount (); ++i)
            path.emplace_back (loop.pts[i * 3], loop.pts[i * 3 + 1]);
        if (path.size () >= 3)
            section.push_back (std::move (path));
    }
    return Boolean (cp::ClipType::Union, section, {}, paths, error, cp::FillRule::EvenOdd);
}
} // namespace geomsrv::surfacecoverage
