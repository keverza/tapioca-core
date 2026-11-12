#include "SunStudy/SunStudySolarExposure.hpp"

#include "Geometry/BvhBuild.hpp"
#include "Geometry/SurfaceCoverage.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
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

// The after scene's blockers, flat for a BVH: three unshared corners each.
struct Blockers {
    std::vector<double> vertices;
    std::vector<uint32_t> meshOf;
    geomsrv::BvhBuildResult bvh;

    size_t Count () const
    {
        return meshOf.size ();
    }
    void Add (const SolarCutTriangle& triangle)
    {
        for (const auto& corner : triangle.v)
            vertices.insert (vertices.end (), corner, corner + 3);
        meshOf.push_back (triangle.mesh);
    }
    SolarCutTriangle At (size_t t) const
    {
        SolarCutTriangle triangle;
        triangle.mesh = meshOf[t];
        std::copy_n (&vertices[9 * t], 9, &triangle.v[0][0]);
        return triangle;
    }
};

// Whether anything in a box can shade `rect` on the receiver plane along
// `sun`. Projection along the sun onto the plane is affine, so the box of its
// eight projected corners contains the image of the box: conservative, never
// a missed shadow. A box wholly behind the plane casts nothing on it.
bool BoxShades (const double mn[3], const double mx[3], const sc::Frame& frame, const V& sun, double facing,
                const cp::RectD& rect)
{
    double above = -INFINITY, left = INFINITY, right = -INFINITY, top = INFINITY, bottom = -INFINITY;
    for (int k = 0; k < 8; ++k) {
        const V p { (k & 1) ? mx[0] : mn[0], (k & 2) ? mx[1] : mn[1], (k & 4) ? mx[2] : mn[2] };
        const double h = Dot (Sub (p, frame.origin), frame.normal);
        above = (std::max) (above, h);
        const auto q = frame.Project ({ p.x - h / facing * sun.x, p.y - h / facing * sun.y, p.z - h / facing * sun.z });
        left = (std::min) (left, q.x), right = (std::max) (right, q.x);
        top = (std::min) (top, q.y), bottom = (std::max) (bottom, q.y);
    }
    return above > 0.0 && right >= rect.left && left <= rect.right && bottom >= rect.top && top <= rect.bottom;
}

// Every blocker whose box can shade `rect`. nanort's pre-order layout: a
// branch's data are its children, a leaf's its slice of `indices`.
template <class Visit>
bool ForShading (const Blockers& blockers, const sc::Frame& frame, const V& sun, double facing, const cp::RectD& rect,
                 Visit&& visit)
{
    const auto& nodes = blockers.bvh.nodes;
    if (nodes.empty ())
        return true;
    std::vector<uint32_t> stack { 0 };
    while (!stack.empty ()) {
        const auto& node = nodes[stack.back ()];
        stack.pop_back ();
        if (!BoxShades (node.bmin, node.bmax, frame, sun, facing, rect))
            continue;
        if (node.flag == 0) {
            stack.push_back (node.data[0]);
            stack.push_back (node.data[1]);
            continue;
        }
        for (uint32_t i = 0; i < node.data[0]; ++i)
            if (!visit (blockers.bvh.indices[node.data[1] + i]))
                return false;
    }
    return true;
}

