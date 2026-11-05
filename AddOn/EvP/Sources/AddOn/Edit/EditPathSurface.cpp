#include "Edit/EditPathSurface.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <array>
#include <limits>

namespace geomsrv::edit {
namespace {
double Cross (Vec3 a, Vec3 b, Vec3 c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
bool Weights (Vec3 p, Vec3 a, Vec3 b, Vec3 c, std::array<double, 3>& w)
{
    const double area = Cross (a, b, c);
    if (std::abs (area) < 1e-14)
        return false;
    w = { Cross (p, b, c) / area, Cross (a, p, c) / area, Cross (a, b, p) / area };
    return *std::min_element (w.begin (), w.end ()) >= -1e-9;
}
using Edge = std::pair<uint32_t, uint32_t>;
Edge Key (uint32_t a, uint32_t b)
{
    return { std::min (a, b), std::max (a, b) };
}
void ImproveEdges (PavedArea& area)
{
    struct Incident {
        size_t face;
        uint32_t opposite;
    };
    struct Use {
        Edge edge;
        Incident at;
    };
    std::vector<Use> uses;
    for (int pass = 0; pass < 8; ++pass) {
        // Every edge with the faces on it, in edge order and each edge's faces in the order
        // they were met: what a std::map<Edge, std::vector<Incident>> gives, without a node per edge.
        uses.clear ();
        for (size_t i = 0; i < area.triangles.size (); i += 3)
            for (int k = 0; k < 3; ++k)
                uses.push_back ({ Key (area.triangles[i + k], area.triangles[i + (k + 1) % 3]),
                                  { i, area.triangles[i + (k + 2) % 3] } });
        std::stable_sort (uses.begin (), uses.end (), [] (const Use& x, const Use& y) { return x.edge < y.edge; });
        std::vector<bool> changed (area.triangles.size () / 3, false);
        bool flipped = false;
        for (size_t first = 0, last = 0; first < uses.size (); first = last) {
            last = first + 1;
            while (last < uses.size () && uses[last].edge == uses[first].edge)
                ++last;
            const Edge edge = uses[first].edge;
            if (last - first != 2)
                continue;
            const Incident faces[2] = { uses[first].at, uses[first + 1].at };
            if (changed[faces[0].face / 3] || changed[faces[1].face / 3])
                continue;
            const auto [a, b] = edge;
            const uint32_t c = faces[0].opposite, d = faces[1].opposite;
            const Vec3 pa = area.vertices[a], pb = area.vertices[b], pc = area.vertices[c], pd = area.vertices[d];
            if (Cross (pa, pb, pc) * Cross (pa, pb, pd) >= -1e-18 || Cross (pc, pd, pa) * Cross (pc, pd, pb) >= -1e-18)
                continue;
            const auto cotangent = [] (Vec3 a, Vec3 b, Vec3 at) {
                return ((a.x - at.x) * (b.x - at.x) + (a.y - at.y) * (b.y - at.y)) / std::abs (Cross (at, a, b));
            };
            if (cotangent (pa, pb, pc) + cotangent (pa, pb, pd) >= -1e-8)
                continue;
            const auto write = [&] (size_t i, uint32_t x, uint32_t y, uint32_t z) {
                if (Cross (area.vertices[x], area.vertices[y], area.vertices[z]) < 0)
                    std::swap (y, z);
                area.triangles[i] = x;
                area.triangles[i + 1] = y;
                area.triangles[i + 2] = z;
                changed[i / 3] = true;
            };
            write (faces[0].face, c, d, a);
            write (faces[1].face, d, c, b);
            flipped = true;
        }
        if (!flipped)
            break;
    }
}
} // namespace

bool PavedHeight (const PavedArea& area, Vec3 point, double& z)
{
    for (size_t i = 0; i + 2 < area.triangles.size (); i += 3) {
        const Vec3 a = area.vertices[area.triangles[i]], b = area.vertices[area.triangles[i + 1]],
                   c = area.vertices[area.triangles[i + 2]];
        std::array<double, 3> w;
        if (Weights (point, a, b, c, w)) {
            z = w[0] * a.z + w[1] * b.z + w[2] * c.z;
            return std::isfinite (z);
        }
    }
    return false;
}

void OntoPavedSurface (const PavedArea& area, std::vector<Vec3>& points)
{
    for (auto& p : points) {
        double z = 0.0;
        if (PavedHeight (area, p, z))
            p.z = z;
    }
}

bool BuildLinearSurface (PavedArea& area, const Path& path, const AlignmentGeometry& geometry,
                         const PathPreparation& preparation)
{
    if (geometry.elements.size () != 1 || geometry.elements[0].kind != ElementKind::Line ||
        path.sections.size () != 1 || path.edgeRadius != 0 || area.rings.size () != 1 || area.rings[0].size () != 4)
        return false;
    for (const auto& b : path.profile)
        if (b.curve != 0)
            return false;
    for (const auto& k : path.kerbs)
        if (k.radius != 0)
            return false;
    StationFrame start, end;
    if (!Evaluate (path, geometry, preparation, 0, start) ||
        !Evaluate (path, geometry, preparation, geometry.length, end))
        return false;
    const auto boundary = [&] (Vec3 p) {
        return *std::min_element (area.rings[0].begin (), area.rings[0].end (),
                                  [&] (Vec3 a, Vec3 b) { return DistanceXY (a, p) < DistanceXY (b, p); });
    };
    const Vec3 sl = boundary (start.leftEdge), sr = boundary (start.rightEdge), el = boundary (end.leftEdge),
               er = boundary (end.rightEdge);
    area.vertices.clear ();
    area.triangles.clear ();
    for (double station : SampleStations (path, geometry, 0)) {
        StationFrame f;
        Evaluate (path, geometry, preparation, station, f);
        Vec3 left = Lerp (sl, el, station / geometry.length), right = Lerp (sr, er, station / geometry.length);
        left.z = f.leftEdge.z;
        right.z = f.rightEdge.z;
        const auto n = uint32_t (area.vertices.size ());
        area.vertices.insert (area.vertices.end (), { left, f.centre, right });
        if (n == 0)
            continue;
        for (uint32_t k = 0; k < 2; ++k) {
            area.triangles.insert (area.triangles.end (), { n - 3 + k, n - 2 + k, n + k, n - 2 + k, n + 1 + k, n + k });
        }
    }
    return true;
}

void RefinePavedSurface (PavedArea& area, const std::vector<Vec3>& samples, double step,
                         const std::function<double (Vec3)>& height, double tolerance)
{
    if (!std::isfinite (step) || step <= 0 || area.triangles.empty ())
        return;
    constexpr size_t kMaxTriangles = 250000;
    std::map<std::pair<int64_t, int64_t>, uint32_t> known;
    std::vector<Vec3> vertices;
    const auto key = [] (Vec3 p) {
        return std::pair { int64_t (std::llround (p.x * 1e8)), int64_t (std::llround (p.y * 1e8)) };
    };
    const auto add = [&] (Vec3 p) {
        const auto k = key (p);
        const auto found = known.find (k);
        if (found != known.end ())
            return found->second;
        const auto id = uint32_t (vertices.size ());
        p.z = height (p);
        vertices.push_back (p);
        known[k] = id;
        return id;
    };
    for (auto& index : area.triangles)
        index = add (area.vertices[index]);
    area.vertices = std::move (vertices);
    // Insert each authored sample on all incident triangles, including both sides
    // of an edge. The same plan point must never get two competing heights.
    for (const auto& p : samples) {
        if (known.contains (key (p)) || area.triangles.size () / 3 >= kMaxTriangles)
            continue;
        // The triangles it splits. One it lies well outside (beyond the barycentric tolerance)
        // is never one of them, so a sample that splits none leaves the mesh as it was.
        const auto splits = [&] (size_t i, std::array<double, 3>& w) {
            const Vec3 &a = area.vertices[area.triangles[i]], &b = area.vertices[area.triangles[i + 1]],
                       &c = area.vertices[area.triangles[i + 2]];
            constexpr double e = 1e-6;
            if (p.x < std::min ({ a.x, b.x, c.x }) - e || p.x > std::max ({ a.x, b.x, c.x }) + e ||
                p.y < std::min ({ a.y, b.y, c.y }) - e || p.y > std::max ({ a.y, b.y, c.y }) + e)
                return false;
            return Weights (p, a, b, c, w) && *std::max_element (w.begin (), w.end ()) <= 1 - 1e-9;
        };
        size_t firstSplit = area.triangles.size ();
        for (size_t i = 0; i + 2 < area.triangles.size (); i += 3) {
            std::array<double, 3> w;
            if (splits (i, w)) {
                firstSplit = i;
                break;
            }
        }
        if (firstSplit == area.triangles.size ())
            continue;
        std::vector<uint32_t> next (area.triangles.begin (), area.triangles.begin () + firstSplit);
        next.reserve (area.triangles.size () + 6);
        uint32_t made = UINT32_MAX;
        // Triangles it does not split are copied a run at a time, in place.
        size_t run = firstSplit;
        for (size_t i = firstSplit; i + 2 < area.triangles.size (); i += 3) {
            const uint32_t a = area.triangles[i], b = area.triangles[i + 1], c = area.triangles[i + 2];
            std::array<double, 3> w;
            if (!splits (i, w))
                continue;
            next.insert (next.end (), area.triangles.begin () + run, area.triangles.begin () + i);
            run = i + 3;
            if (made == UINT32_MAX) {
                made = uint32_t (area.vertices.size ());
                area.vertices.push_back ({ p.x, p.y, height (p) });
                known[key (p)] = made;
            }
            const uint32_t ids[] = { a, b, c };
            for (int k = 0; k < 3; ++k)
                if (w[(k + 2) % 3] > 1e-9)
                    next.insert (next.end (), { ids[k], ids[(k + 1) % 3], made });
        }
        next.insert (next.end (), area.triangles.begin () + run,
                     area.triangles.begin () + (area.triangles.size () / 3) * 3);
        area.triangles = std::move (next);
    }
    ImproveEdges (area);
    // Mark edges globally before splitting triangles: neighbours always receive
    // the same midpoint, so refinement introduces neither cracks nor T-junctions.
    // The long edges in key order, each with its midpoint's vertex: a sorted table rather
    // than a std::map, the vertices numbered in the same order.
    std::vector<std::pair<Edge, uint32_t>> mid;
    const auto midpoint = [&] (uint32_t a, uint32_t b) {
        const Edge key = Key (a, b);
        const auto at =
            std::lower_bound (mid.begin (), mid.end (), key,
                              [] (const std::pair<Edge, uint32_t>& m, const Edge& k) { return m.first < k; });
        return at != mid.end () && at->first == key ? at : mid.end ();
    };
    std::set<Edge> flat;
    std::set<std::array<uint32_t, 3>> smooth;
    for (int pass = 0; pass < 24; ++pass) {
        mid.clear ();
        for (size_t i = 0; i < area.triangles.size (); i += 3)
            for (int k = 0; k < 3; ++k) {
                const uint32_t a = area.triangles[i + k], b = area.triangles[i + (k + 1) % 3];
                if (DistanceXY (area.vertices[a], area.vertices[b]) > step * (1 + 1e-9))
                    mid.emplace_back (Key (a, b), 0);
            }
        std::sort (mid.begin (), mid.end ());
        mid.erase (std::unique (mid.begin (), mid.end ()), mid.end ());
        // With a tolerance, a long edge stays whole where the surface lies on its triangles'
        // planes within it: at its midpoint, and inside each triangle (its centroid and the
        // centroids of its corner thirds), where a blend can bulge between true midpoints. A
        // triangle that bulges splits its longest edge. The outline is always split by `step`:
        // what is graded from it (each edge's faces take its two vertices' sections) needs it dense.
        std::vector<double> heights;
        if (tolerance > 0) {
            std::vector<Edge> sides;
            sides.reserve (area.triangles.size ());
            for (size_t i = 0; i < area.triangles.size (); i += 3)
                for (int k = 0; k < 3; ++k)
                    sides.push_back (Key (area.triangles[i + k], area.triangles[i + (k + 1) % 3]));
            std::sort (sides.begin (), sides.end ());
            const auto outline = [&] (const Edge& edge) {
                const auto [first, last] = std::equal_range (sides.begin (), sides.end (), edge);
                return last - first == 1;
            };
            std::set<Edge> bulging;
            for (size_t i = 0; i < area.triangles.size (); i += 3) {
                const std::array<uint32_t, 3> t { area.triangles[i], area.triangles[i + 1], area.triangles[i + 2] };
                std::array<uint32_t, 3> id = t;
                std::sort (id.begin (), id.end ());
                if (smooth.contains (id))
                    continue;
                const Vec3 &a = area.vertices[t[0]], &b = area.vertices[t[1]], &c = area.vertices[t[2]];
                const double ab = DistanceXY (a, b), bc = DistanceXY (b, c), ca = DistanceXY (c, a);
                if (std::max ({ ab, bc, ca }) <= step * (1 + 1e-9))
                    continue;
                bool bulges = false;
                for (const auto& w :
                     { std::array { 1 / 3.0, 1 / 3.0, 1 / 3.0 }, std::array { 2 / 3.0, 1 / 6.0, 1 / 6.0 },
                       std::array { 1 / 6.0, 2 / 3.0, 1 / 6.0 }, std::array { 1 / 6.0, 1 / 6.0, 2 / 3.0 } }) {
                    const Vec3 p { w[0] * a.x + w[1] * b.x + w[2] * c.x, w[0] * a.y + w[1] * b.y + w[2] * c.y,
                                   w[0] * a.z + w[1] * b.z + w[2] * c.z };
                    if (std::abs (height (p) - p.z) > tolerance) {
                        bulges = true;
                        break;
                    }
                }
                if (!bulges) {
                    smooth.insert (id);
                    continue;
                }
                bulging.insert (ab >= bc && ab >= ca ? Key (t[0], t[1])
                                : bc >= ca           ? Key (t[1], t[2])
                                                     : Key (t[2], t[0]));
            }
            std::vector<std::pair<Edge, uint32_t>> kept;
            for (const auto& entry : mid) {
                const bool forced = bulging.contains (entry.first) || outline (entry.first);
                if (!forced && flat.contains (entry.first))
                    continue;
                Vec3 p = Lerp (area.vertices[entry.first.first], area.vertices[entry.first.second], 0.5);
                const double z = height (p);
                if (forced || std::abs (z - p.z) > tolerance) {
                    kept.push_back (entry);
                    heights.push_back (z);
                }
                else
                    flat.insert (entry.first);
            }
            mid = std::move (kept);
        }
        if (mid.empty () || area.triangles.size () / 3 + 2 * mid.size () > kMaxTriangles)
            break;
        for (size_t m = 0; m < mid.size (); ++m) {
            auto& [edge, index] = mid[m];
            Vec3 p = Lerp (area.vertices[edge.first], area.vertices[edge.second], 0.5);
            p.z = tolerance > 0 ? heights[m] : height (p);
            index = uint32_t (area.vertices.size ());
            area.vertices.push_back (p);
        }
        std::vector<uint32_t> next;
        next.reserve (area.triangles.size () + 9 * mid.size ());
        const auto triangle = [&] (uint32_t a, uint32_t b, uint32_t c) { next.insert (next.end (), { a, b, c }); };
        for (size_t i = 0; i < area.triangles.size (); i += 3) {
            const uint32_t a = area.triangles[i], b = area.triangles[i + 1], c = area.triangles[i + 2];
            const auto ab = midpoint (a, b), bc = midpoint (b, c), ca = midpoint (c, a);
            const int mask = (ab != mid.end () ? 1 : 0) | (bc != mid.end () ? 2 : 0) | (ca != mid.end () ? 4 : 0);
            const uint32_t x = ab != mid.end () ? ab->second : 0, y = bc != mid.end () ? bc->second : 0,
                           z = ca != mid.end () ? ca->second : 0;
            switch (mask) {
                case 0:
                    triangle (a, b, c);
                    break;
                case 1:
                    triangle (a, x, c);
                    triangle (x, b, c);
                    break;
                case 2:
                    triangle (b, y, a);
                    triangle (y, c, a);
                    break;
                case 4:
                    triangle (c, z, b);
                    triangle (z, a, b);
                    break;
                case 3:
                    triangle (b, y, x);
                    triangle (a, x, c);
                    triangle (x, y, c);
                    break;
                case 5:
                    triangle (a, x, z);
                    triangle (b, c, x);
                    triangle (x, c, z);
                    break;
                case 6:
                    triangle (c, z, y);
                    triangle (a, b, z);
                    triangle (z, b, y);
                    break;
                case 7:
                    triangle (a, x, z);
                    triangle (x, b, y);
                    triangle (z, y, c);
                    triangle (x, y, z);
                    break;
            }
        }
        area.triangles = std::move (next);
    }
    ImproveEdges (area);
}
} // namespace geomsrv::edit
