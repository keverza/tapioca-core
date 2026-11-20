#include "Edit/EditTerrainBake.hpp"
#include "Edit/EditPathContours.hpp"
#include "Edit/EditPathNetwork.hpp"
#include "Edit/EditTerrainBakeDivide.hpp"
#include <set>
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <unordered_map>

namespace geomsrv::edit {
namespace {
constexpr double kOnLine = 1.0e-6; // metres in plan: a line's end is on the boundary

// Distance of `p` from the segment a-b; `plan` leaves the heights out. `t` is where along it.
double FromSegment (Vec3 p, Vec3 a, Vec3 b, bool plan, double* t = nullptr)
{
    if (plan)
        p.z = a.z = b.z = 0.0;
    const Vec3 d = b - a;
    const double dd = Dot (d, d);
    const double s = dd > 0.0 ? std::clamp (Dot (p - a, d) / dd, 0.0, 1.0) : 0.0;
    if (t)
        *t = s;
    return Length (p - (a + d * s));
}

// Douglas-Peucker over line[first..last], both kept.
void Keep (const std::vector<Vec3>& line, size_t first, size_t last, double tolerance, std::vector<char>& keep)
{
    std::vector<std::pair<size_t, size_t>> spans { { first, last } };
    keep[first] = keep[last] = 1;
    while (!spans.empty ()) {
        const auto [a, b] = spans.back ();
        spans.pop_back ();
        double worst = tolerance;
        size_t at = a;
        for (size_t i = a + 1; i < b; ++i)
            if (const double d = FromSegment (line[i], line[a], line[b], false); d > worst)
                worst = d, at = i;
        if (at != a) {
            keep[at] = 1;
            spans.push_back ({ a, at });
            spans.push_back ({ at, b });
        }
    }
}

std::vector<Vec3> ReduceOpen (const std::vector<Vec3>& line, double tolerance)
{
    if (line.size () < 3)
        return line;
    std::vector<char> keep (line.size (), 0);
    Keep (line, 0, line.size () - 1, tolerance, keep);
    std::vector<Vec3> out;
    for (size_t i = 0; i < line.size (); ++i)
        if (keep[i])
            out.push_back (line[i]);
    return out;
}

// A ring (no repeated end), reduced between its `fixed` points; with fewer than two of those,
// between its first point and the one farthest from it.
std::vector<Vec3> ReduceRing (const std::vector<Vec3>& ring, std::vector<char> fixed, double tolerance)
{
    const size_t n = ring.size ();
    if (n < 4)
        return ring;
    fixed.resize (n, 0);
    if (std::count (fixed.begin (), fixed.end (), char (1)) < 2) {
        const size_t from = size_t (std::find (fixed.begin (), fixed.end (), char (1)) - fixed.begin ()) % n;
        size_t far = from;
        double best = -1.0;
        for (size_t i = 0; i < n; ++i)
            if (const double d = Length (ring[i] - ring[from]); d > best)
                best = d, far = i;
        fixed[from] = fixed[far] = 1;
    }
    const size_t start = size_t (std::find (fixed.begin (), fixed.end (), char (1)) - fixed.begin ());
    std::vector<Vec3> turned;
    std::vector<char> anchor;
    for (size_t i = 0; i <= n; ++i) {
        turned.push_back (ring[(start + i) % n]);
        anchor.push_back (fixed[(start + i) % n]);
    }
    std::vector<char> keep (n + 1, 0);
    for (size_t a = 0, b = 1; b <= n; ++b)
        if (anchor[b]) {
            Keep (turned, a, b, tolerance, keep);
            a = b;
        }
    std::vector<Vec3> out;
    for (size_t i = 0; i < n; ++i)
        if (keep[i])
            out.push_back (turned[i]);
    return out.size () >= 3 ? out : ring;
}

double Area (const std::vector<Vec3>& ring)
{
    double twice = 0.0;
    for (size_t i = 0; i < ring.size (); ++i) {
        const Vec3 a = ring[i], b = ring[(i + 1) % ring.size ()];
        twice += a.x * b.y - b.x * a.y;
    }
    return twice / 2.0;
}

uint64_t EdgeKey (uint32_t a, uint32_t b)
{
    return (uint64_t (std::min (a, b)) << 32) | std::max (a, b);
}

struct Surface {
    std::vector<Vec3> vertices;
    const std::vector<uint32_t>* triangles = nullptr;
    // Per undirected edge: how many triangles use it, the first two of them, and its
    // direction in the first.
    struct Use {
        uint32_t count = 0, from = 0, to = 0, faces[2] = { 0, 0 };
    };
    std::unordered_map<uint64_t, Use> edges;
};

// The boundary loops in the triangles' own direction; false when the boundary does not close.
bool BoundaryLoops (const Surface& s, std::vector<std::vector<uint32_t>>& loops)
{
    std::multimap<uint32_t, uint32_t> next;
    for (const auto& [key, use] : s.edges)
        if (use.count == 1)
            next.insert ({ use.from, use.to });
    while (!next.empty ()) {
        std::vector<uint32_t> loop;
        const uint32_t start = next.begin ()->first;
        uint32_t at = start;
        do {
            const auto step = next.find (at);
            if (step == next.end ())
                return false;
            loop.push_back (at);
            at = step->second;
            next.erase (step);
        } while (at != start && loop.size () <= s.vertices.size () * 2);
        if (at != start)
            return false;
        loops.push_back (std::move (loop));
    }
    return true;
}

// `heights` (optional, one per vertex): the face's normal at those heights instead of its own.
Vec3 Normal (const Surface& s, uint32_t face, const std::vector<double>* heights = nullptr)
{
    const auto& t = *s.triangles;
    const auto at = [&] (uint32_t v) {
        Vec3 p = s.vertices[v];
        if (heights)
            p.z = (*heights)[v];
        return p;
    };
    const Vec3 a = at (t[face * 3]), u = at (t[face * 3 + 1]) - a, v = at (t[face * 3 + 2]) - a;
    const Vec3 n { u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
    const double length = Length (n);
    return length > 1.0e-14 ? n * (1.0 / length) : Vec3 {};
}

// The edges folded by more than `degrees`, joined end to end: a chain runs from one branching
// or ending vertex to the next; what is left over are rings.
// `faces` (optional, one flag per triangle): only an edge both of whose faces it marks.
// `heights` (optional, one per vertex): the fold measured at those heights (#219: the existing
// ground's, so a design that moved a vertex by millimetres adds no fold of the ground).
std::vector<BakeLine> Creases (const Surface& s, double degrees, const std::vector<char>* faces = nullptr,
                               const std::vector<double>* heights = nullptr)
{
    const double limit = std::cos (std::clamp (degrees, 0.1, 89.0) * 3.14159265358979323846 / 180.0);
    std::map<uint32_t, std::vector<uint32_t>> around;
    for (const auto& [key, use] : s.edges) {
        if (use.count != 2 || (faces && (!(*faces)[use.faces[0]] || !(*faces)[use.faces[1]])))
            continue;
        const Vec3 a = Normal (s, use.faces[0], heights), b = Normal (s, use.faces[1], heights);
        if (Dot (a, a) == 0.0 || Dot (b, b) == 0.0 || Dot (a, b) >= limit)
            continue;
        around[use.from].push_back (use.to);
        around[use.to].push_back (use.from);
    }
    for (auto& [vertex, others] : around)
        std::sort (others.begin (), others.end ());
    std::map<uint64_t, bool> walked;
    std::vector<BakeLine> out;
    const auto walk = [&] (uint32_t from, uint32_t to) {
        BakeLine line;
        line.kind = BakeLineKind::Crease;
        line.points.push_back (s.vertices[from]);
        uint32_t before = from, at = to;
        walked[EdgeKey (from, to)] = true;
        for (;;) {
            line.points.push_back (s.vertices[at]);
            const auto& others = around[at];
            if (others.size () != 2 || at == from)
                break;
            const uint32_t on = others[0] == before ? others[1] : others[0];
            if (walked[EdgeKey (at, on)])
                break;
            walked[EdgeKey (at, on)] = true;
            before = at, at = on;
        }
        if (line.points.size () > 3 && at == from) {
            line.closed = true;
            line.points.pop_back ();
        }
        out.push_back (std::move (line));
    };
    for (const bool rings : { false, true })
        for (const auto& [vertex, others] : around)
            if (rings || others.size () != 2)
                for (const uint32_t other : others)
                    if (!walked[EdgeKey (vertex, other)])
                        walk (vertex, other);
    return out;
}

double RunLength (const BakeLine& line)
{
    double length = 0.0;
    for (size_t i = 1; i < line.points.size (); ++i)
        length += Length (line.points[i] - line.points[i - 1]);
    if (line.closed && line.points.size () > 2)
        length += Length (line.points.front () - line.points.back ());
    return length;
}

// The pieces of `line` whose points `keep` says, in order: a closed line kept whole stays closed;
// otherwise each run of two or more kept points is an open line (a ring's run may wrap round).
std::vector<BakeLine> Runs (const BakeLine& line, const std::vector<char>& keep)
{
    const size_t n = line.points.size ();
    std::vector<BakeLine> out;
    if (std::all_of (keep.begin (), keep.end (), [] (char k) { return k != 0; })) {
        out.push_back (line);
        return out;
    }
    size_t start = 0;
    if (line.closed) // begin just after a dropped point, so no run is cut at the seam
        while (keep[start])
            ++start;
    BakeLine run;
    run.kind = line.kind;
    const auto flush = [&] {
        if (run.points.size () >= 2)
            out.push_back (run);
        run.points.clear ();
    };
    for (size_t i = 0; i < n; ++i) {
        const size_t at = line.closed ? (start + 1 + i) % n : i;
        if (keep[at])
            run.points.push_back (line.points[at]);
        else
            flush ();
    }
    flush ();
    return out;
}

// Where the surface is in plan: triangles binned by their bounding boxes on a uniform grid.
struct Locator {
    const Surface& s;
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0, cell = 1.0;
    int nx = 1, ny = 1;
    std::vector<std::vector<uint32_t>> cells;

    explicit Locator (const Surface& surface) : s (surface)
    {
        x0 = y0 = INFINITY, x1 = y1 = -INFINITY;
        for (const auto& v : s.vertices)
            x0 = std::min (x0, v.x), y0 = std::min (y0, v.y), x1 = std::max (x1, v.x), y1 = std::max (y1, v.y);
        const auto& t = *s.triangles;
        const double w = std::max (x1 - x0, 1.0e-9), h = std::max (y1 - y0, 1.0e-9);
        cell = std::max (std::sqrt (w * h / double (std::max<size_t> (t.size () / 3, 1))) * 2.0, 1.0e-6);
        nx = int (std::clamp (w / cell + 1.0, 1.0, 1024.0));
        ny = int (std::clamp (h / cell + 1.0, 1.0, 1024.0));
        cells.resize (size_t (nx) * size_t (ny));
        for (size_t f = 0; f + 2 < t.size (); f += 3) {
            double fx0 = INFINITY, fy0 = INFINITY, fx1 = -INFINITY, fy1 = -INFINITY;
            for (int k = 0; k < 3; ++k) {
                const auto& v = s.vertices[t[f + k]];
                fx0 = std::min (fx0, v.x), fy0 = std::min (fy0, v.y), fx1 = std::max (fx1, v.x),
                fy1 = std::max (fy1, v.y);
            }
            for (int j = Iy (fy0); j <= Iy (fy1); ++j)
                for (int i = Ix (fx0); i <= Ix (fx1); ++i)
                    cells[size_t (j) * size_t (nx) + size_t (i)].push_back (uint32_t (f / 3));
        }
    }
    int Ix (double x) const
    {
        return std::clamp (int ((x - x0) / cell), 0, nx - 1);
    }
    int Iy (double y) const
    {
        return std::clamp (int ((y - y0) / cell), 0, ny - 1);
    }
    // The surface's height at (x, y); false off the surface.
    bool Height (double x, double y, double& z) const
    {
        constexpr double kEdge = 1.0e-9;
        if (x < x0 - kOnLine || x > x1 + kOnLine || y < y0 - kOnLine || y > y1 + kOnLine)
            return false;
        const auto& t = *s.triangles;
        for (const uint32_t f : cells[size_t (Iy (y)) * size_t (nx) + size_t (Ix (x))]) {
            const Vec3 a = s.vertices[t[f * 3]], b = s.vertices[t[f * 3 + 1]], c = s.vertices[t[f * 3 + 2]];
            const double d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
            if (std::abs (d) < 1.0e-18)
                continue;
            const double l1 = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / d;
            const double l2 = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / d;
            const double l3 = 1.0 - l1 - l2;
            if (l1 >= -kEdge && l2 >= -kEdge && l3 >= -kEdge) {
                z = l1 * a.z + l2 * b.z + l3 * c.z;
                return true;
            }
        }
        return false;
    }
};

// The authored rings at the surface's heights; where a ring leaves the surface it is cut there.
std::vector<BakeLine> Drape (const Locator& at, const std::vector<std::vector<Vec3>>& rings)
{
    std::vector<BakeLine> out;
    for (const auto& ring : rings) {
        if (ring.size () < 3)
            continue;
        BakeLine line;
        line.kind = BakeLineKind::Outline;
        line.closed = true;
        std::vector<char> keep;
        for (const auto& p : ring) {
            double z = 0.0;
            const bool on = std::isfinite (p.x) && std::isfinite (p.y) && at.Height (p.x, p.y, z);
            line.points.push_back ({ p.x, p.y, on ? z : 0.0 });
            keep.push_back (on);
        }
        // An authored ring may repeat its first point at its end.
        if (line.points.size () > 3 && Length (line.points.front () - line.points.back ()) < kOnLine)
            line.points.pop_back (), keep.pop_back ();
        for (auto& piece : Runs (line, keep))
            out.push_back (std::move (piece));
    }
    return out;
}

// Inside the rings in plan, by crossings: a hole (a ring inside another) is outside.
bool InsideRings (Vec3 p, const std::vector<std::vector<Vec3>>& rings)
{
    bool inside = false;
    for (const auto& ring : rings)
        for (size_t i = 0, j = ring.size () - 1; i < ring.size (); j = i++) {
            const Vec3 a = ring[i], b = ring[j];
            if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
                inside = !inside;
        }
    return inside;
}

// The toe: where the surface leaves the existing ground by `ground`, traced across the triangles
// as one level line of |z - base| and joined end to end.
std::vector<BakeLine> Toe (const Surface& s, const std::vector<double>& base, double ground)
{
    const auto& t = *s.triangles;
    std::vector<double> moved (s.vertices.size ());
    for (size_t i = 0; i < moved.size (); ++i)
        moved[i] = std::abs (s.vertices[i].z - base[i]);
    const auto away = [&] (uint32_t v) { return moved[v] >= ground; };
    std::unordered_map<uint64_t, Vec3> crossing;
    const auto cross = [&] (uint32_t a, uint32_t b) {
        const uint64_t key = EdgeKey (a, b);
        if (!crossing.count (key)) {
            const double span = moved[b] - moved[a];
            const double f = std::abs (span) > 0.0 ? std::clamp ((ground - moved[a]) / span, 0.0, 1.0) : 0.5;
            crossing[key] = s.vertices[a] + (s.vertices[b] - s.vertices[a]) * f;
        }
        return key;
    };
    std::vector<std::pair<uint64_t, uint64_t>> segments;
    std::unordered_map<uint64_t, std::vector<size_t>> ends;
    for (size_t f = 0; f + 2 < t.size (); f += 3) {
        uint64_t keys[2] = { 0, 0 };
        int count = 0;
        for (int e = 0; e < 3 && count < 2; ++e) {
            const uint32_t a = t[f + e], b = t[f + (e + 1) % 3];
            if (away (a) != away (b))
                keys[count++] = cross (a, b);
        }
        if (count != 2)
            continue;
        ends[keys[0]].push_back (segments.size ());
        ends[keys[1]].push_back (segments.size ());
        segments.push_back ({ keys[0], keys[1] });
    }
    std::vector<char> used (segments.size (), 0);
    std::vector<BakeLine> out;
    const auto walk = [&] (uint64_t from) {
        BakeLine line;
        line.kind = BakeLineKind::Toe;
        line.points.push_back (crossing[from]);
        uint64_t at = from;
        for (;;) {
            size_t next = segments.size ();
            for (const size_t k : ends[at])
                if (!used[k]) {
                    next = k;
                    break;
                }
            if (next == segments.size ())
                break;
            used[next] = 1;
            at = segments[next].first == at ? segments[next].second : segments[next].first;
            line.points.push_back (crossing[at]);
            if (at == from)
                break;
        }
        if (line.points.size () > 3 && at == from) {
            line.closed = true;
            line.points.pop_back ();
        }
        if (line.points.size () >= 2)
            out.push_back (std::move (line));
    };
    // Open lines first, from their ends on the surface's edge; then the rings.
    std::vector<uint64_t> starts;
    for (const auto& [key, at] : ends)
        if (at.size () == 1)
            starts.push_back (key);
    std::sort (starts.begin (), starts.end ());
    for (const uint64_t key : starts)
        walk (key);
    for (size_t k = 0; k < segments.size (); ++k)
        if (!used[k])
            walk (segments[k].first);
    return out;
}
// Surface: the outline is the boundary as it is; the kept edges are lines; every other vertex is
// put on a line walked along the surface's edges, so the Mesh's own triangulation of the same
// points, constrained by those lines, is this surface.
BakePlan SurfaceLines (const Surface& s, const std::vector<std::vector<uint32_t>>& loops, const std::vector<Vec3>& ring,
                       const std::vector<std::array<uint32_t, 2>>& edges, BakePlan& plan)
{
    const size_t n = s.vertices.size ();
    std::vector<char> covered (n, 0);
    for (const auto& loop : loops)
        for (const uint32_t v : loop)
            covered[v] = 1;
    // The kept edges, chained where two meet at a point.
    std::map<uint32_t, std::vector<uint32_t>> around;
    std::set<uint64_t> kept;
    for (const auto& [a, b] : edges) {
        if (a >= n || b >= n || a == b || !s.edges.contains (EdgeKey (a, b)))
            continue; // not an edge of this surface
        const auto& use = s.edges.at (EdgeKey (a, b));
        if (use.count < 2 || !kept.insert (EdgeKey (a, b)).second)
            continue; // the boundary is the outline's; a duplicate once
        around[a].push_back (b);
        around[b].push_back (a);
    }
    std::set<uint64_t> walked;
    const auto walk = [&] (uint32_t from, uint32_t to) {
        BakeLine line;
        line.kind = BakeLineKind::Outline;
        line.points.push_back (s.vertices[from]);
        covered[from] = 1;
        uint32_t at = from, next = to;
        while (true) {
            walked.insert (EdgeKey (at, next));
            line.points.push_back (s.vertices[next]);
            covered[next] = 1;
            const auto& here = around[next];
            if (here.size () != 2)
                break;
            const uint32_t after = here[0] == at ? here[1] : here[0];
            if (walked.contains (EdgeKey (next, after)))
                break; // a loop closed
            at = next, next = after;
        }
        line.traced = line.points.size ();
        plan.points += line.points.size ();
        plan.traced += line.points.size ();
        plan.lines.push_back (std::move (line));
    };
    for (const auto& [v, list] : around)
        if (list.size () != 2)
            for (const uint32_t w : list)
                if (!walked.contains (EdgeKey (v, w)))
                    walk (v, w);
    for (const auto& [v, list] : around)
        for (const uint32_t w : list)
            if (!walked.contains (EdgeKey (v, w)))
                walk (v, w); // the loops left
    // Every other vertex of the surface is a level point.
    std::vector<char> used (n, 0);
    for (size_t k = 0; k < s.triangles->size (); ++k)
        used[(*s.triangles)[k]] = 1;
    for (uint32_t v = 0; v < n; ++v)
        if (used[v] && !covered[v]) {
            plan.levelPoints.push_back (s.vertices[v]);
            ++plan.points, ++plan.traced;
        }
    plan.outline = ring;
    if (plan.holes)
        plan.notes.push_back (std::to_string (plan.holes) +
                              (plan.holes == 1 ? " inner opening is" : " inner openings are") +
                              " not written: the Mesh covers the area inside.");
    plan.ok = true;
    return plan;
}
} // namespace

BakePlan PlanTerrainBake (const std::vector<double>& points, const std::vector<uint32_t>& triangles,
                          const BakeOptions& options, const BakeDesign& design)
{
    BakePlan plan;
    const auto fail = [&] (const char* why) {
        plan.error = why;
        return plan;
    };
    if (points.size () < 9 || points.size () % 3 != 0 || triangles.size () < 3 || triangles.size () % 3 != 0)
        return fail ("BakeSurfaceEmpty: there is no built terrain surface to make a Mesh from.");
    if (!(options.tolerance >= 0.0) || !std::isfinite (options.tolerance) || !(options.interval >= 0.0) ||
        !std::isfinite (options.interval))
        return fail ("BakeOptions: the interval and the tolerance must be zero or more.");
    Surface s;
    s.triangles = &triangles;
    plan.lo = INFINITY, plan.hi = -INFINITY;
    for (size_t i = 0; i + 2 < points.size (); i += 3) {
        if (!std::isfinite (points[i]) || !std::isfinite (points[i + 1]) || !std::isfinite (points[i + 2]))
            return fail ("BakeSurfaceInvalid: the built terrain has a point that is not a number.");
        s.vertices.push_back ({ points[i], points[i + 1], points[i + 2] });
        plan.lo = std::min (plan.lo, points[i + 2]), plan.hi = std::max (plan.hi, points[i + 2]);
    }
    for (size_t i = 0; i + 2 < triangles.size (); i += 3)
        for (int e = 0; e < 3; ++e) {
            const uint32_t a = triangles[i + e], b = triangles[i + (e + 1) % 3];
            if (a >= s.vertices.size () || b >= s.vertices.size () || a == b)
                return fail ("BakeSurfaceInvalid: the built terrain has a triangle that names no point.");
            auto& use = s.edges[EdgeKey (a, b)];
            if (use.count == 0)
                use.from = a, use.to = b;
            if (use.count < 2)
                use.faces[use.count] = uint32_t (i / 3);
            ++use.count;
        }

    // The boundary: the largest loop, counter-clockwise. The others are holes.
    std::vector<std::vector<uint32_t>> loops;
    if (!BoundaryLoops (s, loops) || loops.empty ())
        return fail ("BakeBoundary: the built terrain's edge is not a closed outline.");
    std::vector<Vec3> ring;
    for (const auto& loop : loops) {
        std::vector<Vec3> candidate;
        for (const uint32_t v : loop)
            candidate.push_back (s.vertices[v]);
        if (std::abs (Area (candidate)) > std::abs (Area (ring)))
            ring = std::move (candidate);
    }
    if (ring.size () < 3 || std::abs (Area (ring)) < 1.0e-9)
        return fail ("BakeBoundary: the built terrain's outline encloses no area.");
    if (Area (ring) < 0.0)
        std::reverse (ring.begin (), ring.end ());
    plan.holes = loops.size () - 1;
    plan.outlineTraced = ring.size ();
    if (options.mode == BakeMode::Surface)
        return SurfaceLines (s, loops, ring, design.edges, plan);

    // The level lines, as traced.
    const double range = plan.hi - plan.lo;
    const bool signature = options.mode == BakeMode::Signature;
    plan.interval = options.interval > 0.0 ? options.interval
                    : range > 1.0e-9       ? std::max (NiceStep (range, signature ? 4 : 10), 0.01)
                                           : 0.0;
    std::vector<BakeLine> traced;
    // Signature: the design's own lines first -- its outlines on the surface, then its toe
    // outside them. The ground it moved is described by those, so the soft-ground contours run
    // only where it moved nothing.
    const bool toe =
        signature && design.base.size () == s.vertices.size () && design.ground > 0.0 &&
        std::isfinite (design.ground) &&
        std::all_of (design.base.begin (), design.base.end (), [] (double z) { return std::isfinite (z); });
    const bool designed = signature && (toe || !design.outlines.empty ());
    if (signature && !design.outlines.empty ()) {
        const Locator at (s);
        for (auto& line : Drape (at, design.outlines))
            traced.push_back (std::move (line));
    }
    if (toe)
        for (const auto& line : Toe (s, design.base, design.ground)) {
            std::vector<char> outside;
            for (const auto& p : line.points)
                outside.push_back (!InsideRings (p, design.outlines));
            for (auto& piece : Runs (line, outside))
                traced.push_back (std::move (piece));
        }
    if (designed) // An open design line shorter than `shortest` is a fragment, not a line.
        std::erase_if (traced, [&] (const BakeLine& line) {
            return !line.closed && RunLength (line) < std::max (options.shortest, 0.0);
        });
    // The ground the design left alone keeps its own shape (the user, 2026-11-16: "existing
    // points should be kept or be redefined to lines"): its folds are lines -- a raised mesh point
    // is where its edges fold, so it becomes the lines through it, and level ground gives none.
    // The graded surface's folds only when asked; without a design, every fold.
    std::vector<char> untouched;
    if (toe) {
        untouched.assign (triangles.size () / 3, 0);
        for (size_t f = 0; f < untouched.size (); ++f)
            untouched[f] = std::all_of (triangles.begin () + f * 3, triangles.begin () + f * 3 + 3, [&] (uint32_t v) {
                return std::abs (s.vertices[v].z - design.base[v]) < design.ground;
            });
    }
    // The existing ground's own vertices, where given: its untouched ones become level points.
    const bool own = toe && design.existing.size () == s.vertices.size () &&
                     std::any_of (design.existing.begin (), design.existing.end (), [] (char e) { return e != 0; });
    if (signature) {
        const bool every = options.folds || !designed;
        if (every || (toe && !own))
            // The untouched ground's folds are its own, at the heights it had: a transition's
            // last millimetres before the ground (#219: refined to 1 cm) are the design's.
            for (auto& line :
                 Creases (s, options.creaseDegrees, every ? nullptr : &untouched, every ? nullptr : &design.base))
                traced.push_back (std::move (line));
    }
    // A peak or pit of the untouched ground that no line passes through yet (a gentle one, under
    // the fold angle) gets one: from it down (or up) its steepest edge, on the surface. With the
    // ground's own points given, they keep it.
    if (toe && !own) {
        std::vector<char> lined (s.vertices.size (), 0), inner (s.vertices.size (), 1);
        std::vector<std::vector<uint32_t>> next (s.vertices.size ());
        for (const auto& [key, use] : s.edges) {
            next[use.from].push_back (use.to);
            next[use.to].push_back (use.from);
            const bool open = use.count != 2 || !untouched[use.faces[0]] || !untouched[use.faces[1]];
            if (open) // on the boundary or beside the design: the outline or the toe describes it
                inner[use.from] = inner[use.to] = 0;
        }
        std::map<std::tuple<double, double, double>, uint32_t> at;
        for (uint32_t v = 0; v < s.vertices.size (); ++v)
            at[{ s.vertices[v].x, s.vertices[v].y, s.vertices[v].z }] = v;
        for (const auto& line : traced)
            for (const auto& p : line.points)
                if (const auto found = at.find ({ p.x, p.y, p.z }); found != at.end ())
                    lined[found->second] = 1;
        const double rise = std::max (options.tolerance, 1.0e-9);
        for (uint32_t v = 0; v < s.vertices.size (); ++v) {
            if (!inner[v] || lined[v] || next[v].empty ())
                continue;
            const double z = s.vertices[v].z;
            const bool peak =
                std::all_of (next[v].begin (), next[v].end (), [&] (uint32_t u) { return z - s.vertices[u].z > rise; });
            const bool pit =
                std::all_of (next[v].begin (), next[v].end (), [&] (uint32_t u) { return s.vertices[u].z - z > rise; });
            if (!peak && !pit)
                continue;
            const uint32_t steepest =
                *std::max_element (next[v].begin (), next[v].end (), [&] (uint32_t a, uint32_t b) {
                    return std::abs (s.vertices[a].z - z) / std::max (Length (s.vertices[a] - s.vertices[v]), 1.0e-12) <
                           std::abs (s.vertices[b].z - z) / std::max (Length (s.vertices[b] - s.vertices[v]), 1.0e-12);
                });
            BakeLine line;
            line.kind = BakeLineKind::Crease;
            line.points = { s.vertices[v], s.vertices[steepest] };
            traced.push_back (std::move (line));
        }
    }
    if (plan.interval > 0.0) {
        PavedArea surface;
        surface.vertices = s.vertices;
        if (toe) {
            for (size_t f = 0; f < untouched.size (); ++f)
                if (untouched[f])
                    surface.triangles.insert (surface.triangles.end (), triangles.begin () + f * 3,
                                              triangles.begin () + f * 3 + 3);
        }
        else
            surface.triangles = triangles;
        const auto contours = SurfaceContours (surface, plan.interval);
        if (contours.empty () && range / plan.interval > 400.0)
            plan.notes.push_back ("No contours: the interval cuts more than 400 levels. Use a larger one.");
        for (const auto& contour : contours) {
            BakeLine line;
            line.points = contour.points;
            if (line.points.size () > 3 && Length (line.points.front () - line.points.back ()) < kOnLine) {
                line.closed = true;
                line.points.pop_back ();
            }
            traced.push_back (std::move (line));
        }
    }

    // Where an open line ends on the boundary, the boundary gets that point, and keeps it.
    std::vector<char> fixed (ring.size (), 0);
    std::vector<std::vector<std::pair<double, Vec3>>> inserted (ring.size ());
    for (const auto& line : traced) {
        if (line.closed || line.points.size () < 2)
            continue;
        for (const Vec3 end : { line.points.front (), line.points.back () }) {
            size_t segment = ring.size ();
            double along = 0.0;
            bool vertex = false;
            for (size_t i = 0; i < ring.size () && !vertex; ++i) {
                double t = 0.0;
                if (FromSegment (end, ring[i], ring[(i + 1) % ring.size ()], true, &t) > kOnLine)
                    continue;
                const size_t near = t < 0.5 ? i : (i + 1) % ring.size ();
                if (FromSegment (end, ring[near], ring[near], true) <= kOnLine)
                    fixed[near] = 1, vertex = true;
                else
                    segment = i, along = t;
            }
            if (!vertex && segment < ring.size ())
                inserted[segment].push_back ({ along, end });
        }
    }
    std::vector<Vec3> threaded;
    std::vector<char> anchors;
    for (size_t i = 0; i < ring.size (); ++i) {
        threaded.push_back (ring[i]);
        anchors.push_back (fixed[i]);
        auto& here = inserted[i];
        std::sort (here.begin (), here.end (), [] (const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t k = 0; k < here.size (); ++k)
            if (k == 0 || here[k].first - here[k - 1].first > 1.0e-12) {
                threaded.push_back (here[k].second);
                anchors.push_back (1);
            }
    }

    // A line that runs along the boundary (a level the surface only touches at its edge) is the
    // outline's own, not a level line.
    const auto onRing = [&] (Vec3 p) {
        for (size_t i = 0; i < ring.size (); ++i)
            if (FromSegment (p, ring[i], ring[(i + 1) % ring.size ()], true) <= kOnLine)
                return true;
        return false;
    };
    std::erase_if (traced, [&] (const BakeLine& line) {
        return !line.closed && line.points.size () >= 2 && onRing (line.points.front ()) &&
               onRing (line.points.back ()) && std::all_of (line.points.begin (), line.points.end (), onRing) &&
               onRing ((line.points[0] + line.points[1]) * 0.5);
    });

    const double tolerance = std::max (options.tolerance, 1.0e-9);
    plan.outline = ReduceRing (threaded, anchors, tolerance);
    // #177: the contours divided together, so a long step cannot make one cross another.
    const bool divide = options.divide > 0.0;
    std::vector<std::vector<Vec3>> contours;
    std::vector<char> rings;
    if (divide) {
        for (const auto& line : traced)
            if (line.kind == BakeLineKind::Contour)
                contours.push_back (line.points), rings.push_back (line.closed ? 1 : 0);
        auto divided = DivideContours (contours, rings, plan.outline, options.divide, tolerance);
        if (!divided.ok) {
            plan.error = divided.error;
            return plan;
        }
        if (divided.shortened)
            plan.notes.push_back (std::to_string (divided.shortened) +
                                  (divided.shortened == 1 ? " contour is" : " contours are") +
                                  " divided more finely where a piece would cross a neighbour or the outline.");
        contours = std::move (divided.lines);
    }
    size_t nextContour = 0;
    for (auto& line : traced) {
        line.traced = line.points.size ();
        if (divide && line.kind == BakeLineKind::Contour)
            line.points = std::move (contours[nextContour++]);
        else
            line.points = line.closed ? ReduceRing (line.points, {}, tolerance) : ReduceOpen (line.points, tolerance);
        // A line shorter than the tolerance says nothing the reduction would keep.
        if (line.points.size () < 2 || (line.closed && line.points.size () < 3) || RunLength (line) <= tolerance)
            continue;
        plan.points += line.points.size ();
        plan.traced += line.traced;
        plan.lines.push_back (std::move (line));
    }

    if (own) {
        // The untouched ones, but not on the boundary (the outline's) nor on a line already.
        std::set<std::pair<double, double>> taken;
        for (const auto& line : plan.lines)
            for (const auto& p : line.points)
                taken.emplace (p.x, p.y);
        std::vector<char> rim (s.vertices.size (), 0), used (s.vertices.size (), 0);
        for (const auto& loop : loops)
            for (const uint32_t v : loop)
                rim[v] = 1;
        for (const uint32_t v : triangles)
            used[v] = 1;
        for (uint32_t v = 0; v < s.vertices.size (); ++v) {
            const auto& p = s.vertices[v];
            if (!used[v] || !design.existing[v] || rim[v] || std::abs (p.z - design.base[v]) >= design.ground ||
                !taken.emplace (p.x, p.y).second)
                continue;
            plan.levelPoints.push_back (p);
            ++plan.points, ++plan.traced;
        }
    }
    if (plan.holes)
        plan.notes.push_back (std::to_string (plan.holes) +
                              (plan.holes == 1 ? " inner opening is" : " inner openings are") +
                              " not written: the Mesh covers the area inside.");
    if (!signature && range > 1.0e-9)
        plan.notes.push_back ("A flat area between two levels is not in a contour Mesh. Use Signature lines "
                              "where hardscape must keep its height.");
    if (signature && !designed)
        plan.notes.push_back ("No design lines were given: the lines are the surface's folds.");
    if (plan.lines.empty () && range > 1.0e-9)
        plan.notes.push_back ("No level lines: the Mesh would be shaped by its outline alone.");
    plan.ok = true;
    return plan;
}

BakePayload TerrainBakePayload (const BakePlan& plan, double skirtDepth)
{
    BakePayload out;
    if (!plan.ok)
        return out;
    out.baseLevel = plan.lo - std::max (skirtDepth, 0.0);
    for (const auto& p : plan.outline) {
        out.outline.insert (out.outline.end (), { p.x, p.y });
        out.polyZ.push_back (p.z - out.baseLevel);
    }
    for (const auto& line : plan.lines) {
        for (const auto& p : line.points)
            out.ridgeCoords.insert (out.ridgeCoords.end (), { p.x, p.y, p.z - out.baseLevel });
        if (line.closed) {
            const auto& p = line.points.front ();
            out.ridgeCoords.insert (out.ridgeCoords.end (), { p.x, p.y, p.z - out.baseLevel });
        }
        out.ridgeCounts.push_back (int32_t (line.points.size () + (line.closed ? 1 : 0)));
    }
    for (const auto& p : plan.levelPoints) {
        out.ridgeCoords.insert (out.ridgeCoords.end (), { p.x, p.y, p.z - out.baseLevel });
        out.ridgeCounts.push_back (1);
    }
    out.levelPoints = plan.levelPoints.size ();
    return out;
}

std::vector<std::string> CheckBakePayload (const BakePayload& payload)
{
    std::vector<std::string> problems;
    const size_t n = payload.outline.size () / 2;
    if (n < 3 || payload.outline.size () % 2 != 0)
        problems.push_back ("The outline needs at least three points.");
    if (payload.polyZ.size () != n)
        problems.push_back ("The outline needs one height per point.");
    double twice = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        twice +=
            payload.outline[i * 2] * payload.outline[j * 2 + 1] - payload.outline[j * 2] * payload.outline[i * 2 + 1];
    }
    if (n >= 3 && std::abs (twice) < 1.0e-9)
        problems.push_back ("The outline encloses no area.");
    int64_t total = 0;
    const size_t lines = payload.ridgeCounts.size () - std::min (payload.levelPoints, payload.ridgeCounts.size ());
    for (size_t i = 0; i < payload.ridgeCounts.size (); ++i) {
        const auto count = payload.ridgeCounts[i];
        total += count;
        if (i < lines && count < 2) {
            problems.push_back ("A level line has fewer than two points.");
            break;
        }
        if (i >= lines && count != 1) {
            problems.push_back ("A level point is not one point.");
            break;
        }
    }
    if (payload.levelPoints > payload.ridgeCounts.size ())
        problems.push_back ("More level points than ridges.");
    if (total * 3 != int64_t (payload.ridgeCoords.size ()))
        problems.push_back ("The level lines' point counts do not add up to their points.");
    const auto sound = [] (const std::vector<double>& values) {
        return std::all_of (values.begin (), values.end (), [] (double v) { return std::isfinite (v); });
    };
    if (!sound (payload.outline) || !sound (payload.polyZ) || !sound (payload.ridgeCoords) ||
        !std::isfinite (payload.baseLevel))
        problems.push_back ("A coordinate is not a number.");
    for (size_t i = 0; i < payload.polyZ.size (); ++i)
        if (payload.polyZ[i] < -1.0e-9) {
            problems.push_back ("A height is under the Mesh's base plane.");
            break;
        }
    return problems;
}
} // namespace geomsrv::edit