// Slivers out: what is narrower than two centimetres is numerical residue of
// coincident shadow edges (the cut line where removed meets kept), not won
// sun. Drawn, it was the stray yellow lines (#168). An opening -- shrink, grow
// back, keep within the original -- removes it and leaves real regions exact.
bool Open (cp::PathsD& won, std::string& error)
{
    constexpr double kSliver = 0.01;
    constexpr double kMiter = 20.0; // corners down to ~6 degrees regrow exactly
    const auto shrunk =
        cp::InflatePaths (won, -kSliver, cp::JoinType::Miter, cp::EndType::Polygon, kMiter, sc::kPrecision);
    if (shrunk.empty ()) {
        won.clear ();
        return true;
    }
    const auto grown =
        cp::InflatePaths (shrunk, kSliver, cp::JoinType::Miter, cp::EndType::Polygon, kMiter, sc::kPrecision);
    cp::PathsD kept;
    if (!sc::Boolean (cp::ClipType::Intersection, grown, won, kept, error))
        return false;
    won = std::move (kept);
    return true;
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

void Outline (const cp::PathsD& paths, const Surface& surface, std::vector<std::array<double, 6>>& out)
{
    for (const auto& path : paths)
        for (size_t i = 0; i < path.size (); ++i) {
            const V a = surface.frame.Lift (path[i]), b = surface.frame.Lift (path[(i + 1) % path.size ()]);
            out.push_back ({ a.x, a.y, a.z, b.x, b.y, b.z });
        }
}

double AreaOf (const cp::PathsD& paths)
{
    double area = 0.0;
    for (const auto& path : paths)
        area += cp::Area (path);
    return std::abs (area);
}

// Andrew's monotone chain over every vertex of both regions.
cp::PathD Hull (const cp::PathsD& a, const cp::PathsD& b)
{
    std::vector<cp::PointD> points;
    for (const auto* paths : { &a, &b })
        for (const auto& path : *paths)
            points.insert (points.end (), path.begin (), path.end ());
    std::sort (points.begin (), points.end (),
               [] (const cp::PointD& p, const cp::PointD& q) { return p.x < q.x || (p.x == q.x && p.y < q.y); });
    if (points.size () < 3)
        return {};
    const auto turn = [] (const cp::PointD& o, const cp::PointD& p, const cp::PointD& q) {
        return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x);
    };
    cp::PathD hull (2 * points.size ());
    size_t k = 0;
    for (size_t i = 0; i < points.size (); ++i) {
        while (k >= 2 && turn (hull[k - 2], hull[k - 1], points[i]) <= 0.0)
            --k;
        hull[k++] = points[i];
    }
    for (size_t i = points.size () - 1, lower = k + 1; i-- > 0;) {
        while (k >= lower && turn (hull[k - 2], hull[k - 1], points[i]) <= 0.0)
            --k;
        hull[k++] = points[i];
    }
    hull.resize (k > 0 ? k - 1 : 0);
    return hull;
}

// One piece with no hole, within 2 % of its hull: its motion between two
// suns sweeps their common hull. A notched or split region would have its
// notches filled, so it is not swept that way.
bool NearlyConvex (const cp::PathsD& won, double area)
{
    if (won.size () != 1)
        return false;
    return std::abs (cp::Area (Hull (won, {}))) <= 1.02 * area;
}

// Regions with the time each stands for, minutes.
struct Span {
    double from = 0.0, to = 0.0;
    cp::PathsD region;
};

// The union split by minutes gained. Each span region is a Boolean result, so
// its outers share one orientation and its holes the other. Laid together
// `copies` times each (its duration in a common unit), the winding number at
// a point is the time it is won in that unit; k reversed copies of a box
// around everything lower it by k, so the region the Positive (or Negative)
// rule keeps is "won for more than k units".
bool Bands (const std::vector<Span>& spans, const cp::PathsD& united, double unit, double bandMinutes,
            const Surface& surface, std::vector<SolarExposureBand>& out, const std::function<bool ()>& charge,
            std::string& error)
{
    std::vector<long long> copies;
    long long common = 0, total = 0;
    double orientation = 0.0;
    for (const auto& span : spans) {
        copies.push_back ((std::max) (1ll, std::llround ((span.to - span.from) / unit)));
        common = std::gcd (common, copies.back ());
        if (orientation == 0.0)
            for (const auto& path : span.region)
                orientation += cp::Area (path);
    }
    if (united.empty () || common == 0)
        return true;
    cp::PathsD all;
    for (size_t s = 0; s < spans.size (); ++s) {
        total += copies[s] / common;
        for (long long c = 0; c < copies[s] / common; ++c)
            all.insert (all.end (), spans[s].region.begin (), spans[s].region.end ());
        if (all.size () > 200000) {
            error = "Sunlit-region shading exceeds the preview polygon budget";
            return false;
        }
    }
    const bool positive = orientation > 0.0;
    auto bounds = cp::GetBounds (all);
    bounds.left -= 1.0, bounds.top -= 1.0, bounds.right += 1.0, bounds.bottom += 1.0;
    cp::PathD box { { bounds.left, bounds.top },
                    { bounds.right, bounds.top },
                    { bounds.right, bounds.bottom },
                    { bounds.left, bounds.bottom } };
    if ((cp::Area (box) > 0.0) == positive)
        std::reverse (box.begin (), box.end ());
    const double step = unit * double (common);
    long long boxes = 0;
    cp::PathsD atLeast = united; // won for more than (level - 1) bands
    for (int level = 1; !atLeast.empty () && level < 10000; ++level) {
        if (!charge ())
            return false;
        cp::PathsD more; // won for more than `level` bands
        const long long below = (long long) std::floor (level * bandMinutes / step + 1.0e-9);
        if (below < total) {
            for (; boxes < below; ++boxes)
                all.push_back (box);
            if (!sc::Boolean (cp::ClipType::Union, all, {}, more, error,
                              positive ? cp::FillRule::Positive : cp::FillRule::Negative))
                return false;
        }
        cp::PathsD band;
        if (more.empty ())
            band = std::move (atLeast);
        else if (!sc::Boolean (cp::ClipType::Difference, atLeast, more, band, error))
            return false;
        SolarExposureBand shaded;
        shaded.level = level;
        if (!Fill (band, surface, shaded.triangles, error))
            return false;
        if (!shaded.triangles.empty ())
            out.push_back (std::move (shaded));
        atLeast = std::move (more);
    }
    return true;
}

