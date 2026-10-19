#include "Edit/EditPathSurface.hpp"
#include <algorithm>
#include <cmath>
#include <map>
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

void RefinePavedSurface (PavedArea& area, const std::vector<Vec3>& samples, double step,
                         const std::function<double (Vec3)>& height)
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
        std::vector<uint32_t> next;
        uint32_t made = UINT32_MAX;
        for (size_t i = 0; i + 2 < area.triangles.size (); i += 3) {
            const uint32_t a = area.triangles[i], b = area.triangles[i + 1], c = area.triangles[i + 2];
            std::array<double, 3> w;
            if (!Weights (p, area.vertices[a], area.vertices[b], area.vertices[c], w) ||
                *std::max_element (w.begin (), w.end ()) > 1 - 1e-9) {
                next.insert (next.end (), { a, b, c });
                continue;
            }
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
        area.triangles = std::move (next);
    }
    // Mark edges globally before splitting triangles: neighbours always receive
    // the same midpoint, so refinement introduces neither cracks nor T-junctions.
    for (int pass = 0; pass < 24; ++pass) {
        std::map<Edge, uint32_t> mid;
        for (size_t i = 0; i < area.triangles.size (); i += 3)
            for (int k = 0; k < 3; ++k) {
                const uint32_t a = area.triangles[i + k], b = area.triangles[i + (k + 1) % 3];
                if (DistanceXY (area.vertices[a], area.vertices[b]) > step * (1 + 1e-9))
                    mid.emplace (Key (a, b), 0);
            }
        if (mid.empty () || area.triangles.size () / 3 + 2 * mid.size () > kMaxTriangles)
            break;
        for (auto& [edge, index] : mid) {
            Vec3 p = Lerp (area.vertices[edge.first], area.vertices[edge.second], 0.5);
            p.z = height (p);
            index = uint32_t (area.vertices.size ());
            area.vertices.push_back (p);
        }
        std::vector<uint32_t> next;
        const auto triangle = [&] (uint32_t a, uint32_t b, uint32_t c) { next.insert (next.end (), { a, b, c }); };
        for (size_t i = 0; i < area.triangles.size (); i += 3) {
            const uint32_t a = area.triangles[i], b = area.triangles[i + 1], c = area.triangles[i + 2];
            const auto ab = mid.find (Key (a, b)), bc = mid.find (Key (b, c)), ca = mid.find (Key (c, a));
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
}
} // namespace geomsrv::edit
