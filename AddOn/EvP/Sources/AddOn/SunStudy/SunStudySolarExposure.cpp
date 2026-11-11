#include "SunStudy/SunStudySolarExposure.hpp"

#include "Geometry/BvhBuild.hpp"
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
            // A surface belongs to one mesh: search only this mesh's planes,
            // never every plane found so far (quadratic on a large model).
            const size_t meshSurfaces = surfaces.size ();
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
                auto found = surfaces.begin () + std::ptrdiff_t (meshSurfaces);
                for (; found != surfaces.end (); ++found) {
                    if (!charge ())
                        return false;
                    if (Dot (found->frame.normal, frame.normal) > 1.0 - 1.0e-12 && Coplanar (triangle, found->frame))
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
                const double facing = Dot (surface.frame.normal, sun);
                if (!(sun.z > 0.0) || !(facing > 1.0e-8))
                    continue;
                if (!BoxShades (removedMin, removedMax, surface.frame, sun, facing, bounds))
                    continue; // the removed part cannot shade this receiver at this sun
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
                        prepared.error = "Sunlit-region projection exceeds the preview polygon budget";
                        return false;
                    }
                    return true;
                };
                cp::PathsD removedShadow, afterShadow, won;
                for (const auto& triangle : cut.removed)
                    if (!project (triangle, bounds))
                        return false;
                if (!sc::Boolean (cp::ClipType::Intersection, paths, domain, removedShadow, prepared.error))
                    return false;
                if (removedShadow.empty ())
                    continue;
                // Only what can shade the removed part's shadow matters here.
                const auto region = cp::GetBounds (removedShadow);
                paths.clear ();
                if (!ForShading (after, surface.frame, sun, facing, region,
                                 [&] (uint32_t t) { return project (after.At (t), region); }))
                    return false;
                if (!sc::Boolean (cp::ClipType::Intersection, paths, domain, afterShadow, prepared.error) ||
                    !sc::Boolean (cp::ClipType::Difference, removedShadow, afterShadow, won, prepared.error) ||
                    !Open (won, prepared.error))
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