double NearestHit (const std::vector<SolarCutTriangle>& triangles, const V& o, const V& d)
{
    double nearest = INFINITY;
    for (const auto& triangle : triangles) {
        const V a = Corner (triangle, 0), e1 = Sub (Corner (triangle, 1), a), e2 = Sub (Corner (triangle, 2), a);
        const V h = geomsrv::engine::Cross (d, e2);
        const double det = Dot (e1, h);
        if (std::abs (det) < 1.0e-12)
            continue;
        const V s = Sub (o, a), q = geomsrv::engine::Cross (s, e1);
        const double u = Dot (s, h) / det, v = Dot (d, q) / det, t = Dot (e2, q) / det;
        if (u < -1.0e-8 || v < -1.0e-8 || u + v > 1.0 + 1.0e-8 || t < 0.0)
            continue;
        nearest = (std::min) (nearest, t);
    }
    return nearest;
}

V Unit (const V& v)
{
    const double length = std::sqrt (Dot (v, v));
    return length > 0.0 ? V { v.x / length, v.y / length, v.z / length } : v;
}

} // namespace

bool ComputeSolarExposure (const geomsrv::Snapshot& snapshot, const SolarCutResult& cut,
                           const std::vector<uint8_t>& blocks, const std::vector<uint32_t>& receivers,
                           const std::vector<SunStep>& steps, SolarExposureResult& result,
                           const std::function<bool ()>& cancelled, size_t workLimit,
                           const std::function<void (size_t done, size_t of)>& progress)
{
    result = {};
    SolarExposureResult prepared;
    size_t work = 0;
    bool stopped = false, exhausted = false; // cancelled; out of work budget
    const auto charge = [&] {
        if (cancelled && cancelled ()) {
            stopped = true;
            prepared.error = "Sunlit-region calculation cancelled";
            return false;
        }
        if (++work > workLimit) {
            exhausted = true;
            prepared.error = "Sunlit-region geometry exceeds the preview work budget; narrow the cut slice";
            return false;
        }
        return true;
    };
    const auto compute = [&] () -> bool {
        if (cut.removed.empty ())
            return true; // nothing goes, nothing is won
        std::set<uint32_t> wanted (receivers.begin (), receivers.end ());
        std::vector<Surface> surfaces;
        Blockers after;
        std::map<size_t, const geomsrv::Mesh*> kept;
        for (const auto& [mesh, remains] : cut.kept)
            kept[mesh] = &remains;
        // What goes, boxed once: a receiver it cannot shade at a step is skipped
        // before a single triangle is projected.
        double removedMin[3] = { INFINITY, INFINITY, INFINITY }, removedMax[3] = { -INFINITY, -INFINITY, -INFINITY };
        for (const auto& triangle : cut.removed)
            for (const auto& corner : triangle.v)
                for (int axis = 0; axis < 3; ++axis) {
                    removedMin[axis] = (std::min) (removedMin[axis], corner[axis]);
                    removedMax[axis] = (std::max) (removedMax[axis], corner[axis]);
                }
        uint32_t global = 0;
        for (size_t m = 0; m < snapshot.meshes.size (); ++m) {
            const auto& mesh = snapshot.meshes[m];
            // A surface belongs to one mesh, and its planes are bucketed by a
            // quantised normal and offset: a terrain of thousands of distinct
            // planes searched pairwise spent the whole work budget before a
            // single shadow was cast (#168). A plane split across two buckets
            // is two surfaces, which only costs a seam.
            std::map<std::array<long long, 4>, std::vector<size_t>> planes;
            for (size_t t = 0; t < mesh.TriangleCount (); ++t, ++global) {
                if ((global & 1023u) == 0 && !charge ())
                    return false;
                if (wanted.count (global) == 0)
                    continue;
                SolarCutTriangle triangle;
                sc::Frame frame;
                if (!Triangle (mesh, t, uint32_t (m), triangle) ||
                    !sc::MakeFrame (Corner (triangle, 0), Corner (triangle, 1), Corner (triangle, 2), frame))
                    continue;
                const std::array<long long, 4> key { std::llround (frame.normal.x * 1.0e5),
                                                     std::llround (frame.normal.y * 1.0e5),
                                                     std::llround (frame.normal.z * 1.0e5),
                                                     std::llround (Dot (frame.origin, frame.normal) * 1.0e4) };
                auto& bucket = planes[key];
                size_t found = surfaces.size ();
                for (const size_t candidate : bucket)
                    if (Dot (surfaces[candidate].frame.normal, frame.normal) > 1.0 - 1.0e-12 &&
                        Coplanar (triangle, surfaces[candidate].frame)) {
                        found = candidate;
                        break;
                    }
                if (found == surfaces.size ()) {
                    bucket.push_back (found);
                    surfaces.push_back ({ uint32_t (m), frame, {} });
                }
                surfaces[found].domain.push_back (Project (triangle, surfaces[found].frame));
            }
            if (m >= blocks.size () || !blocks[m])
                continue;
            const auto change = kept.find (m);
            const auto& remains = change == kept.end () ? mesh : *change->second;
            for (size_t t = 0; t < remains.TriangleCount (); ++t) {
                if ((t & 1023u) == 0 && !charge ())
                    return false;
                SolarCutTriangle triangle;
                if (Triangle (remains, t, uint32_t (m), triangle))
                    after.Add (triangle);
            }
        }
        // The after scene, indexed once: each receiver and sun visits only the
        // blockers whose shadow can reach what the removed part shaded.
        if (after.Count () > 0) {
            std::vector<uint32_t> faces (3 * after.Count ());
            for (uint32_t i = 0; i < faces.size (); ++i)
                faces[i] = i;
            after.bvh = geomsrv::BuildBinnedSahBvh (after.vertices.data (), faces.data (), after.Count ());
        }
        if (cancelled && cancelled ()) {
            prepared.error = "Sunlit-region calculation cancelled";
            return false;
        }
        // The timestep each measured sun stands for, from the steps themselves
        // (a slice is consecutive measured steps); an hour for a lone step.
        double timestep = 0.0;
        for (size_t i = 1; i < steps.size (); ++i) {
            const double gap = double ((steps[i].time.hour - steps[i - 1].time.hour) * 60 +
                                       (steps[i].time.minute - steps[i - 1].time.minute));
            if (gap > 0.0 && (timestep == 0.0 || gap < timestep))
                timestep = gap;
        }
        if (!(timestep > 0.0))
            timestep = 60.0;
        // At most ten shades over the slice, each a power-of-two share of the
        // timestep: a quarter of it at the finest.
        constexpr double kMaxBands = 10.0;
        double bandMinutes = 0.25 * timestep;
        while (bandMinutes * kMaxBands < double (steps.size ()) * timestep)
            bandMinutes *= 2.0;
        // Only a surface the removed part can shade at some measured sun is
        // worth a Boolean; on a large site that is a handful of many thousands.
        const auto reachable = [&] (const Surface& surface) {
            const auto bounds = cp::GetBounds (surface.domain);
            for (const auto& step : steps) {
                const V sun { step.direction[0], step.direction[1], step.direction[2] };
                const double facing = Dot (surface.frame.normal, sun);
                if (sun.z > 0.0 && facing > 1.0e-8 &&
                    BoxShades (removedMin, removedMax, surface.frame, sun, facing, bounds))
                    return true;
            }
            return false;
        };
        std::vector<Surface*> shaded;
        for (auto& surface : surfaces) {
            if (!charge ())
                return false;
            if (reachable (surface))
                shaded.push_back (&surface);
        }
        prepared.surfacesShaded = shaded.size ();
        std::string firstSkip;
        // One surface's failure is that surface's, not the slice's (#168):
        // everything finished so far is kept and the rest is said.
        for (Surface* const each : shaded) {
            if (progress)
                progress (prepared.surfacesDone, shaded.size ());
            SolarExposureResult own;
            const auto shade = [&] (SolarExposureResult& out) -> bool {
                auto& surface = *each;
                cp::PathsD domain;
                if (!sc::Boolean (cp::ClipType::Union, surface.domain, {}, domain, out.error))
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
                    if (!sc::Boolean (cp::ClipType::Intersection, domain, surviving, clipped, out.error))
                        return false;
                    domain = std::move (clipped);
                }
                if (domain.empty ())
                    return true;
                const auto bounds = cp::GetBounds (domain);
                // The won region at one sun direction: the removed part's shadow
                // minus every remaining shadow, on this surface. Empty when the
                // sun cannot reach it or the removed part cannot shade it.
                const auto wonAt = [&] (const V& sun, cp::PathsD& won) -> bool {
                    won.clear ();
                    if (!charge ())
                        return false;
                    const double facing = Dot (surface.frame.normal, sun);
                    if (!(sun.z > 0.0) || !(facing > 1.0e-8))
                        return true;
                    if (!BoxShades (removedMin, removedMax, surface.frame, sun, facing, bounds))
                        return true; // the removed part cannot shade this receiver at this sun
                    // Clip each projected piece to `within` before unioning, keeping
                    // low-sun world extents out of the Boolean.
                    cp::PathsD paths;
                    const auto project = [&] (const SolarCutTriangle& triangle, const cp::RectD& within) {
                        if (!charge ())
                            return false;
                        auto path = Shadow (triangle, surface.frame, sun);
                        if (path.size () < 3 || std::abs (cp::Area (path)) < 1.0e-14)
                            return true;
                        const auto box = cp::GetBounds (path);
                        if (box.right < within.left || box.left > within.right || box.bottom < within.top ||
                            box.top > within.bottom)
                            return true;
                        const auto clipped = cp::RectClip (within, path, sc::kPrecision);
                        paths.insert (paths.end (), clipped.begin (), clipped.end ());
                        if (paths.size () > 100000) {
                            out.error = "Sunlit-region projection exceeds the preview polygon budget";
                            return false;
                        }
                        return true;
                    };
                    cp::PathsD removedShadow, afterShadow;
                    for (const auto& triangle : cut.removed)
                        if (!project (triangle, bounds))
                            return false;
                    if (!sc::Boolean (cp::ClipType::Intersection, paths, domain, removedShadow, out.error))
                        return false;
                    if (removedShadow.empty ())
                        return true;
                    // Only what can shade the removed part's shadow matters here.
                    const auto region = cp::GetBounds (removedShadow);
                    paths.clear ();
                    if (!ForShading (after, surface.frame, sun, facing, region,
                                     [&] (uint32_t t) { return project (after.At (t), region); }))
                        return false;
                    return sc::Boolean (cp::ClipType::Intersection, paths, domain, afterShadow, out.error) &&
                           sc::Boolean (cp::ClipType::Difference, removedShadow, afterShadow, won, out.error) &&
                           Open (won, out.error);
                };
                struct Sample {
                    double minute = 0.0;
                    V sun {};
                    cp::PathsD won;
                    double area = 0.0;
                };
                const auto sample = [&] (Sample& s) {
                    if (!wonAt (s.sun, s.won))
                        return false;
                    s.area = AreaOf (s.won);
                    return true;
                };
                // The measured steps, exactly: the clock step view and the step
                // accounting read these, never an interpolated sun.
                std::vector<Sample> measured;
                for (const auto& step : steps) {
                    Sample s;
                    s.minute = step.time.hour * 60 + step.time.minute;
                    s.sun = { step.direction[0], step.direction[1], step.direction[2] };
                    if (!sample (s))
                        return false;
                    if (!s.won.empty ()) {
                        SolarExposureStep timed;
                        timed.minute = int (s.minute);
                        if (!Fill (s.won, surface, timed.triangles, out.error))
                            return false;
                        Outline (s.won, surface, timed.boundary);
                        out.steps.push_back (std::move (timed));
                    }
                    measured.push_back (std::move (s));
                }
                // SWEPT BETWEEN STEPS, NOT SAMPLED (#168). A thin won strip moves
                // between two suns and lights what it passes over; drawn only
                // where it stood at each step, the slice was a comb of strips with
                // unlit gaps. Two near-convex regions sweep their common hull (a
                // rigid motion's exact sweep); the sun halfway is traced to check
                // that nothing remaining interrupts it, and the interval is split
                // where it does or where the regions are not convex.
                std::vector<Span> spans;
                const std::function<bool (const Sample&, const Sample&, int)> sweep = [&] (const Sample& a,
                                                                                           const Sample& b, int depth) {
                    constexpr int kMaxDepth = 3;
                    constexpr double kNone = 1.0e-8;
                    if (!charge ())
                        return false;
                    if (a.area <= kNone && b.area <= kNone)
                        return true;
                    Sample m;
                    m.minute = 0.5 * (a.minute + b.minute);
                    const V sa = Unit (a.sun), sb = Unit (b.sun);
                    m.sun = { sa.x + sb.x, sa.y + sb.y, sa.z + sb.z };
                    Span span { a.minute, b.minute, {} };
                    if (a.area > kNone && b.area > kNone && NearlyConvex (a.won, a.area) &&
                        NearlyConvex (b.won, b.area)) {
                        const cp::PathsD hull { Hull (a.won, b.won) };
                        bool swept = depth >= kMaxDepth;
                        if (!swept) {
                            cp::PathsD outside;
                            if (!sample (m) || !sc::Boolean (cp::ClipType::Difference, m.won, hull, outside, out.error))
                                return false;
                            swept = m.area >= 0.8 * (std::min) (a.area, b.area) && AreaOf (outside) <= 0.2 * m.area;
                        }
                        if (swept) {
                            if (!sc::Boolean (cp::ClipType::Intersection, hull, domain, span.region, out.error))
                                return false;
                            spans.push_back (std::move (span));
                            return true;
                        }
                        return sweep (a, m, depth + 1) && sweep (m, b, depth + 1);
                    }
                    bool joined = depth >= kMaxDepth;
                    if (!joined && a.area > kNone && b.area > kNone) {
                        cp::PathsD common;
                        if (!sc::Boolean (cp::ClipType::Intersection, a.won, b.won, common, out.error))
                            return false;
                        joined = AreaOf (common) >= 0.5 * (std::min) (a.area, b.area);
                    }
                    if (joined) {
                        if (!sc::Boolean (cp::ClipType::Union, a.won, b.won, span.region, out.error))
                            return false;
                        spans.push_back (std::move (span));
                        return true;
                    }
                    return sample (m) && sweep (a, m, depth + 1) && sweep (m, b, depth + 1);
                };
                // Each step stands for one timestep: half of it on each side.
                // Between consecutive steps the sweep covers that time; at the
                // slice's ends (or across a gap in the measured hours), the step's
                // own region stands for its outer half.
                for (size_t i = 0; i < measured.size (); ++i) {
                    const Sample& s = measured[i];
                    const bool left = i > 0 && s.minute - measured[i - 1].minute <= 1.5 * timestep;
                    const bool right = i + 1 < measured.size () && measured[i + 1].minute - s.minute <= 1.5 * timestep;
                    if (!s.won.empty () && !left)
                        spans.push_back ({ s.minute - 0.5 * timestep, s.minute, s.won });
                    if (!s.won.empty () && !right)
                        spans.push_back ({ s.minute, s.minute + 0.5 * timestep, s.won });
                    if (right && !sweep (s, measured[i + 1], 0))
                        return false;
                }
                spans.erase (std::remove_if (spans.begin (), spans.end (),
                                             [] (const Span& span) { return span.region.empty (); }),
                             spans.end ());
                if (spans.empty ())
                    return true;
                cp::PathsD all, united;
                for (const auto& span : spans)
                    all.insert (all.end (), span.region.begin (), span.region.end ());
                if (!sc::Boolean (cp::ClipType::Union, all, {}, united, out.error) ||
                    !Fill (united, surface, out.triangles, out.error))
                    return false;
                Outline (united, surface, out.boundary);
                for (const auto& span : spans) {
                    SolarExposureSpan timed { span.from, span.to, {} };
                    if (!Fill (span.region, surface, timed.triangles, out.error))
                        return false;
                    out.spans.push_back (std::move (timed));
                }
                if (!Bands (spans, united, timestep / 16.0, bandMinutes, surface, out.bands, charge, out.error))
                    return false;
                return true;
            };
            if (!shade (own)) {
                if (stopped)
                    return false;
                if (exhausted) {
                    prepared.partial = true;
                    break;
                }
                ++prepared.skipped;
                if (firstSkip.empty ())
                    firstSkip = own.error.empty () ? prepared.error : own.error;
                prepared.error.clear ();
                ++prepared.surfacesDone;
                continue;
            }
            const auto append = [] (auto& to, auto& from) {
                to.insert (to.end (), std::make_move_iterator (from.begin ()), std::make_move_iterator (from.end ()));
            };
            append (prepared.triangles, own.triangles);
            append (prepared.boundary, own.boundary);
            append (prepared.steps, own.steps);
            append (prepared.spans, own.spans);
            append (prepared.bands, own.bands);
            ++prepared.surfacesDone;
        }
        prepared.error.clear ();
        prepared.bandMinutes = bandMinutes;
        if (prepared.partial)
            prepared.note = "Work budget reached after " + std::to_string (prepared.surfacesDone) + " of " +
                            std::to_string (prepared.surfacesShaded) +
                            " surfaces the removed part can shade; the rest are not shown. Narrow the cut slice.";
        if (prepared.skipped > 0)
            prepared.note += (prepared.note.empty () ? "" : " ") + std::to_string (prepared.skipped) + " surface" +
                             (prepared.skipped == 1 ? "" : "s") + " could not be shaded and " +
                             (prepared.skipped == 1 ? "is" : "are") + " left out: " + firstSkip;
        return true;
    };
    if (!compute ()) {
        result.error = prepared.error;
        result.work = work;
        return false;
    }
    result = std::move (prepared);
    result.work = work;
    return true;
}

