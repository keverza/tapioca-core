#include "SurfaceCoverage.hpp"
#include "SliceEngine.hpp"

#include <algorithm>
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

bool TriangulateOnGrid (const Clipper2Lib::PathsD& loops, double grid, Clipper2Lib::PathsD& triangles,
                        std::string& error)
{
    namespace cp = Clipper2Lib;
    triangles.clear ();
    error.clear ();
    if (!std::isfinite (grid) || grid <= 0) {
        error = "Invalid fragment triangulation grid.";
        return false;
    }
    // ClipperD's lattice is binary, while Triangulate(PathsD) re-snaps to a
    // decimal lattice. Keep its exact integer boundary, expanded fourfold because
    // the triangulator ignores triangles with a one-unit edge. Expansion creates
    // no new vertices or area and keeps one-grid strips instead of deleting them.
    constexpr double expansion = 4;
    int scaleError = 0;
    const cp::Paths64 boundary = cp::ScalePaths<int64_t, double> (loops, expansion / grid, scaleError);
    cp::Paths64 integerTriangles;
    if (scaleError || cp::Triangulate (boundary, integerTriangles) != cp::TriangulateResult::success ||
        integerTriangles.empty ()) {
        error = "Exposed fragment triangulation failed; no partial result.";
        return false;
    }
    auto prepared = cp::ScalePaths<double, int64_t> (integerTriangles, grid / expansion, scaleError);
    if (scaleError) {
        error = "Exposed fragment triangle coordinate scaling failed.";
        return false;
    }
    triangles = std::move (prepared);
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
