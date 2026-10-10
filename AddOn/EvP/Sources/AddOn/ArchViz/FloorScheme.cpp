#include "ArchViz/FloorSchemeDetail.hpp"
#include <algorithm>
#include <clipper2/clipper.triangulation.h>

// T0 and T7 plus the hard-rule check. Generate joins the massing floor into one outline,
// runs the skeleton, circulation and division stages, then moulds the scheme back onto
// the real outline: what the inscribed wings left out joins the flat beside it when that
// leaves no nook (a sliver along a slanted facade), otherwise it is reported unassigned.
namespace geomsrv::archviz::floorscheme {
namespace detail {
double Overlap (const Ring& a, const Ring& b)
{
    return std::abs (detail::Area (cp::Intersect ({ ToPath (a) }, { ToPath (b) }, cp::FillRule::NonZero, kPrecision)));
}
// Boundary shared by two touching polygons, from the overlap of a thin band around `a`.
double Touch (const Ring& a, const Ring& b, double band)
{
    auto grown = cp::InflatePaths ({ ToPath (a) }, band, cp::JoinType::Miter, cp::EndType::Polygon, 2.0, kPrecision);
    return std::abs (detail::Area (cp::Intersect (grown, { ToPath (b) }, cp::FillRule::NonZero, kPrecision))) / band;
}
double FacadeOf (const cp::PathsD& outline, const Ring& ring)
{
    const Ring r = Counter (ring);
    double sum = 0;
    for (size_t i = 0; i < r.size (); ++i) {
        const Vec a = r[i], b = r[(i + 1) % r.size ()];
        const Vec d = Unit ({ b.x - a.x, b.y - a.y });
        sum += FacadeAlong (outline, a, b, { d.y, -d.x }); // outward of a counter-clockwise ring
    }
    return sum;
}
void Note (Scheme& s, Diagnostic::Level level, const char* code, const std::string& text, Vec at)
{
    s.diagnostics.push_back ({ level, code, text, at });
}
Vec Centroid (const Ring& r)
{
    Vec c;
    for (const auto& p : r)
        c.x += p.x, c.y += p.y;
    if (!r.empty ())
        c.x /= static_cast<double> (r.size ()), c.y /= static_cast<double> (r.size ());
    return c;
}
// Bounding-box overlap test before the Clipper work.
bool Near (const Ring& a, const Ring& b, double gap)
{
    double ax0 = 1e18, ay0 = 1e18, ax1 = -1e18, ay1 = -1e18, bx0 = 1e18, by0 = 1e18, bx1 = -1e18, by1 = -1e18;
    for (const auto& p : a)
        ax0 = (std::min) (ax0, p.x), ay0 = (std::min) (ay0, p.y), ax1 = (std::max) (ax1, p.x),
        ay1 = (std::max) (ay1, p.y);
    for (const auto& p : b)
        bx0 = (std::min) (bx0, p.x), by0 = (std::min) (by0, p.y), bx1 = (std::max) (bx1, p.x),
        by1 = (std::max) (by1, p.y);
    return ax0 <= bx1 + gap && bx0 <= ax1 + gap && ay0 <= by1 + gap && by0 <= ay1 + gap;
}
} // namespace detail

namespace {
using namespace detail;
// Area of a polygon over the area of its bounding box in the frame of its first edge.
double Squareness (const cp::PathD& path, Vec dir)
{
    const Vec n { -dir.y, dir.x };
    double u0 = 1e18, u1 = -1e18, v0 = 1e18, v1 = -1e18;
    for (const auto& p : path) {
        const double u = p.x * dir.x + p.y * dir.y, v = p.x * n.x + p.y * n.y;
        u0 = (std::min) (u0, u), u1 = (std::max) (u1, u), v0 = (std::min) (v0, v), v1 = (std::max) (v1, v);
    }
    const double box = (u1 - u0) * (v1 - v0);
    return box > kEps ? std::abs (cp::Area (path)) / box : 0;
}
// A shape's extent in the frame of its band: u along `axis`, v across it.
struct Extent {
    Vec u, v;
    double u0 = 1e18, v0 = 1e18, u1 = -1e18, v1 = -1e18;
    Extent (const Ring& r, Vec axis) : u (Unit (axis)), v { -u.y, u.x }
    {
        for (const auto& p : r)
            Add (p);
    }
    void Add (Vec p)
    {
        const double a = Dot (p, u), b = Dot (p, v);
        u0 = (std::min) (u0, a), u1 = (std::max) (u1, a), v0 = (std::min) (v0, b), v1 = (std::max) (v1, b);
    }
    Vec World (double a, double b) const
    {
        return { a * u.x + b * v.x, a * u.y + b * v.y };
    }
    cp::PathD Slab (double a0, double b0, double a1, double b1) const
    {
        const Vec p[4] = { World (a0, b0), World (a1, b0), World (a1, b1), World (a0, b1) };
        return { { p[0].x, p[0].y }, { p[1].x, p[1].y }, { p[2].x, p[2].y }, { p[3].x, p[3].y } };
    }
    // The slab beyond side `dir` (0 +v, 1 -v, 2 +u, 3 -u) over the cross range [lo, hi].
    cp::PathD Beyond (int dir, double lo, double hi, double reach) const
    {
        switch (dir) {
            case 0:
                return Slab (lo, v1, hi, v1 + reach);
            case 1:
                return Slab (lo, v0 - reach, hi, v0);
            case 2:
                return Slab (u1, lo, u1 + reach, hi);
            default:
                return Slab (u0 - reach, lo, u0, hi);
        }
    }
    Vec Out (int dir) const
    {
        return dir == 0 ? v : dir == 1 ? Vec { -v.x, -v.y } : dir == 2 ? u : Vec { -u.x, -u.y };
    }
};
// Leftover floor beyond side `dir` of a shape, between its walls produced straight on and out
// to the outline. A piece that does not reach the outline within `reach` is left alone.
cp::PathsD Strip (const cp::PathsD& rest, const Ring& shape, const Extent& e, int dir, double reach)
{
    const bool along = dir < 2;
    const auto pieces = cp::Intersect (rest, { e.Beyond (dir, along ? e.u0 : e.v0, along ? e.u1 : e.v1, reach) },
                                       cp::FillRule::NonZero, kPrecision);
    cp::PathsD take;
    for (const auto& piece : pieces) {
        if (cp::Area (piece) < 0)
            return {}; // a hole: not a strip
        if (cp::Area (piece) < 0.01)
            continue;
        const Ring ring = FromPath (piece);
        double perimeter = 0;
        for (size_t k = 0; k < ring.size (); ++k)
            perimeter +=
                std::hypot (ring[(k + 1) % ring.size ()].x - ring[k].x, ring[(k + 1) % ring.size ()].y - ring[k].y);
        if (2 * cp::Area (piece) / perimeter < 0.05)
            continue; // a hairline left by rounding: joined it folds the flat's outline
        Extent pe (ring, e.u);
        const double depth = dir == 0 ? pe.v1 - e.v1 : dir == 1 ? e.v0 - pe.v0 : dir == 2 ? pe.u1 - e.u1 : e.u0 - pe.u0;
        if (depth < reach - 0.05 && Touch (Counter (ring), shape) >= 0.5)
            take.push_back (piece);
    }
    return take;
}
// The union of a shape and a piece beside it. Pieces of a rotated frame meet the shape on
// edges a rounding error apart, so both grow by a millimetre and the union shrinks back.
cp::PathsD Merge (const Ring& shape, const cp::PathD& piece)
{
    constexpr double kWeld = 1e-3;
    // Offsetting reads a clockwise ring as a hole; flats of a mirrored frame are clockwise.
    cp::PathD p = piece;
    if (cp::Area (p) < 0)
        std::reverse (p.begin (), p.end ());
    const auto grown = cp::InflatePaths ({ ToPath (Counter (shape)), p }, kWeld, cp::JoinType::Miter,
                                         cp::EndType::Polygon, 4.0, kPrecision);
    return cp::InflatePaths (cp::Union (grown, cp::FillRule::NonZero, kPrecision), -kWeld, cp::JoinType::Miter,
                             cp::EndType::Polygon, 4.0, kPrecision);
}
// True when an outline doubles back on itself or pinches: a spike, a fold or a bridge of zero width.
bool Folded (const Ring& ring)
{
    Ring r;
    for (const auto& p : ring)
        if (r.empty () || std::hypot (p.x - r.back ().x, p.y - r.back ().y) > 1e-4)
            r.push_back (p);
    while (r.size () > 1 && std::hypot (r.front ().x - r.back ().x, r.front ().y - r.back ().y) <= 1e-4)
        r.pop_back ();
    // A bridge or spike of no width (a piece hung on by a hairline): opening the outline by a
    // centimetre removes it, so the outline loses length but no area.
    auto perimeter = [] (const cp::PathD& p) {
        double sum = 0;
        for (size_t i = 0; i < p.size (); ++i)
            sum += std::hypot (p[(i + 1) % p.size ()].x - p[i].x, p[(i + 1) % p.size ()].y - p[i].y);
        return sum;
    };
    const auto opened = cp::InflatePaths (
        cp::InflatePaths ({ ToPath (Counter (r)) }, -0.01, cp::JoinType::Miter, cp::EndType::Polygon, 4.0, kPrecision),
        0.01, cp::JoinType::Miter, cp::EndType::Polygon, 4.0, kPrecision);
    if (opened.size () != 1 || perimeter (ToPath (r)) - perimeter (opened.front ()) > 0.2)
        return true;
    for (size_t i = 0; i < r.size (); ++i) {
        const Vec a = r[(i + r.size () - 1) % r.size ()], b = r[i], c = r[(i + 1) % r.size ()];
        const double l1 = std::hypot (b.x - a.x, b.y - a.y), l2 = std::hypot (c.x - b.x, c.y - b.y);
        if (l1 < 1e-6 || l2 < 1e-6)
            continue;
        if (((b.x - a.x) * (c.x - b.x) + (b.y - a.y) * (c.y - b.y)) / (l1 * l2) < -0.985) // sharper than 10 degrees
            return true;
    }
    return false;
}
// Joins `piece` to `shape` when the result is one ring without a hole or a fold.
bool Join (Ring& shape, const cp::PathD& piece)
{
    if (std::abs (cp::Area (piece)) < 1e-4)
        return false; // degenerate: offsetting it can read as a hole
    const auto joined = Merge (shape, piece);
    // One ring holding both, or nothing is joined.
    if (joined.size () != 1 ||
        cp::Area (joined.front ()) < std::abs (Area (shape)) + std::abs (cp::Area (piece)) - 0.01)
        return false;
    // Simplifying can fold the weld's millimetre edges away; keep it only when the area holds.
    const auto simple = cp::SimplifyPath (joined.front (), 1e-3);
    const double area = cp::Area (joined.front ());
    const Ring result = Counter (
        FromPath (std::abs (cp::Area (simple) - area) < 1e-3 * (std::max) (1.0, area) ? simple : joined.front ()));
    if (Folded (result))
        return false;
    shape = result;
    return true;
}
// The rooms on side `dir` grow over the piece between their own walls, and their windows move
// out onto the new facade.
void Grow (Flat& flat, const cp::PathD& piece, const Extent& e, int dir)
{
    const bool along = dir < 2;
    for (auto& room : flat.roomList) {
        const Extent re (room.shape, e.u);
        const double side = dir == 0 ? e.v1 - re.v1 : dir == 1 ? re.v0 - e.v0 : dir == 2 ? e.u1 - re.u1 : re.u0 - e.u0;
        if (side > 0.3)
            continue;
        const double lo = along ? re.u0 : re.v0, hi = along ? re.u1 : re.v1;
        for (const auto& part :
             cp::Intersect ({ piece }, { e.Beyond (dir, lo, hi, 50) }, cp::FillRule::NonZero, kPrecision))
            if (cp::Area (part) > 1e-3)
                Join (room.shape, part);
    }
    const Vec out = e.Out (dir);
    const cp::PathsD area { piece };
    for (auto& w : flat.windows) {
        const Extent we (Ring { w.a, w.b }, e.u);
        const double at = dir == 0 ? we.v0 - e.v1 : dir == 1 ? e.v0 - we.v1 : dir == 2 ? we.u0 - e.u1 : e.u0 - we.u1;
        if (std::abs (at) > 0.05)
            continue;
        // Each end walks out to the outline, sampled a hair inside the window.
        const Vec d = Unit ({ w.b.x - w.a.x, w.b.y - w.a.y });
        for (Vec* p : { &w.a, &w.b }) {
            const double s = p == &w.a ? 0.02 : -0.02;
            const Vec q { p->x + d.x * s, p->y + d.y * s };
            double t = 0;
            while (t < 50 && Inside (area, { q.x + out.x * (t + 0.02), q.y + out.y * (t + 0.02) }))
                t += 0.02;
            p->x += out.x * t, p->y += out.y * t;
        }
    }
}
// True when `piece` runs along a corridor of another section than `section`: joined to a flat it
// would open that flat onto a second stair's circulation.
bool Foreign (const Scheme& s, const Ring& piece, int section)
{
    for (const auto& c : s.corridors)
        if (c.section != section && Near (piece, c.shape, 0.1) && Touch (piece, c.shape) >= 0.3)
            return true;
    return false;
}
// Leftover floor beside flats and stairs joins them between their walls produced straight on to
// the outline (user, 2026-10-10), so a skewed facade adds clean strips instead of diagonal cuts:
// long sides first, then gables. A strip that would take a flat over `cap` stays out (division
// plans for the strips, so this is rare). Returns what is left.
// `wedge`: only gables, out to 8 m (a slanted end): the flat's walls at an angle leave its rooms
// to the user.
cp::PathsD Extend (cp::PathsD rest, Scheme& s, size_t first, double cap, bool wedge = false)
{
    constexpr double kSide = 3.5, kGable = 2.0, kWedge = 8.0;
    // A stair's axis is read before it grows: a joined strip can put a slanted edge first.
    std::vector<Vec> axes;
    for (const auto& core : s.cores)
        axes.push_back (core.shape.size () < 2
                            ? Vec { 1, 0 }
                            : Vec { core.shape[1].x - core.shape[0].x, core.shape[1].y - core.shape[0].y });
    for (int dir = wedge ? 2 : 0; dir < 4 && !rest.empty (); ++dir) {
        const double reach = wedge ? kWedge : dir < 2 ? kSide : kGable;
        auto apply = [&] (const cp::PathsD& take) {
            if (!take.empty ())
                rest = cp::Difference (rest, take, cp::FillRule::NonZero, kPrecision);
        };
        for (size_t k = 0; k < s.cores.size () && first == 0 && !wedge; ++k) {
            auto& core = s.cores[k];
            const Extent e (core.shape, axes[k]);
            cp::PathsD took;
            for (const auto& piece : Strip (rest, core.shape, e, dir, reach))
                if (Join (core.shape, piece))
                    took.push_back (piece);
            apply (took);
        }
        for (size_t i = first; i < s.flats.size (); ++i) {
            auto& flat = s.flats[i];
            const Extent e (flat.shape, flat.axis);
            cp::PathsD took;
            for (const auto& piece : Strip (rest, flat.shape, e, dir, reach))
                if (flat.net + cp::Area (piece) <= cap + 0.5 &&
                    !Foreign (s, Counter (FromPath (piece)), flat.section) && Join (flat.shape, piece)) {
                    if (wedge && cp::Area (piece) > 0.5)
                        flat.manual = true, flat.roomList.clear (), flat.windows.clear ();
                    else
                        Grow (flat, piece, e, dir);
                    const double area = cp::Area (piece);
                    flat.gross += area, flat.net += area;
                    took.push_back (piece);
                }
            apply (took);
        }
    }
    return rest;
}
// A small piece joins a flat it touches when it is a sliver or keeps the flat square.
bool Joinable (const Scheme& s, const Flat& flat, const cp::PathD& path, double cap)
{
    const double area = cp::Area (path);
    if (flat.net + area > cap + 0.5)
        return false; // a flat over the cap: the piece goes to a neighbour or stays out
    if (Foreign (s, Counter (FromPath (path)), flat.section))
        return false;
    const Ring piece = Counter (FromPath (path));
    double perimeter = 0;
    for (size_t k = 0; k < piece.size (); ++k)
        perimeter +=
            std::hypot (piece[(k + 1) % piece.size ()].x - piece[k].x, piece[(k + 1) % piece.size ()].y - piece[k].y);
    const double thickness = perimeter > 0 ? 2 * area / perimeter : 0;
    if (area < 1e-4)
        return false;
    const auto joined = Merge (flat.shape, path);
    if (joined.size () != 1)
        return false;
    const bool sliver = thickness <= 1.2 && area <= 0.5 * flat.gross;
    const bool small = area <= (std::max) (2.0, 0.2 * flat.gross) && Squareness (joined.front (), flat.axis) >= 0.85;
    return area < 0.5 || sliver || small;
}
// Each simple leftover region goes whole to the flat it touches most, when Joinable; else to
// the next one it touches.
cp::PathsD Absorb (const cp::PathsD& rest, Scheme& s, size_t first, double cap)
{
    cp::PathsD left;
    for (const auto& path : rest) {
        bool holed = cp::Area (path) <= 0;
        for (const auto& hole : rest)
            holed = holed ||
                    (cp::Area (hole) < 0 && !hole.empty () && Inside ({ path }, { hole.front ().x, hole.front ().y }));
        std::vector<std::pair<double, size_t>> owners;
        const Ring ring = Counter (FromPath (path));
        for (size_t i = first; i < s.flats.size () && !holed; ++i) {
            if (!Near (ring, s.flats[i].shape, 0.1))
                continue;
            const double t = Touch (ring, s.flats[i].shape);
            if (t > 0.5)
                owners.push_back ({ -t, i });
        }
        std::sort (owners.begin (), owners.end ());
        bool joined = false;
        for (const auto& [t, i] : owners)
            if (Joinable (s, s.flats[i], path, cap) && Join (s.flats[i].shape, path)) {
                s.flats[i].gross += cp::Area (path), s.flats[i].net += cp::Area (path);
                joined = true;
                break;
            }
        if (!joined)
            left.push_back (path);
    }
    return left;
}
// Area over the area of the convex hull.
double Convexity (const cp::PathD& path)
{
    cp::PathD pts = path;
    std::sort (pts.begin (), pts.end (),
               [] (const cp::PointD& a, const cp::PointD& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    if (pts.size () < 3)
        return 0;
    auto cross = [] (const cp::PointD& o, const cp::PointD& a, const cp::PointD& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    cp::PathD hull (2 * pts.size ());
    size_t k = 0;
    for (size_t i = 0; i < pts.size (); ++i) {
        while (k >= 2 && cross (hull[k - 2], hull[k - 1], pts[i]) <= 0)
            --k;
        hull[k++] = pts[i];
    }
    for (size_t i = pts.size () - 1, t = k + 1; i-- > 0;) {
        while (k >= t && cross (hull[k - 2], hull[k - 1], pts[i]) <= 0)
            --k;
        hull[k++] = pts[i];
    }
    hull.resize (k - 1);
    const double h = std::abs (cp::Area (hull));
    return h > kEps ? std::abs (cp::Area (path)) / h : 0;
}
// A leftover wedge on the outline (a slanted end, an angled junction) joins the flat it touches
// most when the flat stays under the cap and convex: walls at an angle, so its rooms are left to
// the user (user, 2026-10-10). Returns what is left.
cp::PathsD Wedges (const cp::PathsD& outline, const cp::PathsD& rest, Scheme& s, double cap)
{
    cp::PathsD left;
    for (const auto& path : rest) {
        const double area = cp::Area (path);
        const Ring ring = Counter (FromPath (path));
        bool joined = false;
        if (area >= 2.0 && FacadeOf (outline, ring) >= 1.0) {
            std::vector<std::pair<double, size_t>> owners;
            for (size_t i = 0; i < s.flats.size (); ++i)
                if (Near (ring, s.flats[i].shape, 0.1)) {
                    const double t = Touch (ring, s.flats[i].shape);
                    if (t > 1.5)
                        owners.push_back ({ -t, i });
                }
            std::sort (owners.begin (), owners.end ());
            for (const auto& [t, i] : owners) {
                auto& flat = s.flats[i];
                if (flat.net + area > cap + 0.5 || Foreign (s, ring, flat.section))
                    continue;
                const auto merged = Merge (flat.shape, path);
                if (merged.size () != 1 || Convexity (cp::SimplifyPath (merged.front (), 1e-3)) < 0.92 ||
                    !Join (flat.shape, path))
                    continue;
                flat.gross += area, flat.net += area;
                flat.manual = true;
                flat.roomList.clear ();
                flat.windows.clear ();
                joined = true;
                break;
            }
        }
        if (!joined)
            left.push_back (path);
    }
    return left;
}
// What the strips leave (corners, steps of the outline) goes whole to the flat it touches most
// when that keeps the flat square; failing that it is triangulated and each triangle goes to the
// flat it touches most, joined when it is a sliver or a small square-keeping piece.
void Mould (const cp::PathsD& outline, Scheme& s, double cap)
{
    cp::PathsD used;
    for (const auto& f : s.flats)
        used.push_back (ToPath (f.shape));
    for (const auto& c : s.corridors)
        used.push_back (ToPath (c.shape));
    for (const auto& c : s.cores)
        used.push_back (ToPath (c.shape));
    for (const auto& l : s.lobbies)
        used.push_back (ToPath (l));
    for (const auto& u : s.unassigned)
        used.push_back (ToPath (u.shape));
    auto rest = Extend (cp::Difference (outline, cp::Union (used, cp::FillRule::NonZero, kPrecision),
                                        cp::FillRule::NonZero, kPrecision),
                        s, 0, cap);
    rest = Wedges (outline, Extend (Absorb (rest, s, 0, cap), s, 0, cap, true), s, cap);
    if (std::abs (detail::Area (rest)) < 0.05)
        return;
    // Simple pieces: each region with its holes triangulated, or cut on a 2 m grid when the
    // triangulation refuses (touching or near-degenerate rings of real outlines).
    cp::PathsD triangles;
    for (const auto& outer : rest) {
        if (cp::Area (outer) <= 0)
            continue;
        cp::PathsD region { outer };
        for (const auto& hole : rest)
            if (cp::Area (hole) < 0 && !hole.empty () && Inside ({ outer }, { hole.front ().x, hole.front ().y }))
                region.push_back (hole);
        cp::PathsD part;
        if (cp::Triangulate (region, kPrecision, part, false) == cp::TriangulateResult::success) {
            // Either winding comes back; the shares below keep only counter-clockwise paths.
            double sum = 0;
            for (auto& t : part) {
                if (cp::Area (t) < 0)
                    std::reverse (t.begin (), t.end ());
                sum += cp::Area (t);
            }
            // On touching rings of real outlines it can also drop or repeat triangles.
            if (std::abs (sum - detail::Area (region)) < 0.01) {
                triangles.insert (triangles.end (), part.begin (), part.end ());
                continue;
            }
        }
        const auto box = cp::GetBounds (region);
        constexpr double cell = 2.0;
        for (double x = box.left; x < box.right; x += cell)
            for (double y = (std::min) (box.top, box.bottom); y < (std::max) (box.top, box.bottom); y += cell) {
                const cp::PathD square { { x, y }, { x + cell, y }, { x + cell, y + cell }, { x, y + cell } };
                for (auto& piece : cp::Intersect (region, { square }, cp::FillRule::NonZero, kPrecision))
                    if (cp::Area (piece) > 1e-4)
                        triangles.push_back (std::move (piece));
            }
    }
    std::vector<cp::PathsD> share (s.flats.size () + 1); // last: nobody's
    auto distance = [] (Vec p, const Ring& ring) {
        double best = 1e18;
        for (size_t i = 0, j = ring.size () - 1; i < ring.size (); j = i++) {
            const Vec a = ring[j], b = ring[i];
            const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
            const double t = l2 > 0 ? std::clamp (((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0, 1.0) : 0;
            best = (std::min) (best, std::hypot (a.x + t * dx - p.x, a.y + t * dy - p.y));
        }
        return best;
    };
    for (const auto& t : triangles) {
        if (std::abs (cp::Area (t)) < 1e-6)
            continue;
        const Ring tri = Counter (FromPath (t));
        const Vec mid = Centroid (tri);
        double best = 1.5;
        size_t owner = s.flats.size ();
        for (size_t i = 0; i < s.flats.size (); ++i) {
            if (!Near (tri, s.flats[i].shape, 1.5))
                continue;
            const double d = distance (mid, s.flats[i].shape);
            if (d < best && Touch (tri, s.flats[i].shape) > 0.02)
                best = d, owner = i;
        }
        share[owner].push_back (t);
    }
    auto leave = [&] (const cp::PathD& path, const char* reason) {
        if (std::abs (cp::Area (path)) > 0.05)
            s.unassigned.push_back ({ Counter (FromPath (path)), reason });
    };
    // A share that closes into a ring (it has a hole) is reported as its triangles.
    auto merged = [&] (const cp::PathsD& parts) {
        const auto joined = cp::Union (parts, cp::FillRule::NonZero, kPrecision);
        for (const auto& path : joined)
            if (cp::Area (path) < 0)
                return parts;
        return joined;
    };
    for (size_t i = 0; i < s.flats.size (); ++i) {
        if (share[i].empty ())
            continue;
        auto& flat = s.flats[i];
        for (const auto& path : merged (share[i])) {
            const double area = cp::Area (path);
            if (area <= 0)
                continue;
            // Only a small piece next to the flat: anything larger stays visible.
            if (Joinable (s, flat, path, cap) && Join (flat.shape, path))
                flat.gross += area, flat.net += area;
            else
                leave (path, "would make a nook in the flat beside it");
        }
    }
    for (const auto& path : merged (share.back ()))
        if (cp::Area (path) > 0)
            leave (path, "outside the scheme");
}
// Unassigned floor of 20 m2 or more that meets a corridor and has a facade opposite it is
// divided again (user, 2026-10-10): the largest rectangle along that corridor becomes a band.
void Rework (const cp::PathsD& outline, Scheme& s, const floorprogramme::Programme& programme, const Pins& pins,
             const Options& o)
{
    // Touching leftovers are one zone: a band read as dark beside the strip that reaches the
    // real facade of a stepped outline only divides once they are joined.
    cp::PathsD pieces;
    for (const auto& u : s.unassigned)
        pieces.push_back (ToPath (u.shape));
    const auto grown = cp::InflatePaths (pieces, 0.05, cp::JoinType::Miter, cp::EndType::Polygon, 2.0, kPrecision);
    const auto zones = cp::InflatePaths (cp::Union (grown, cp::FillRule::NonZero, kPrecision), -0.05,
                                         cp::JoinType::Miter, cp::EndType::Polygon, 2.0, kPrecision);
    std::vector<Slot> slots;
    cp::PathsD reworked;
    for (const auto& zonePath : zones) {
        if (cp::Area (zonePath) < 20.0)
            continue;
        const Ring zone = Counter (FromPath (zonePath));
        int best = -1;
        double touch = 1.5;
        for (size_t k = 0; k < s.corridors.size (); ++k) {
            const double t = Touch (zone, s.corridors[k].shape);
            if (t > touch)
                touch = t, best = static_cast<int> (k);
        }
        if (best < 0)
            continue;
        const auto& corridor = s.corridors[best];
        // The corridor leg nearest the zone gives the direction of the band.
        const Vec mid = Centroid (zone);
        Vec dir { 1, 0 };
        double near = 1e18;
        for (size_t k = 0; k + 1 < corridor.axis.size (); ++k) {
            const Vec a = corridor.axis[k], b = corridor.axis[k + 1];
            const double d = std::hypot ((a.x + b.x) / 2 - mid.x, (a.y + b.y) / 2 - mid.y);
            if (d < near)
                near = d, dir = Unit ({ b.x - a.x, b.y - a.y });
        }
        Frame f;
        Box box;
        if (!Inscribed ({ zonePath }, std::atan2 (dir.y, dir.x), kLivingMin + 0.1, RoomFrontage (1).min, f, box))
            continue;
        // v away from the corridor, so the band's facade is v1 and its door v0.
        double cv0 = 1e18, cv1 = -1e18, cu0 = 1e18, cu1 = -1e18;
        for (const auto& p : corridor.shape) {
            const auto q = f.Local (p);
            cv0 = (std::min) (cv0, q.y), cv1 = (std::max) (cv1, q.y), cu0 = (std::min) (cu0, q.x),
            cu1 = (std::max) (cu1, q.x);
        }
        if (cv0 > box.v1 - 0.01) {
            f.v = { -f.v.x, -f.v.y };
            box = { box.u0, -box.v1, box.u1, -box.v0 };
            std::swap (cv0, cv1), cv0 = -cv0, cv1 = -cv1;
        }
        if (std::abs (cv1 - box.v0) > 0.35)
            continue; // the rectangle does not reach the corridor
        box.v0 = cv1; // closes a grid-sized gap so the halls meet the corridor
        // Every flat opens off the corridor: the band reaches at most one flat past its ends.
        const double past = RoomFrontage (3).pref - 1.2;
        box.u0 = (std::max) (box.u0, cu0 - past), box.u1 = (std::min) (box.u1, cu1 + past);
        if (box.W () < RoomFrontage (1).min)
            continue;
        if (FacadeAlong (outline, f.World (box.u0, box.v1), f.World (box.u1, box.v1), f.v) < 0.5 * box.W ())
            continue;
        Slot slot;
        slot.frame = f, slot.box = box;
        slot.section = corridor.section;
        slot.doorLo = cu0, slot.doorHi = cu1;
        slot.cornerLo = FacadeAlong (outline, f.World (box.u0, box.v0), f.World (box.u0, box.v1), { -f.u.x, -f.u.y }) >=
                        0.5 * box.H ();
        slot.cornerHi =
            FacadeAlong (outline, f.World (box.u1, box.v0), f.World (box.u1, box.v1), f.u) >= 0.5 * box.H ();
        slots.push_back (slot);
        reworked.push_back (zonePath);
    }
    if (slots.empty ())
        return;
    const size_t before = s.flats.size ();
    Divide (programme, slots, s, pins, o, true);
    cp::PathsD made;
    for (size_t k = before; k < s.flats.size (); ++k)
        made.push_back (ToPath (s.flats[k].shape));
    // A reworked zone keeps only what the new flats leave of it, after they take the strips
    // beside them out to the outline: the zone and its flats end in clean rectangles.
    std::vector<Piece> kept;
    cp::PathsD zone;
    for (const auto& u : s.unassigned) {
        const cp::PathD path = ToPath (u.shape);
        bool inZone = false;
        for (const auto& z : reworked)
            inZone =
                inZone || std::abs (detail::Area (cp::Intersect ({ path }, { z }, cp::FillRule::NonZero, kPrecision))) >
                              0.5 * std::abs (cp::Area (path));
        if (inZone)
            zone.push_back (path);
        else
            kept.push_back (u);
    }
    zone =
        cp::Difference (cp::Union (zone, cp::FillRule::NonZero, kPrecision), made, cp::FillRule::NonZero, kPrecision);
    for (const auto& rest :
         Absorb (Extend (zone, s, before, MaxFlat (programme, o)), s, before, MaxFlat (programme, o)))
        if (cp::Area (rest) > 0.5)
            kept.push_back ({ Counter (FromPath (rest)), "left beside the flats divided again" });
    s.unassigned = std::move (kept);
}
// Touching unassigned pieces with the same reason are shown as one, unless they close a ring.
void Tidy (Scheme& s)
{
    std::vector<Piece> tidy;
    std::vector<bool> done (s.unassigned.size (), false);
    for (size_t i = 0; i < s.unassigned.size (); ++i) {
        if (done[i])
            continue;
        cp::PathsD group;
        for (size_t k = i; k < s.unassigned.size (); ++k)
            if (!done[k] && s.unassigned[k].reason == s.unassigned[i].reason)
                group.push_back (ToPath (s.unassigned[k].shape)), done[k] = true;
        const auto joined = cp::Union (group, cp::FillRule::NonZero, kPrecision);
        const bool ring = std::any_of (joined.begin (), joined.end (), [] (const auto& p) { return cp::Area (p) < 0; });
        for (const auto& path : ring ? group : joined)
            if (cp::Area (path) > 0)
                tidy.push_back ({ Counter (FromPath (cp::SimplifyPath (path, 1e-3))), s.unassigned[i].reason });
    }
    s.unassigned = std::move (tidy);
}
} // namespace

namespace {
// Net area less what the stairs cost and the floor left unassigned; a broken scheme loses.
double Economy (const Scheme& s, const Options& o)
{
    double red = 0;
    for (const auto& u : s.unassigned)
        if (u.reason.find ("storage") == std::string::npos)
            red += std::abs (Area (u.shape));
    return s.net - o.stairCost * static_cast<double> (s.cores.size ()) - red - (s.ok ? 0.0 : 1e6);
}
Scheme Plan (const std::vector<Ring>& outline, const floorprogramme::Programme& programme, const Pins& pins,
             const Options& options);
} // namespace

Scheme Generate (const std::vector<Ring>& outline, const floorprogramme::Programme& programme, const Pins& pins,
                 const Options& options)
{
    auto s = Plan (outline, programme, pins, options);
    if (options.shallow != Access::Auto)
        return s;
    // Typology adaptation (user, 2026-10-10): shallow wings planned as sections are weighed
    // against a corridor on one side, the economic plan of a thin bar.
    bool sections = false;
    for (const auto& w : s.wings)
        sections = sections || w.access == Access::CoreOnly;
    if (!sections)
        return s;
    Options corridor = options;
    corridor.shallow = Access::OneSide;
    auto alt = Plan (outline, programme, pins, corridor);
    const double a = Economy (s, options), b = Economy (alt, options);
    auto& kept = b > a ? alt : s;
    kept.diagnostics.push_back ({ Diagnostic::Info,
                                  "typology.choice",
                                  std::string (b > a ? "A corridor on one side" : "Sections round stairs") +
                                      " scored " + std::to_string (static_cast<int> (std::round ((std::max) (a, b)))) +
                                      " against " + std::to_string (static_cast<int> (std::round ((std::min) (a, b)))) +
                                      " (net m2 less stairs and unassigned).",
                                  {} });
    return std::move (kept);
}

namespace {
Scheme Plan (const std::vector<Ring>& outline, const floorprogramme::Programme& programme, const Pins& pins,
             const Options& options)
{
    Scheme s;
    auto paths = cp::Union (ToPaths (outline), cp::FillRule::NonZero, kPrecision);
    {
        cp::PathsD kept;
        for (auto& p : paths)
            if (std::abs (cp::Area (p)) > 0.5)
                kept.push_back (cp::SimplifyPath (p, 1e-3));
        paths = kept;
    }
    s.outline = FromPaths (paths);
    s.gross = detail::Area (paths);
    if (paths.empty () || !floorprogramme::Valid (programme)) {
        Note (s, Diagnostic::Error, "input.empty", paths.empty () ? "No floor outline." : "The programme is not valid.",
              {});
        return s;
    }
    auto skeleton = Decompose (paths, options, s.diagnostics);
    s.typology = skeleton.typology;
    auto layout = Circulate (paths, skeleton, programme, pins, options, s.diagnostics);
    if (!layout.culled.empty ()) {
        // What the typology leaves out is no longer floor: the rest is planned and checked.
        cp::PathsD cut;
        for (const auto& piece : layout.culled)
            cut.push_back (ToPath (piece.shape));
        paths = cp::Difference (paths, cut, cp::FillRule::NonZero, kPrecision);
        s.outline = FromPaths (paths);
        s.gross = detail::Area (paths);
        s.culled = layout.culled;
    }
    s.wings = skeleton.wings;
    s.sections = layout.sections;
    s.corridors = layout.corridors;
    s.lobbies = layout.lobbies;
    s.cores = layout.cores;
    s.unassigned = layout.storage;
    for (const auto& slot : layout.slots) {
        Band band;
        band.shape = ToRing (slot.frame, slot.box);
        band.a = slot.frame.World (slot.box.u0, (slot.box.v0 + slot.box.v1) / 2);
        band.b = slot.frame.World (slot.box.u1, (slot.box.v0 + slot.box.v1) / 2);
        band.depth = slot.box.H ();
        band.section = slot.section;
        s.bands.push_back (std::move (band));
    }
    Divide (programme, layout.slots, s, pins, options);
    for (const auto& f : s.flats)
        if (f.band >= 0 && f.band < static_cast<int> (s.bands.size ()))
            ++s.bands[f.band].flats;
    Mould (paths, s, MaxFlat (programme, options));
    Rework (paths, s, programme, pins, options);
    Tidy (s);
    // A flat whose rooms are left to the user takes the type its area fits.
    for (auto& f : s.flats) {
        if (!f.manual)
            continue;
        size_t pick = f.type;
        double miss = 1e18;
        for (size_t t = 0; t < programme.types.size (); ++t) {
            const auto& type = programme.types[t];
            const double m = f.net < type.minM2 ? type.minM2 - f.net : f.net > type.maxM2 ? f.net - type.maxM2 : 0;
            if (m < miss)
                miss = m, pick = t;
        }
        if (pick != f.type && pick < s.counts.size () && f.type < s.counts.size ())
            --s.counts[f.type], ++s.counts[pick];
        f.type = pick, f.rooms = programme.types[pick].rooms;
        f.inRange = miss <= 0.5;
    }
    for (const auto& f : s.flats)
        s.net += f.net;
    for (const auto& c : s.corridors)
        s.circulation += Area (c.shape);
    for (const auto& c : s.cores)
        s.circulation += Area (c.shape);
    for (const auto& l : s.lobbies)
        s.circulation += Area (l);
    Check (s, options);
    return s;
}

} // namespace

} // namespace geomsrv::archviz::floorscheme
