#include "ArchViz/SunStudyCylinderContour.hpp"
#include "ArchViz/SectionCaps.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv::archviz {
namespace {
using Point = std::array<double, 3>;
constexpr double kTurn = 6.2831853071795864769;
constexpr size_t kSegmentLimit = 65536;

bool ReachesCircle (double xmin, double xmax, double ymin, double ymax, double r)
{
    const double dx = std::clamp (0., xmin, xmax), dy = std::clamp (0., ymin, ymax);
    return std::hypot (dx, dy) <= r &&
           std::hypot (std::max (std::abs (xmin), std::abs (xmax)), std::max (std::abs (ymin), std::abs (ymax))) >= r;
}

// Intersect an infinite plan line with the circle; parameter is metres along the line.
template <class Visit> void Crossings (const Point& a, double ux, double uy, double radius, Visit visit)
{
    const double distance = std::abs (a[0] * uy - a[1] * ux);
    if (distance > radius)
        return;
    const double along = -(a[0] * ux + a[1] * uy);
    const double half = std::sqrt ((radius - distance) * (radius + distance));
    visit (along - half);
    if (half > 0)
        visit (along + half);
}

void Emit (CylinderContour& out, const SunCylinder& c, const Point& a, const Point& b)
{
    CylinderSegment s { { { float (a[0] + c.x), float (a[1] + c.y), float (a[2]) },
                          { float (b[0] + c.x), float (b[1] + c.y), float (b[2]) } } };
    if (s[0] == s[1])
        return;
    if (s[1] < s[0])
        std::swap (s[0], s[1]);
    out.segments.insert (s);
    if (out.segments.size () > kSegmentLimit) {
        out.segments.clear ();
        out.refused = true;
    }
}

void CutTriangle (const Point p[3], const SunCylinder& c, CylinderContour& out)
{
    const double dx = p[1][0] - p[0][0], dy = p[1][1] - p[0][1], dz = p[1][2] - p[0][2];
    const double ex = p[2][0] - p[0][0], ey = p[2][1] - p[0][1], ez = p[2][2] - p[0][2];
    const double nx = dy * ez - dz * ey, ny = dz * ex - dx * ez, nz = dx * ey - dy * ex;
    if (std::abs (nz) <= 1e-12 * std::hypot (nx, ny)) {
        const double norm = std::hypot (nx, ny);
        if (norm == 0)
            return;
        const double ux = -ny / norm, uy = nx / norm;
        double along[3];
        for (int i = 0; i < 3; ++i)
            along[i] = (p[i][0] - p[0][0]) * ux + (p[i][1] - p[0][1]) * uy;
        Crossings (p[0], ux, uy, c.radius, [&] (double t) {
            double lo = c.top, hi = c.floor;
            for (int i = 0; i < 3; ++i) {
                int j = (i + 1) % 3;
                if (along[i] == along[j] || t < std::min (along[i], along[j]) || t > std::max (along[i], along[j]))
                    continue;
                double z = p[i][2] + (t - along[i]) / (along[j] - along[i]) * (p[j][2] - p[i][2]);
                lo = std::min (lo, z);
                hi = std::max (hi, z);
            }
            lo = std::max (lo, c.floor);
            hi = std::min (hi, c.top);
            if (hi > lo && !out.refused)
                Emit (out, c, { p[0][0] + t * ux, p[0][1] + t * uy, lo }, { p[0][0] + t * ux, p[0][1] + t * uy, hi });
        });
        return;
    }
    // Every inside/outside transition is on an edge or a height limit. Between
    // consecutive crossings one midpoint classifies the entire circular arc.
    std::vector<double> cuts { 0, kTurn };
    auto angle = [&] (double x, double y) {
        double a = std::atan2 (y, x);
        cuts.push_back (a < 0 ? a + kTurn : a);
    };
    for (int i = 0; i < 3; ++i) {
        const auto& a = p[i];
        const auto& b = p[(i + 1) % 3];
        double length = std::hypot (b[0] - a[0], b[1] - a[1]);
        double ux = (b[0] - a[0]) / length, uy = (b[1] - a[1]) / length;
        Crossings (a, ux, uy, c.radius, [&] (double t) {
            if (t >= 0 && t <= length)
                angle (a[0] + t * ux, a[1] + t * uy);
        });
    }
    const double horizontal = std::hypot (nx, ny);
    if (horizontal > 0) {
        for (double z : { c.floor, c.top }) {
            const double d = (nx * p[0][0] + ny * p[0][1] - nz * (z - p[0][2])) / horizontal;
            Point a { nx / horizontal * d, ny / horizontal * d, z };
            double ux = -ny / horizontal, uy = nx / horizontal;
            Crossings (a, ux, uy, c.radius, [&] (double t) { angle (a[0] + t * ux, a[1] + t * uy); });
        }
    }
    auto point = [&] (double a) -> Point {
        if (a == kTurn)
            a = 0;
        double x = c.radius * std::cos (a), y = c.radius * std::sin (a);
        return { x, y, p[0][2] - (nx * (x - p[0][0]) + ny * (y - p[0][1])) / nz };
    };
    std::sort (cuts.begin (), cuts.end ());
    for (size_t i = 1; i < cuts.size () && !out.refused; ++i) {
        double start = cuts[i - 1], span = cuts[i] - start;
        if (span <= 1e-14)
            continue;
        Point mid = point (start + span / 2);
        bool inside = mid[2] >= c.floor && mid[2] <= c.top;
        for (int j = 0; j < 3; ++j) {
            const auto& a = p[j];
            const auto& b = p[(j + 1) % 3];
            inside &= ((b[0] - a[0]) * (mid[1] - a[1]) - (b[1] - a[1]) * (mid[0] - a[0])) * nz >= 0;
        }
        if (!inside)
            continue;
        const double count = std::ceil (span / std::min (0.1, 0.15 / c.radius));
        if (count > kSegmentLimit) {
            out.segments.clear ();
            out.refused = true;
            return;
        }
        Point a = point (start);
        for (size_t k = 1; k <= size_t (count) && !out.refused; ++k) {
            Point b = point (k == size_t (count) ? cuts[i] : start + span * k / count);
            Emit (out, c, a, b);
            a = b;
        }
    }
}
} // namespace

void CylinderContour::Append (const SectionMesh& mesh, const SunCylinder& c)
{
    if (refused || !(c.radius > 0) || c.floor > c.top ||
        !ReachesCircle (mesh.boundsMin[0] - c.x, mesh.boundsMax[0] - c.x, mesh.boundsMin[1] - c.y,
                        mesh.boundsMax[1] - c.y, c.radius))
        return;
    for (size_t i = 0; i + 2 < mesh.indices.size () && !refused; i += 3) {
        Point p[3];
        bool valid = true;
        for (int j = 0; j < 3; ++j) {
            size_t index = mesh.indices[i + j];
            if (index >= mesh.positions.size () / 3) {
                valid = false;
                break;
            }
            auto v = mesh.positions.data () + 3 * index;
            p[j] = { v[0] - c.x, v[1] - c.y, v[2] };
        }
        if (valid && ReachesCircle (std::min ({ p[0][0], p[1][0], p[2][0] }), std::max ({ p[0][0], p[1][0], p[2][0] }),
                                    std::min ({ p[0][1], p[1][1], p[2][1] }), std::max ({ p[0][1], p[1][1], p[2][1] }),
                                    c.radius))
            CutTriangle (p, c, *this);
    }
}
} // namespace geomsrv::archviz
