#include "SunStudy/SunStudySolarExposure.hpp"

#include "Geometry/SurfaceCoverage.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace evp::sunstudy {
namespace {
namespace cp = Clipper2Lib;
namespace sc = geomsrv::surfacecoverage;
using V = sc::V;
using Poly = std::vector<V>;
using geomsrv::engine::Dot;

V Sub (const V& a, const V& b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
V Corner (const SolarCutTriangle& t, size_t i)
{
    return { t.v[i][0], t.v[i][1], t.v[i][2] };
}

bool Triangle (const geomsrv::Mesh& mesh, size_t t, uint32_t m, SolarCutTriangle& out)
{
    out.mesh = m;
    for (size_t c = 0; c < 3; ++c) {
        const size_t v = 3ull * mesh.triangles[3 * t + c];
        if (v + 2 >= mesh.vertices.size ())
            return false;
        std::copy_n (&mesh.vertices[v], 3, out.v[c]);
    }
    return true;
}

struct Surface {
    uint32_t mesh = 0;
    sc::Frame frame;
    cp::PathsD domain;
};

cp::PathD Project (const SolarCutTriangle& triangle, const sc::Frame& frame)
{
    cp::PathD path;
    for (size_t c = 0; c < 3; ++c)
        path.push_back (frame.Project (Corner (triangle, c)));
    if (cp::Area (path) < 0.0)
        std::reverse (path.begin (), path.end ());
    return path;
}

bool Coplanar (const SolarCutTriangle& triangle, const sc::Frame& frame)
{
    for (size_t c = 0; c < 3; ++c)
        if (std::abs (Dot (Sub (Corner (triangle, c), frame.origin), frame.normal)) > sc::kPlaneTolerance)
            return false;
    return true;
}

// Clip the occluder to the sunward side BEFORE projection: triangles straddling
// a receiver must not cast a backwards shadow onto it.
cp::PathD Shadow (const SolarCutTriangle& triangle, const sc::Frame& frame, const V& sun)
{
    constexpr double kRayStart = 1.0e-6;
    const double facing = Dot (frame.normal, sun);
    Poly polygon;
    for (size_t c = 0; c < 3; ++c) {
        const V a = Corner (triangle, c), b = Corner (triangle, (c + 1) % 3);
        const double da = Dot (Sub (a, frame.origin), frame.normal) - kRayStart;
        const double db = Dot (Sub (b, frame.origin), frame.normal) - kRayStart;
        if (da >= 0.0)
            polygon.push_back (a);
        if ((da >= 0.0) != (db >= 0.0)) {
            const double t = da / (da - db);
            polygon.push_back ({ a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z) });
        }
    }
    cp::PathD path;
    for (const V& p : polygon) {
        const double t = Dot (Sub (p, frame.origin), frame.normal) / facing;
        path.push_back (frame.Project ({ p.x - t * sun.x, p.y - t * sun.y, p.z - t * sun.z }));
    }
    if (cp::Area (path) < 0.0)
        std::reverse (path.begin (), path.end ());
    return path;
}

bool Fill (const cp::PathsD& paths, const Surface& surface, std::vector<SolarCutTriangle>& out, std::string& error)
{
    if (paths.empty ())
        return true;
    cp::PathsD triangles;
    // The shared Boolean precision is 7; ClipperD snaps to this binary grid.
    const double grid = std::ldexp (1.0, -24);
    if (!sc::TriangulateOnGrid (paths, grid, triangles, error))
        return false;
    for (const auto& path : triangles) {
        SolarCutTriangle triangle;
        triangle.mesh = surface.mesh;
        for (size_t c = 0; c < 3; ++c) {
            const V p = surface.frame.Lift (path[c]);
            triangle.v[c][0] = p.x;
            triangle.v[c][1] = p.y;
            triangle.v[c][2] = p.z;
        }
        out.push_back (triangle);
    }
    return true;
}

} // namespace

