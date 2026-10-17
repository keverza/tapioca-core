#include "ExteriorSurfaceRegion.hpp"
#include "ExposedSurfaceInternal.hpp"
#include "SurfaceSourceMap.hpp"
#include <algorithm>
#include <cmath>
#include <deque>

namespace geomsrv::exposed {
namespace {
namespace cp = Clipper2Lib;
using V = engine::Vector3;
constexpr double kTolerance = surfacecoverage::kPlaneTolerance;
struct Candidate {
    FaceKey key;
    surfacemap::SourceFace source;
    cp::PathsD paths;
};
V Sub (const V& a, const V& b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
V Point (const std::array<double, 3>& p)
{
    return { p[0], p[1], p[2] };
}
bool Fail (std::string& error, const char* reason)
{
    error = reason;
    return false;
}

bool Contains (const Candidate& face, const surfacecoverage::Frame& frame, const V& point)
{
    const auto p = frame.Project (point);
    int winding = 0;
    for (const auto& path : face.paths) {
        const auto location = cp::PointInPolygon (p, path);
        if (location == cp::PointInPolygonResult::IsOn)
            return true;
        if (location == cp::PointInPolygonResult::IsInside)
            winding += cp::Area (path) > 0 ? 1 : -1;
    }
    return winding != 0;
}

bool Adjacent (const Candidate& a, const Candidate& b, detail::Work& work, bool& connected, std::string& error)
{
    connected = false;
    if (!work.Check (1, error))
        return false;
    cp::PathsD intersection;
    if (!surfacecoverage::Boolean (cp::ClipType::Intersection, a.paths, b.paths, intersection, error))
        return false;
    if (std::abs (cp::Area (intersection)) > 1e-12) {
        connected = true;
        return true;
    }
    for (const auto& left : a.paths)
        for (const auto& right : b.paths)
            for (size_t i = 0; i < left.size (); ++i)
                for (size_t j = 0; j < right.size (); ++j) {
                    if (!work.Check (1, error))
                        return false;
                    const auto p = left[i], q = left[(i + 1) % left.size ()];
                    const auto r = right[j], s = right[(j + 1) % right.size ()];
                    const double dx = q.x - p.x, dy = q.y - p.y, length = std::hypot (dx, dy);
                    if (length <= kTolerance)
                        continue;
                    const auto distance = [&] (const cp::PointD& v) {
                        return std::abs (dx * (v.y - p.y) - dy * (v.x - p.x)) / length;
                    };
                    if (distance (r) > kTolerance || distance (s) > kTolerance)
                        continue;
                    const double u = ((r.x - p.x) * dx + (r.y - p.y) * dy) / length;
                    const double v = ((s.x - p.x) * dx + (s.y - p.y) * dy) / length;
                    if (std::min (length, std::max (u, v)) - std::max (0.0, std::min (u, v)) > kTolerance) {
                        connected = true;
                        return true;
                    }
                }
    return true;
}

bool Candidates (const Snapshot& snapshot, const std::set<size_t>& targets, const ExteriorSeed& seed,
                 surfacecoverage::Frame& frame, std::vector<Candidate>& candidates, detail::Work& work,
                 std::string& error)
{
    const V origin = Point (seed.point), n = Point (seed.normal);
    const double length = std::sqrt (engine::Dot (n, n));
    if (!std::isfinite (length) || length < 1e-12)
        return Fail (error, "Exterior seed normal is invalid.");
    frame.origin = origin;
    frame.normal = { n.x / length, n.y / length, n.z / length };
    const V helper = std::abs (frame.normal.x) < .9 ? V { 1, 0, 0 } : V { 0, 1, 0 };
    const V axis = engine::Cross (frame.normal, helper);
    const double axisLength = std::sqrt (engine::Dot (axis, axis));
    frame.u = { axis.x / axisLength, axis.y / axisLength, axis.z / axisLength };
    frame.v = engine::Cross (frame.normal, frame.u);
    for (size_t index : targets) {
        const auto& mesh = snapshot.meshes[index];
        double lowX = INFINITY, lowY = INFINITY, highX = -INFINITY, highY = -INFINITY;
        for (size_t i = 0; i < mesh.VertexCount (); ++i) {
            if (!work.Check (1, error))
                return false;
            const auto p = frame.Project (detail::Point (mesh.vertices, i));
            lowX = std::min (lowX, p.x);
            highX = std::max (highX, p.x);
            lowY = std::min (lowY, p.y);
            highY = std::max (highY, p.y);
        }
        if (!std::isfinite (lowX) || !std::isfinite (highY))
            return Fail (error, "Target has no finite geometry.");
        surfacemap::FaceQuery query;
        for (const cp::PointD p : { cp::PointD { lowX - .001, lowY - .001 },
                                    { highX + .001, lowY - .001 },
                                    { highX + .001, highY + .001 },
                                    { lowX - .001, highY + .001 } }) {
            const V world = frame.Lift (p);
            query.points.insert (query.points.end (), { world.x, world.y, world.z });
        }
        const surfacemap::MatchTolerance tolerance { 1 - 1e-10, kTolerance, kTolerance };
        const auto inventory = surfacemap::EnumerateQuerySourceFaces (mesh, query, &error, tolerance);
        if (!error.empty ())
            return false;
        for (const auto& source : inventory) {
            const V normal { source.normal[0], source.normal[1], source.normal[2] };
            if (engine::Dot (normal, frame.normal) < tolerance.normalDot)
                continue;
            if (candidates.size () >= work.limits.maxFaces)
                return Fail (error, "Facade candidate face budget exceeded.");
            Candidate candidate;
            candidate.key = { { index, source.sourceBody }, source.sourcePgon };
            candidate.source = source;
            cp::PathsD triangles;
            for (size_t i = 0; i < source.trianglePoints.size (); i += 9) {
                if (!work.Check (1, error))
                    return false;
                triangles.push_back ({ frame.Project (detail::Point (source.trianglePoints, i / 3)),
                                       frame.Project (detail::Point (source.trianglePoints, i / 3 + 1)),
                                       frame.Project (detail::Point (source.trianglePoints, i / 3 + 2)) });
            }
            if (!surfacecoverage::Boolean (cp::ClipType::Union, triangles, {}, candidate.paths, error))
                return false;
            candidates.push_back (std::move (candidate));
        }
    }
    return work.Check (0, error);
}
} // namespace

bool ResolveFacadeRegion (const Snapshot& snapshot, const std::vector<size_t>& targets,
                          const std::vector<ExteriorSeed>& seeds, std::vector<FaceKey>& faces, std::string& error,
                          const Limits& limits, const std::function<bool ()>& cancelled)
{
    faces.clear ();
    error.clear ();
    if (!snapshot.id || !snapshot.captureStamp || targets.empty () || targets.size () > 64 || seeds.empty () ||
        seeds.size () > 8)
        return Fail (error, "Explicit bounded targets and user-confirmed exterior seeds are required.");
    if (!std::isfinite (limits.maxSeconds) || limits.maxSeconds <= 0)
        return Fail (error, "Invalid region time budget.");
    const std::set<size_t> scope (targets.begin (), targets.end ());
    size_t count = 0;
    for (size_t i : scope) {
        if (i >= snapshot.meshes.size ())
            return Fail (error, "Facade target is absent.");
        const auto& mesh = snapshot.meshes[i];
        if (mesh.vertices.size () % 3 || mesh.triangles.size () % 3 ||
            mesh.TriangleCount () > limits.maxInputTriangles ||
            count > limits.maxInputTriangles - mesh.TriangleCount ())
            return Fail (error, "Invalid/over-budget facade input.");
        count += mesh.TriangleCount ();
    }
    detail::Work work { limits, cancelled, {} };
    std::set<FaceKey> selected;
    for (const auto& seed : seeds) {
        if (!scope.contains (seed.meshIndex))
            return Fail (error, "Exterior seed must belong to the explicit target scope.");
        for (double value : seed.point)
            if (!std::isfinite (value) || std::abs (value) > 1e9)
                return Fail (error, "Invalid exterior seed point.");
        surfacecoverage::Frame frame;
        std::vector<Candidate> candidates;
        if (!Candidates (snapshot, scope, seed, frame, candidates, work, error))
            return false;
        std::vector<size_t> matches;
        for (size_t i = 0; i < candidates.size (); ++i)
            if (candidates[i].key.body.meshIndex == seed.meshIndex &&
                Contains (candidates[i], frame, Point (seed.point)) &&
                std::abs (engine::Dot (Sub (Point (seed.point), detail::Point (candidates[i].source.trianglePoints, 0)),
                                       frame.normal)) <= kTolerance)
                matches.push_back (i);
        if (matches.size () != 1)
            return Fail (error,
                         "Exterior seed has no unique captured planar source face; refresh and pick inside a face.");
        std::deque<size_t> pending { matches.front () };
        std::set<size_t> reached { matches.front () };
        while (!pending.empty ()) {
            const size_t current = pending.front ();
            pending.pop_front ();
            selected.insert (candidates[current].key);
            if (selected.size () > limits.maxFaces)
                return Fail (error, "Facade region face budget exceeded.");
            for (size_t next = 0; next < candidates.size (); ++next) {
                if (reached.contains (next))
                    continue;
                bool connected = false;
                if (!Adjacent (candidates[current], candidates[next], work, connected, error))
                    return false;
                if (connected) {
                    reached.insert (next);
                    pending.push_back (next);
                }
            }
        }
    }
    if (!work.Check (0, error))
        return false;
    faces.assign (selected.begin (), selected.end ());
    return true;
}
} // namespace geomsrv::exposed
