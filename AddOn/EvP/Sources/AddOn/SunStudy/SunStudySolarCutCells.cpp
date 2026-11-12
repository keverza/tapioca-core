#include "SunStudy/SunStudySolarCutCells.hpp"

#include "Geometry/SurfaceCoverage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <vector>

namespace evp::sunstudy {
namespace {
namespace cp = Clipper2Lib;
namespace sc = geomsrv::surfacecoverage;
using V = sc::V;

V At (const double p[3])
{
    return { p[0], p[1], p[2] };
}

cp::PathD Oriented (cp::PathD path)
{
    if (cp::Area (path) < 0.0)
        std::reverse (path.begin (), path.end ());
    return path;
}

// Hours are whole study steps: a millihour key keeps equal values together.
long long KeyOf (float hours)
{
    return std::llround (double (hours) * 1000.0);
}

bool Fill (const cp::PathsD& paths, const sc::Frame& frame, uint32_t mesh, std::vector<SolarCutTriangle>& out,
           std::string& error)
{
    if (paths.empty ())
        return true;
    cp::PathsD triangles;
    if (!sc::TriangulateOnGrid (paths, std::ldexp (1.0, -24), triangles, error))
        return false;
    for (const auto& path : triangles) {
        SolarCutTriangle triangle;
        triangle.mesh = mesh;
        for (size_t c = 0; c < 3; ++c) {
            const V p = frame.Lift (path[c]);
            triangle.v[c][0] = p.x;
            triangle.v[c][1] = p.y;
            triangle.v[c][2] = p.z;
        }
        out.push_back (triangle);
    }
    return true;
}

// One edge of a surface's merged cells, with the side its cells are on.
struct Edge {
    V a, b;
    V inward; // unit, in the surface's plane, perpendicular to the edge
};

V Minus (const V& p, const V& q)
{
    return { p.x - q.x, p.y - q.y, p.z - q.z };
}

double Length (const V& v)
{
    return std::sqrt (geomsrv::engine::Dot (v, v));
}

// ⚠️ SEAMS ARE NOT CONTOURS (#168). Each surface outlines its own changed
// cells, so where two surfaces meet with changed cells on both sides -- a
// terrain crease, two patches of one floor -- each draws the shared edge, and
// a line runs across the middle of the lit cells. The same happens at a
// hairline crack in one surface's union. Such an edge has cells on BOTH
// sides: where two near-collinear edges overlap with their cells facing away
// from each other, the overlap is removed from both. Edges whose cells face
// the same way (a duplicated surface) or meet at a real fold (a wall on a
// floor) are kept.
std::vector<std::array<double, 6>> WithoutSeams (const std::vector<Edge>& edges)
{
    constexpr double kGap = 2.0e-3;   // metres between the two sides of a seam
    constexpr double kOpposed = -0.5; // inward directions at least 120 degrees apart
    constexpr double kBucket = 2.0;   // spatial hash cell, metres
    using Key = std::array<long long, 3>;
    const auto keyOf = [] (const V& p) {
        return Key { std::llround (std::floor (p.x / kBucket)), std::llround (std::floor (p.y / kBucket)),
                     std::llround (std::floor (p.z / kBucket)) };
    };
    // Each edge filed under every bucket it passes through (sampled at half a
    // bucket), and looked up in the neighbours of each of its own samples.
    const auto samples = [] (const Edge& e) {
        std::vector<V> points;
        const V d = Minus (e.b, e.a);
        const int n = (std::max) (1, int (std::ceil (Length (d) / (0.5 * kBucket))));
        for (int i = 0; i <= n; ++i) {
            const double t = double (i) / double (n);
            points.push_back ({ e.a.x + d.x * t, e.a.y + d.y * t, e.a.z + d.z * t });
        }
        return points;
    };
    std::map<Key, std::vector<size_t>> buckets;
    for (size_t i = 0; i < edges.size (); ++i) {
        std::vector<Key> keys;
        for (const V& p : samples (edges[i]))
            keys.push_back (keyOf (p));
        std::sort (keys.begin (), keys.end ());
        keys.erase (std::unique (keys.begin (), keys.end ()), keys.end ());
        for (const Key& key : keys)
            buckets[key].push_back (i);
    }
    std::vector<std::array<double, 6>> out;
    for (size_t i = 0; i < edges.size (); ++i) {
        const Edge& e = edges[i];
        const V d = Minus (e.b, e.a);
        const double length = Length (d);
        if (length <= kGap)
            continue;
        const V u { d.x / length, d.y / length, d.z / length };
        std::vector<size_t> near;
        for (const V& p : samples (e)) {
            const Key k = keyOf (p);
            for (long long dx = -1; dx <= 1; ++dx)
                for (long long dy = -1; dy <= 1; ++dy)
                    for (long long dz = -1; dz <= 1; ++dz) {
                        const auto found = buckets.find ({ k[0] + dx, k[1] + dy, k[2] + dz });
                        if (found != buckets.end ())
                            near.insert (near.end (), found->second.begin (), found->second.end ());
                    }
        }
        std::sort (near.begin (), near.end ());
        near.erase (std::unique (near.begin (), near.end ()), near.end ());
        // The parts of [0, length] along e that another side covers.
        std::vector<std::array<double, 2>> covered;
        for (const size_t j : near) {
            if (j == i || geomsrv::engine::Dot (e.inward, edges[j].inward) > kOpposed)
                continue;
            double t[2];
            bool onLine = true;
            for (int end = 0; end < 2 && onLine; ++end) {
                const V q = Minus (end == 0 ? edges[j].a : edges[j].b, e.a);
                t[end] = geomsrv::engine::Dot (q, u);
                const V off { q.x - u.x * t[end], q.y - u.y * t[end], q.z - u.z * t[end] };
                onLine = Length (off) <= kGap;
            }
            if (!onLine)
                continue;
            const double from = (std::max) (0.0, (std::min) (t[0], t[1]));
            const double to = (std::min) (length, (std::max) (t[0], t[1]));
            if (to - from > kGap)
                covered.push_back ({ from, to });
        }
        std::sort (covered.begin (), covered.end ());
        double at = 0.0;
        const auto keep = [&] (double from, double to) {
            if (to - from <= kGap)
                return;
            out.push_back ({ e.a.x + u.x * from, e.a.y + u.y * from, e.a.z + u.z * from, e.a.x + u.x * to,
                             e.a.y + u.y * to, e.a.z + u.z * to });
        };
        for (const auto& [from, to] : covered) {
            if (from > at)
                keep (at, from);
            at = (std::max) (at, to);
        }
        if (at < length) {
            if (at == 0.0)
                out.push_back ({ e.a.x, e.a.y, e.a.z, e.b.x, e.b.y, e.b.z }); // untouched: exact ends
            else
                keep (at, length);
        }
    }
    return out;
}

} // namespace

bool SolarCutCellsResult::Read (uint32_t sample, float& beforeHours, float& afterHours) const
{
    const auto found = std::lower_bound (samples.begin (), samples.end (), sample);
    if (found == samples.end () || *found != sample)
        return false;
    const size_t i = size_t (found - samples.begin ());
    beforeHours = before[i];
    afterHours = after[i];
    return true;
}

bool BuildSolarCutCells (const std::vector<SolarCutCell>& cells,
                         const std::map<uint64_t, std::vector<SolarCutTriangle>>& surfaces, SolarCutCellsResult& out,
                         const std::function<bool ()>& cancelled)
{
    out = {};
    // The hover's table, whatever can be drawn.
    std::vector<size_t> order (cells.size ());
    std::iota (order.begin (), order.end (), size_t (0));
    std::sort (order.begin (), order.end (), [&] (size_t a, size_t b) { return cells[a].sample < cells[b].sample; });
    for (const size_t i : order) {
        if (!out.samples.empty () && out.samples.back () == cells[i].sample)
            continue;
        out.samples.push_back (cells[i].sample);
        out.before.push_back (cells[i].before);
        out.after.push_back (cells[i].after);
        out.mostGained = (std::max) (out.mostGained, cells[i].after - cells[i].before);
    }

    std::map<uint64_t, std::vector<size_t>> bySurface;
    for (size_t i = 0; i < cells.size (); ++i)
        bySurface[cells[i].surface].push_back (i);
    std::map<long long, SolarCutCellGroup> groups;
    std::vector<Edge> edges;
    for (const auto& [key, members] : bySurface) {
        if (cancelled && cancelled ()) {
            out.error = "Cell re-count cancelled";
            return false;
        }
        const auto found = surfaces.find (key);
        if (found == surfaces.end () || found->second.empty ()) {
            out.unplaced += members.size ();
            continue;
        }
        const auto& triangles = found->second;
        sc::Frame frame;
        bool framed = false;
        for (const auto& t : triangles)
            if (sc::MakeFrame (At (t.v[0]), At (t.v[1]), At (t.v[2]), frame)) {
                framed = true;
                break;
            }
        if (!framed) {
            out.unplaced += members.size ();
            continue;
        }
        const uint32_t mesh = triangles.front ().mesh;
        cp::PathsD pieces;
        for (const auto& t : triangles)
            pieces.push_back (
                Oriented ({ frame.Project (At (t.v[0])), frame.Project (At (t.v[1])), frame.Project (At (t.v[2])) }));
        cp::PathsD domain;
        if (!sc::Boolean (cp::ClipType::Union, pieces, {}, domain, out.error))
            return false;
        std::map<long long, cp::PathsD> squares;
        cp::PathsD all;
        for (const size_t i : members) {
            const auto& cell = cells[i];
            cp::PathD square;
            for (const auto& corner : cell.corners)
                square.push_back (frame.Project (At (corner)));
            square = Oriented (std::move (square));
            squares[KeyOf (cell.after)].push_back (square);
            all.push_back (std::move (square));
        }
        for (auto& [hours, paths] : squares) {
            cp::PathsD region;
            if (!sc::Boolean (cp::ClipType::Intersection, paths, domain, region, out.error))
                return false;
            auto& group = groups[hours];
            group.after = float (double (hours) / 1000.0);
            if (!Fill (region, frame, mesh, group.triangles, out.error))
                return false;
        }
        cp::PathsD merged;
        if (!sc::Boolean (cp::ClipType::Intersection, all, domain, merged, out.error))
            return false;
        for (const auto& path : merged) {
            out.area += cp::Area (path);
            // Outer rings run counter-clockwise and holes clockwise in the
            // frame, so the cells are always on the left: normal x direction.
            for (size_t i = 0; i < path.size (); ++i) {
                const V a = frame.Lift (path[i]), b = frame.Lift (path[(i + 1) % path.size ()]);
                V inward = geomsrv::engine::Cross (frame.normal, Minus (b, a));
                const double size = Length (inward);
                if (size <= 0.0)
                    continue;
                inward = { inward.x / size, inward.y / size, inward.z / size };
                edges.push_back ({ a, b, inward });
            }
        }
        out.cells += members.size ();
    }
    out.outline = WithoutSeams (edges);
    out.area = std::abs (out.area);
    for (auto& [hours, group] : groups)
        if (!group.triangles.empty ())
            out.groups.push_back (std::move (group));
    return true;
}

} // namespace evp::sunstudy