bool ComputeSolarExposure (const geomsrv::Snapshot& snapshot, const SolarCutResult& cut,
                           const std::vector<uint8_t>& blocks, const std::vector<uint32_t>& receivers,
                           const std::vector<SunStep>& steps, SolarExposureResult& result,
                           const std::function<bool ()>& cancelled, size_t workLimit)
{
    result = {};
    SolarExposureResult prepared;
    size_t work = 0;
    const auto charge = [&] {
        if (cancelled && cancelled ()) {
            prepared.error = "Sunlit-region calculation cancelled";
            return false;
        }
        if (++work > workLimit) {
            prepared.error = "Sunlit-region geometry exceeds the preview work budget; narrow the cut slice";
            return false;
        }
        return true;
    };
    const auto compute = [&] () -> bool {
        std::set<uint32_t> wanted (receivers.begin (), receivers.end ());
        std::vector<Surface> surfaces;
        std::vector<SolarCutTriangle> after;
        std::map<size_t, const geomsrv::Mesh*> kept;
        for (const auto& [mesh, remains] : cut.kept)
            kept[mesh] = &remains;
        uint32_t global = 0;
        for (size_t m = 0; m < snapshot.meshes.size (); ++m) {
            const auto& mesh = snapshot.meshes[m];
            for (size_t t = 0; t < mesh.TriangleCount (); ++t, ++global) {
                if (!charge ())
                    return false;
                if (wanted.count (global) == 0)
                    continue;
                SolarCutTriangle triangle;
                sc::Frame frame;
                if (!Triangle (mesh, t, uint32_t (m), triangle) ||
                    !sc::MakeFrame (Corner (triangle, 0), Corner (triangle, 1), Corner (triangle, 2), frame))
                    continue;
                auto found = surfaces.begin ();
                for (; found != surfaces.end (); ++found) {
                    if (!charge ())
                        return false;
                    if (found->mesh == m && Dot (found->frame.normal, frame.normal) > 1.0 - 1.0e-12 &&
                        Coplanar (triangle, found->frame))
                        break;
                }
                if (found == surfaces.end ()) {
                    surfaces.push_back ({ uint32_t (m), frame, {} });
                    found = surfaces.end () - 1;
                }
                found->domain.push_back (Project (triangle, found->frame));
            }
            if (m >= blocks.size () || !blocks[m])
                continue;
            const auto change = kept.find (m);
            const auto& remains = change == kept.end () ? mesh : *change->second;
            for (size_t t = 0; t < remains.TriangleCount (); ++t) {
                if (!charge ())
                    return false;
                SolarCutTriangle triangle;
                if (Triangle (remains, t, uint32_t (m), triangle))
                    after.push_back (triangle);
            }
        }
        for (auto& surface : surfaces) {
            cp::PathsD domain;
            if (!sc::Boolean (cp::ClipType::Union, surface.domain, {}, domain, prepared.error))
                return false;
            const auto change = kept.find (surface.mesh);
            if (change != kept.end ()) {
                cp::PathsD surviving;
                for (size_t t = 0; t < change->second->TriangleCount (); ++t) {
                    if (!charge ())
                        return false;
                    SolarCutTriangle triangle;
                    if (Triangle (*change->second, t, surface.mesh, triangle) && Coplanar (triangle, surface.frame))
                        surviving.push_back (Project (triangle, surface.frame));
                }
                cp::PathsD clipped;
                if (!sc::Boolean (cp::ClipType::Intersection, domain, surviving, clipped, prepared.error))
                    return false;
                domain = std::move (clipped);
            }
            if (domain.empty ())
                continue;
            const auto bounds = cp::GetBounds (domain);
            cp::PathsD allWon;
            for (const auto& step : steps) {
                if (!charge ())
                    return false;
                const V sun { step.direction[0], step.direction[1], step.direction[2] };
                if (!(sun.z > 0.0) || !(Dot (surface.frame.normal, sun) > 1.0e-8))
                    continue;
                // Clip each projected piece to the receiver's small bounds before
                // unioning, keeping low-sun world extents out of the Boolean.
                const auto shadows = [&] (const std::vector<SolarCutTriangle>& triangles, cp::PathsD& out) {
                    cp::PathsD paths;
                    for (const auto& triangle : triangles) {
                        if (!charge ())
                            return false;
                        auto path = Shadow (triangle, surface.frame, sun);
                        if (path.size () < 3 || std::abs (cp::Area (path)) < 1.0e-14)
                            continue;
                        const auto box = cp::GetBounds (path);
                        if (box.right < bounds.left || box.left > bounds.right || box.bottom < bounds.top ||
                            box.top > bounds.bottom)
                            continue;
                        const auto clipped = cp::RectClip (bounds, path, sc::kPrecision);
                        paths.insert (paths.end (), clipped.begin (), clipped.end ());
                        if (paths.size () > 100000) {
                            prepared.error = "Sunlit-region projection exceeds the preview polygon budget";
                            return false;
                        }
                    }
                    return sc::Boolean (cp::ClipType::Intersection, paths, domain, out, prepared.error);
                };
                cp::PathsD removedShadow, afterShadow, won;
                if (!shadows (cut.removed, removedShadow))
                    return false;
                if (removedShadow.empty ())
                    continue;
                if (!shadows (after, afterShadow) ||
                    !sc::Boolean (cp::ClipType::Difference, removedShadow, afterShadow, won, prepared.error))
                    return false;
                if (won.empty ())
                    continue;
                SolarExposureStep timed;
                timed.minute = step.time.hour * 60 + step.time.minute;
                if (!Fill (won, surface, timed.triangles, prepared.error))
                    return false;
                prepared.steps.push_back (std::move (timed));
                allWon.insert (allWon.end (), won.begin (), won.end ());
            }
            cp::PathsD united;
            if (!sc::Boolean (cp::ClipType::Union, allWon, {}, united, prepared.error) ||
                !Fill (united, surface, prepared.triangles, prepared.error))
                return false;
            for (const auto& path : united)
                for (size_t i = 0; i < path.size (); ++i) {
                    const V a = surface.frame.Lift (path[i]), b = surface.frame.Lift (path[(i + 1) % path.size ()]);
                    prepared.boundary.push_back ({ a.x, a.y, a.z, b.x, b.y, b.z });
                }
        }
        return charge ();
    };
    if (!compute ()) {
        result.error = prepared.error;
        return false;
    }
    result = std::move (prepared);
    return true;
}

int ReadSolarExposure (const SolarExposureResult& result, const double origin[3], const double direction[3])
{
    const V o { origin[0], origin[1], origin[2] }, d { direction[0], direction[1], direction[2] };
    double nearest = INFINITY;
    int minute = -1;
    for (const auto& step : result.steps)
        for (const auto& triangle : step.triangles) {
            const V a = Corner (triangle, 0), e1 = Sub (Corner (triangle, 1), a), e2 = Sub (Corner (triangle, 2), a);
            const V h = geomsrv::engine::Cross (d, e2);
            const double det = Dot (e1, h);
            if (std::abs (det) < 1.0e-12)
                continue;
            const V s = Sub (o, a), q = geomsrv::engine::Cross (s, e1);
            const double u = Dot (s, h) / det, v = Dot (d, q) / det, t = Dot (e2, q) / det;
            if (u < -1.0e-8 || v < -1.0e-8 || u + v > 1.0 + 1.0e-8 || t < 0.0)
                continue;
            if (t < nearest - 1.0e-7 || (std::abs (t - nearest) <= 1.0e-7 && (minute < 0 || step.minute < minute))) {
                nearest = t;
                minute = step.minute;
            }
        }
    return minute;
}

} // namespace evp::sunstudy