// Regions on one surface lie in one plane, so they meet the ray at the same
// distance up to rounding; one farther along the ray is a different place.
constexpr double kSameSurface = 1.0e-5;

std::vector<std::array<double, 2>> ReadSolarExposureRuns (const SolarExposureResult& result, const double origin[3],
                                                          const double direction[3])
{
    const V o { origin[0], origin[1], origin[2] }, d { direction[0], direction[1], direction[2] };
    std::vector<std::pair<double, std::array<double, 2>>> hits;
    double nearest = INFINITY;
    for (const auto& span : result.spans) {
        const double t = NearestHit (span.triangles, o, d);
        if (std::isfinite (t)) {
            hits.push_back ({ t, { span.from, span.to } });
            nearest = (std::min) (nearest, t);
        }
    }
    std::vector<std::array<double, 2>> runs;
    for (const auto& [t, run] : hits)
        if (t <= nearest + kSameSurface)
            runs.push_back (run);
    std::sort (runs.begin (), runs.end ());
    std::vector<std::array<double, 2>> merged;
    for (const auto& run : runs) {
        if (!merged.empty () && run[0] <= merged.back ()[1] + 1.0e-6)
            merged.back ()[1] = (std::max) (merged.back ()[1], run[1]);
        else
            merged.push_back (run);
    }
    return merged;
}

int ReadSolarExposure (const SolarExposureResult& result, const double origin[3], const double direction[3])
{
    const V o { origin[0], origin[1], origin[2] }, d { direction[0], direction[1], direction[2] };
    std::vector<std::pair<double, int>> hits;
    double nearest = INFINITY;
    for (const auto& step : result.steps) {
        const double t = NearestHit (step.triangles, o, d);
        if (std::isfinite (t)) {
            hits.emplace_back (t, step.minute);
            nearest = (std::min) (nearest, t);
        }
    }
    int minute = -1;
    for (const auto& [t, at] : hits)
        if (t <= nearest + kSameSurface && (minute < 0 || at < minute))
            minute = at;
    return minute;
}

} // namespace evp::sunstudy
