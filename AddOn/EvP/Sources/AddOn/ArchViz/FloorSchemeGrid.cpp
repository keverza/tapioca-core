#include "ArchViz/FloorSchemeDetail.hpp"
#include <algorithm>
#include <cmath>

// The building's main walls (user, 2026-10-15: "generator should create grid lines of main walls
// and try to align unit partition walls. A new walls may be anywhere but main wall should go from
// bottom to top similar to stairs core"): lines across each wing where the floor every floor shares
// has a wall across it -- a party wall between flats or a stair's side -- held on every floor
// (Options::grid), where the flats' party walls snap to them.
namespace geomsrv::archviz::floorscheme {
namespace {
// Where a ring's points fall along `dir` from `a`: its extent.
std::pair<double, double> Along (const Ring& ring, Vec a, Vec dir)
{
    double lo = 1e18, hi = -1e18;
    for (const auto& p : ring) {
        const double t = detail::Dot ({ p.x - a.x, p.y - a.y }, dir);
        lo = (std::min) (lo, t), hi = (std::max) (hi, t);
    }
    return { lo, hi };
}
bool InWing (const Wing& w, Vec p)
{
    return detail::Inside (detail::ToPaths ({ w.rect }), p);
}
} // namespace

std::vector<std::array<Vec, 2>> MainWalls (const Scheme& s)
{
    std::vector<std::array<Vec, 2>> out;
    for (const auto& w : s.wings) {
        if (w.length < 1e-6)
            continue;
        const Vec dir = detail::Unit ({ w.b.x - w.a.x, w.b.y - w.a.y }), n { -dir.y, dir.x };
        // Each wall across the wing: where it stands along it, and whether a stair's side (fixed).
        std::vector<std::pair<double, bool>> at;
        const auto across = [&] (double t, bool stair) {
            if (t > kGridEnd && t < w.length - kGridEnd) // a wing's ends are its gables
                at.push_back ({ t, stair });
        };
        for (const auto& f : s.flats) {
            if (f.band < 0 || std::abs (detail::Dot (f.axis, dir)) < 0.99 || !InWing (w, detail::Centroid (f.shape)))
                continue;
            const auto [lo, hi] = Along (f.shape, w.a, dir);
            across (lo, false), across (hi, false);
        }
        for (const auto& c : s.cores) {
            if (!InWing (w, c.centre))
                continue;
            const auto [lo, hi] = Along (c.shape, w.a, dir);
            across (lo, true), across (hi, true);
        }
        std::sort (at.begin (), at.end ());
        // Walls closer than kGridMerge are one main wall -- the two rows across the corridor share
        // it: at a stair's side when one is among them, else at their middle.
        for (size_t i = 0; i < at.size ();) {
            size_t j = i;
            double sum = 0, stair = -1;
            while (j < at.size () && at[j].first - at[i].first <= kGridMerge) {
                sum += at[j].first;
                if (at[j].second)
                    stair = at[j].first;
                ++j;
            }
            const double t = stair >= 0 ? stair : sum / static_cast<double> (j - i);
            const Vec p { w.a.x + dir.x * t, w.a.y + dir.y * t };
            out.push_back ({ Vec { p.x - n.x * w.depth / 2, p.y - n.y * w.depth / 2 },
                             Vec { p.x + n.x * w.depth / 2, p.y + n.y * w.depth / 2 } });
            i = j;
        }
    }
    return out;
}

namespace detail {
std::vector<double> GridAcross (const std::vector<std::array<Vec, 2>>& grid, const Slot& slot)
{
    std::vector<double> out;
    for (const auto& g : grid) {
        const Vec d = Unit ({ g[1].x - g[0].x, g[1].y - g[0].y });
        if (std::abs (Dot (d, slot.frame.u)) > 0.05) // not across the band
            continue;
        const Vec q0 = slot.frame.Local (g[0]), q1 = slot.frame.Local (g[1]);
        const double v0 = (std::min) (q0.y, q1.y), v1 = (std::max) (q0.y, q1.y);
        const double over = (std::min) (v1, slot.box.v1) - (std::max) (v0, slot.box.v0);
        if (over < 0.5 * slot.box.H ()) // another wing's, or beside this band
            continue;
        const double u = (q0.x + q1.x) / 2;
        if (u > slot.box.u0 + 1e-6 && u < slot.box.u1 - 1e-6)
            out.push_back (u);
    }
    std::sort (out.begin (), out.end ());
    return out;
}
int SnapToGrid (const std::vector<double>& lines, double u0, std::vector<double>& widths,
                const std::vector<std::pair<double, double>>& ranges)
{
    int on = 0;
    double at = u0;
    for (size_t k = 0; k + 1 < widths.size (); ++k) {
        at += widths[k];
        double best = 0;
        bool found = false;
        for (const double line : lines) {
            const double d = line - at;
            if (std::abs (d) > kGridSnap || (found && std::abs (d) >= std::abs (best)))
                continue;
            // Both flats either side keep a width their type allows.
            const double a = widths[k] + d, b = widths[k + 1] - d;
            if (a < ranges[k].first - 1e-6 || a > ranges[k].second + 1e-6 || b < ranges[k + 1].first - 1e-6 ||
                b > ranges[k + 1].second + 1e-6)
                continue;
            best = d, found = true;
        }
        if (!found)
            continue;
        widths[k] += best, widths[k + 1] -= best, at += best;
        ++on;
    }
    return on;
}
} // namespace detail
} // namespace geomsrv::archviz::floorscheme
