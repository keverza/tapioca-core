#include "ArchViz/FloorSchemeDetail.hpp"
#include <numbers>
#include <sstream>

// T1: the floor reduced to wings. A wing is the largest rectangle that still fits the
// floor in one of its principal directions (any angle); the search repeats on what is
// left. Wings that grow out of another one take its side as their depth, so a short arm
// of an L is still an arm. Junction kinds name the typology.
namespace geomsrv::archviz::floorscheme {
double Area (const Ring& ring)
{
    double a = 0;
    for (size_t i = 0, j = ring.size () - 1; i < ring.size (); j = i++)
        a += ring[j].x * ring[i].y - ring[i].x * ring[j].y;
    return a / 2;
}
} // namespace geomsrv::archviz::floorscheme

namespace geomsrv::archviz::floorscheme::detail {
bool Inside (const cp::PathsD& paths, Vec p)
{
    int winding = 0;
    for (const auto& path : paths) {
        if (path.size () < 3)
            continue;
        bool inside = false;
        for (size_t i = 0, j = path.size () - 1; i < path.size (); j = i++) {
            const auto& a = path[j];
            const auto& b = path[i];
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double cross = dx * (p.y - a.y) - dy * (p.x - a.x);
            const double dot = (p.x - a.x) * dx + (p.y - a.y) * dy;
            if (std::abs (cross) <= 1e-9 * (std::max) (1.0, std::hypot (dx, dy)) && dot >= -1e-12 &&
                dot <= dx * dx + dy * dy + 1e-12)
                return true;
            if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + dx * (p.y - a.y) / dy)
                inside = !inside;
        }
        if (inside)
            winding += cp::Area (path) > 0 ? 1 : -1;
    }
    return winding != 0;
}
double Area (const cp::PathsD& paths)
{
    double a = 0;
    for (const auto& path : paths)
        a += cp::Area (path);
    return a;
}
double FacadeAlong (const cp::PathsD& outline, Vec a, Vec b, Vec outward, const cp::PathsD* blind)
{
    const double length = std::hypot (b.x - a.x, b.y - a.y);
    if (length < kEps)
        return 0;
    const int samples = (std::max) (2, static_cast<int> (std::ceil (length / 0.3)));
    int out = 0;
    for (int i = 0; i < samples; ++i) {
        const double t = (i + 0.5) / samples;
        const Vec p { a.x + (b.x - a.x) * t + outward.x * kFacadeOffset,
                      a.y + (b.y - a.y) * t + outward.y * kFacadeOffset };
        out += Inside (outline, p) || (blind && Inside (*blind, p)) ? 0 : 1;
    }
    return length * out / samples;
}

namespace {
struct Candidate {
    double score = 0;
    double theta = 0;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0; // in the rotated frame
};
// Principal directions modulo 90 degrees, weighted by edge length.
std::vector<double> Directions (const cp::PathsD& paths)
{
    struct Cluster {
        double weight = 0, sin = 0, cos = 0;
    };
    std::vector<Cluster> clusters;
    const double quarter = std::numbers::pi / 2;
    for (const auto& path : paths)
        for (size_t i = 0, j = path.size () - 1; i < path.size (); j = i++) {
            const double dx = path[i].x - path[j].x, dy = path[i].y - path[j].y;
            const double length = std::hypot (dx, dy);
            if (length < 0.5)
                continue;
            double a = std::fmod (std::atan2 (dy, dx), quarter);
            if (a < 0)
                a += quarter;
            // Average on the 4x circle so 0 and 90 degrees are the same direction.
            const double s = std::sin (4 * a), c = std::cos (4 * a);
            bool joined = false;
            for (auto& k : clusters) {
                const double ka = std::atan2 (k.sin, k.cos);
                if (std::abs (std::remainder (4 * a - ka, 2 * std::numbers::pi)) < 4 * 2.0 * std::numbers::pi / 180) {
                    k.weight += length, k.sin += s * length, k.cos += c * length;
                    joined = true;
                    break;
                }
            }
            if (!joined)
                clusters.push_back ({ length, s * length, c * length });
        }
    std::sort (clusters.begin (), clusters.end (),
               [] (const Cluster& a, const Cluster& b) { return a.weight > b.weight; });
    std::vector<double> out;
    for (const auto& k : clusters) {
        if (out.size () == 4 || k.weight < 0.1 * clusters.front ().weight)
            break;
        double a = std::atan2 (k.sin, k.cos) / 4;
        if (a < 0)
            a += quarter;
        out.push_back (a);
    }
    if (out.empty ())
        out.push_back (0);
    return out;
}
std::vector<double> Lines (std::vector<double> values, double step)
{
    std::sort (values.begin (), values.end ());
    std::vector<double> unique;
    for (double v : values)
        if (unique.empty () || v - unique.back () > 1e-4)
            unique.push_back (v);
    std::vector<double> out;
    for (size_t i = 0; i + 1 < unique.size (); ++i) {
        out.push_back (unique[i]);
        const double gap = unique[i + 1] - unique[i];
        const int parts = static_cast<int> (std::ceil (gap / step - 1e-9));
        for (int k = 1; k < parts; ++k)
            out.push_back (unique[i] + gap * k / parts);
    }
    if (!unique.empty ())
        out.push_back (unique.back ());
    return out;
}
// Inside intervals of a horizontal line through the rotated paths (even-odd).
std::vector<std::pair<double, double>> Intervals (const cp::PathsD& paths, double y)
{
    std::vector<double> xs;
    for (const auto& path : paths)
        for (size_t i = 0, j = path.size () - 1; i < path.size (); j = i++) {
            const auto& a = path[j];
            const auto& b = path[i];
            if ((a.y > y) != (b.y > y))
                xs.push_back (a.x + (b.x - a.x) * (y - a.y) / (b.y - a.y));
        }
    std::sort (xs.begin (), xs.end ());
    std::vector<std::pair<double, double>> out;
    for (size_t i = 0; i + 1 < xs.size (); i += 2)
        out.emplace_back (xs[i], xs[i + 1]);
    return out;
}
bool Covered (const std::vector<std::pair<double, double>>& intervals, double x0, double x1)
{
    for (const auto& [a, b] : intervals)
        if (a <= x0 + 1e-6 && x1 <= b + 1e-6)
            return true;
    return false;
}
void Consider (Candidate& best, double theta, double x0, double y0, double x1, double y1, const Options& o)
{
    double w = x1 - x0, h = y1 - y0;
    if ((std::min) (w, h) > o.maxWingDepth) {
        if (h <= w)
            y1 = y0 + o.maxWingDepth, h = o.maxWingDepth;
        else
            x1 = x0 + o.maxWingDepth, w = o.maxWingDepth;
    }
    const double depth = (std::min) (w, h), length = (std::max) (w, h);
    if (depth < o.minWingDepth - 1e-6 || length < o.minWingLength - 1e-6)
        return;
    const double score = w * h;
    if (score > best.score + 1e-6)
        best = { score, theta, x0, y0, x1, y1 };
}
Candidate Largest (const cp::PathsD& world, double theta, const Options& o)
{
    const Vec d { std::cos (theta), std::sin (theta) }, n { -d.y, d.x };
    cp::PathsD paths;
    std::vector<double> xv, yv;
    for (const auto& path : world) {
        cp::PathD p;
        for (const auto& q : path) {
            const double x = q.x * d.x + q.y * d.y, y = q.x * n.x + q.y * n.y;
            p.emplace_back (x, y);
            xv.push_back (x);
            yv.push_back (y);
        }
        paths.push_back (std::move (p));
    }
    Candidate best;
    if (xv.empty ())
        return best;
    double step = o.raster;
    auto xs = Lines (xv, step), ys = Lines (yv, step);
    while (xs.size () * ys.size () > 400000 && step < 5) {
        step *= 1.5;
        xs = Lines (xv, step), ys = Lines (yv, step);
    }
    if (xs.size () < 2 || ys.size () < 2)
        return best;
    const size_t cols = xs.size () - 1;
    std::vector<int> height (cols, 0);
    for (size_t j = 0; j + 1 < ys.size (); ++j) {
        const double y0 = ys[j], y1 = ys[j + 1];
        const auto lo = Intervals (paths, y0 + 1e-7), mid = Intervals (paths, (y0 + y1) / 2),
                   hi = Intervals (paths, y1 - 1e-7);
        for (size_t i = 0; i < cols; ++i) {
            const bool in =
                Covered (mid, xs[i], xs[i + 1]) && Covered (lo, xs[i], xs[i + 1]) && Covered (hi, xs[i], xs[i + 1]);
            height[i] = in ? height[i] + 1 : 0;
        }
        // Largest rectangles of every bar height ending on this row.
        std::vector<size_t> stack;
        for (size_t i = 0; i <= cols; ++i) {
            const int h = i < cols ? height[i] : 0;
            while (!stack.empty () && height[stack.back ()] >= h) {
                const size_t top = stack.back ();
                stack.pop_back ();
                const int bar = height[top];
                if (bar == 0)
                    continue;
                const size_t left = stack.empty () ? 0 : stack.back () + 1;
                Consider (best, theta, xs[left], ys[j + 1 - bar], xs[i], y1, o);
            }
            stack.push_back (i);
        }
    }
    return best;
}
Ring CandidateRing (const Candidate& c)
{
    const Vec d { std::cos (c.theta), std::sin (c.theta) }, n { -d.y, d.x };
    auto at = [&] (double x, double y) { return Vec { x * d.x + y * n.x, x * d.y + y * n.y }; };
    return Counter ({ at (c.x0, c.y0), at (c.x1, c.y0), at (c.x1, c.y1), at (c.x0, c.y1) });
}
struct Side {
    Vec a, b;
};
// The four sides of a wing in its frame: 0 at v = 0, 1 at u = length, 2 at v = depth, 3 at u = 0.
Side SideOf (const Frame& f, const Wing& w, int side)
{
    switch (side) {
        case 0:
            return { f.World (0, 0), f.World (w.length, 0) };
        case 1:
            return { f.World (w.length, 0), f.World (w.length, w.depth) };
        case 2:
            return { f.World (w.length, w.depth), f.World (0, w.depth) };
        default:
            return { f.World (0, w.depth), f.World (0, 0) };
    }
}
// Length of `s` lying on side `t` (collinear within 5 cm).
double Shared (Side s, Side t)
{
    const Vec d = Unit ({ t.b.x - t.a.x, t.b.y - t.a.y });
    const double lt = Dot ({ t.b.x - t.a.x, t.b.y - t.a.y }, d);
    auto off = [&] (Vec p) { return std::abs ((p.x - t.a.x) * -d.y + (p.y - t.a.y) * d.x); };
    if (off (s.a) > 0.05 || off (s.b) > 0.05)
        return 0;
    double p0 = Dot ({ s.a.x - t.a.x, s.a.y - t.a.y }, d), p1 = Dot ({ s.b.x - t.a.x, s.b.y - t.a.y }, d);
    if (p0 > p1)
        std::swap (p0, p1);
    return (std::max) (0.0, (std::min) (p1, lt) - (std::max) (p0, 0.0));
}
Wing MakeWing (const Candidate& c, bool alongX, Frame& frame)
{
    const Vec d { std::cos (c.theta), std::sin (c.theta) }, n { -d.y, d.x };
    auto at = [&] (double x, double y) { return Vec { x * d.x + y * n.x, x * d.y + y * n.y }; };
    Wing w;
    w.rect = CandidateRing (c);
    frame.o = at (c.x0, c.y0);
    if (alongX) {
        frame.u = d, frame.v = n;
        w.length = c.x1 - c.x0, w.depth = c.y1 - c.y0;
    }
    else {
        frame.u = n, frame.v = d;
        w.length = c.y1 - c.y0, w.depth = c.x1 - c.x0;
    }
    w.a = frame.World (0, w.depth / 2), w.b = frame.World (w.length, w.depth / 2);
    return w;
}
// Re-express a wing so its axis runs perpendicular to `side` (it grows out of a neighbour there).
void Turn (Wing& w, Frame& f)
{
    const Frame old = f;
    f.o = old.World (0, 0);
    f.u = old.v, f.v = old.u;
    std::swap (w.length, w.depth);
    w.a = f.World (0, w.depth / 2), w.b = f.World (w.length, w.depth / 2);
}
std::string Name (const Skeleton& s, const std::vector<int>& members, bool courtyard)
{
    std::vector<int> l, t, step, angled;
    for (int i : members) {
        const auto& w = s.wings[i];
        if (w.parent < 0)
            continue;
        if (std::abs (w.angle - 90) > 10 && w.joint != 'S')
            angled.push_back (i);
        (w.joint == 'L' ? l : w.joint == 'T' ? t : step).push_back (i);
    }
    std::ostringstream name;
    if (courtyard)
        name << "O";
    else if (members.size () == 1)
        name << "I";
    else if (t.size () >= 2) {
        // Two arms at the same place on opposite sides make a cross.
        bool cross = false;
        for (size_t i = 0; i < t.size () && !cross; ++i)
            for (size_t j = i + 1; j < t.size () && !cross; ++j) {
                const auto& a = s.wings[t[i]];
                const auto& b = s.wings[t[j]];
                if (a.parent != b.parent)
                    continue;
                const auto& f = s.frames[a.parent];
                const auto pa = f.Local (a.b), pb = f.Local (b.b);
                const double mid = s.wings[a.parent].depth / 2;
                cross = (pa.y - mid) * (pb.y - mid) < 0 &&
                        std::abs (f.Local (a.a).x - f.Local (b.a).x) < (a.depth + b.depth) / 4;
            }
        name << (cross ? (angled.empty () ? "+" : "X") : "comb");
    }
    else if (t.size () == 1)
        name << (l.empty () ? "T" : "F");
    else if (l.size () == 1)
        name << (angled.empty () ? "L" : "V");
    else if (l.size () == 2) {
        const auto& a = s.wings[l[0]];
        const auto& b = s.wings[l[1]];
        // Both arms on the same side of their common neighbour: U (short arms: bracket).
        const int p = a.parent == b.parent ? a.parent : -1;
        if (p >= 0) {
            const auto& f = s.frames[p];
            const double sa = f.Local (a.b).y - s.wings[p].depth / 2, sb = f.Local (b.b).y - s.wings[p].depth / 2;
            const bool same = (sa > 0) == (sb > 0);
            const bool shortArms = (std::min) (a.length, b.length) < s.wings[p].depth;
            name << (same ? (shortArms ? "[" : "U") : "Z");
        }
        else
            name << "U";
    }
    else if (members.size () > 1 && step.size () + 1 == members.size ())
        name << "I stepped";
    else
        name << "compound";
    for (int i : angled)
        name << " " << static_cast<int> (std::round (s.wings[i].angle)) << "\xC2\xB0";
    return name.str ();
}
// A bar whose facades jog (a sheared or notched outline) is read as one wing as deep as its
// narrowest part. The free depth beyond each long side is sampled along the wing; where it
// jumps the wing is cut, and each part takes the depth its own facades allow (user, 2026-10-10:
// sections are planned on the real depth).
void Steps (Skeleton& s, const cp::PathsD& outline, const Options& o)
{
    constexpr double kSample = 0.5, kJump = 0.8, kGain = 1.0;
    const double minPiece = (std::max) (o.minWingLength, 8.0);
    auto clear = [&] (Vec p, size_t self) {
        if (!Inside (outline, p))
            return false;
        for (size_t j = 0; j < s.wings.size (); ++j)
            if (j != self && Inside ({ ToPath (s.wings[j].rect) }, p))
                return false;
        return true;
    };
    const size_t count = s.wings.size ();
    for (size_t i = 0; i < count; ++i) {
        const Wing w = s.wings[i];
        const Frame f = s.frames[i];
        const double reach = (std::min) (3.0, o.maxWingDepth - w.depth);
        if (reach < kGain || w.length < 2 * minPiece)
            continue;
        // Free depth beyond side v = depth (+) and v = 0 (-) at u.
        auto free = [&] (double u, int sign) {
            double e = 0;
            while (e + 0.1 <= reach && clear (f.World (u, sign > 0 ? w.depth + e + 0.1 : -e - 0.1), i))
                e += 0.1;
            return e;
        };
        const int n = static_cast<int> (std::floor (w.length / kSample));
        if (n < 4)
            continue;
        std::vector<double> plus (n), minus (n);
        for (int k = 0; k < n; ++k) {
            const double u = (k + 0.5) * w.length / n;
            plus[k] = free (u, 1), minus[k] = free (u, -1);
        }
        // Cuts where either side jumps, refined to 5 cm.
        std::vector<double> cuts { 0 };
        for (int k = 1; k < n; ++k) {
            if (std::abs (plus[k] - plus[k - 1]) < kJump && std::abs (minus[k] - minus[k - 1]) < kJump)
                continue;
            const int sign = std::abs (plus[k] - plus[k - 1]) >= kJump ? 1 : -1;
            const double before = sign > 0 ? plus[k - 1] : minus[k - 1];
            double lo = (k - 0.5) * w.length / n, hi = (k + 0.5) * w.length / n;
            while (hi - lo > 0.05) {
                const double mid = (lo + hi) / 2;
                (std::abs (free (mid, sign) - before) < kJump / 2 ? lo : hi) = mid;
            }
            cuts.push_back ((lo + hi) / 2);
        }
        cuts.push_back (w.length);
        // Parts shorter than a flat or two join their neighbour.
        for (bool merged = true; merged && cuts.size () > 2;) {
            merged = false;
            for (size_t k = 0; k + 1 < cuts.size (); ++k)
                if (cuts[k + 1] - cuts[k] < minPiece) {
                    cuts.erase (cuts.begin () + static_cast<long> (k == 0 ? 1 : k));
                    merged = true;
                    break;
                }
        }
        if (cuts.size () < 3)
            continue;
        // Each part's extra depth: the least free depth inside it, then checked as a rectangle.
        struct Part {
            double u0, u1, lo, hi;
        };
        std::vector<Part> parts;
        bool gain = false;
        for (size_t k = 0; k + 1 < cuts.size (); ++k) {
            Part p { cuts[k], cuts[k + 1], reach, reach };
            for (int j = 0; j < n; ++j) {
                const double u = (j + 0.5) * w.length / n;
                if (u > p.u0 + 0.3 && u < p.u1 - 0.3)
                    p.hi = (std::min) (p.hi, plus[j]), p.lo = (std::min) (p.lo, minus[j]);
            }
            p.hi = std::floor (p.hi * 10) / 10, p.lo = std::floor (p.lo * 10) / 10;
            gain = gain || p.hi + p.lo >= kGain;
            parts.push_back (p);
        }
        if (!gain)
            continue;
        auto ring = [&] (const Part& p) {
            return Counter ({ f.World (p.u0, -p.lo), f.World (p.u1, -p.lo), f.World (p.u1, w.depth + p.hi),
                              f.World (p.u0, w.depth + p.hi) });
        };
        // The grown wing already leans a hair past a skewed outline: a few hundredths of a m2.
        auto fits = [&] (const Part& p) {
            const auto r = ring (p);
            if (Area (cp::Difference ({ ToPath (r) }, outline, cp::FillRule::NonZero, kPrecision)) > 0.05)
                return false;
            for (size_t j = 0; j < s.wings.size (); ++j)
                if (j != i && Area (cp::Intersect ({ ToPath (r) }, { ToPath (s.wings[j].rect) }, cp::FillRule::NonZero,
                                                   kPrecision)) > 0.05)
                    return false;
            return true;
        };
        // Each side shrinks on its own until the part fits.
        for (auto& p : parts) {
            for (double* side : { &p.hi, &p.lo }) {
                Part one = p;
                (side == &p.hi ? one.lo : one.hi) = 0;
                double& e = side == &p.hi ? one.hi : one.lo;
                while (e > 0 && !fits (one))
                    e = (std::max) (0.0, e - 0.1);
                *side = e;
            }
            while ((p.hi > 0 || p.lo > 0) && !fits (p))
                p.hi = (std::max) (0.0, p.hi - 0.1), p.lo = (std::max) (0.0, p.lo - 0.1);
        }
        for (size_t k = 0; k < parts.size (); ++k) {
            const auto& p = parts[k];
            Frame pf = f;
            pf.o = f.World (p.u0, -p.lo);
            Wing piece = w;
            piece.length = p.u1 - p.u0, piece.depth = w.depth + p.lo + p.hi;
            piece.rect = ring (p);
            piece.a = pf.World (0, piece.depth / 2), piece.b = pf.World (piece.length, piece.depth / 2);
            if (k == 0)
                s.wings[i] = piece, s.frames[i] = pf;
            else
                s.wings.push_back (piece), s.frames.push_back (pf);
        }
    }
}
} // namespace

bool Inscribed (const cp::PathsD& area, double theta, double minDepth, double minLength, Frame& frame, Box& box)
{
    Options o;
    o.minWingDepth = minDepth, o.minWingLength = minLength, o.maxWingDepth = 1e9;
    const auto c = Largest (area, theta, o);
    if (c.score <= 0)
        return false;
    frame.o = { 0, 0 };
    frame.u = { std::cos (theta), std::sin (theta) };
    frame.v = { -frame.u.y, frame.u.x };
    box = { c.x0, c.y0, c.x1, c.y1 };
    return true;
}
Skeleton Decompose (const cp::PathsD& outline, const Options& o, std::vector<Diagnostic>& notes)
{
    Skeleton s;
    const auto directions = Directions (outline);
    cp::PathsD remain = outline;
    while (s.wings.size () < 32) {
        Candidate best;
        for (double theta : directions) {
            const auto c = Largest (remain, theta, o);
            if (c.score > best.score + 1e-6)
                best = c;
        }
        if (best.score <= 0)
            break;
        Frame frame;
        auto wing = MakeWing (best, best.x1 - best.x0 >= best.y1 - best.y0, frame);
        s.wings.push_back (wing);
        s.frames.push_back (frame);
        remain = cp::Difference (remain, { ToPath (wing.rect) }, cp::FillRule::NonZero, kPrecision);
        cp::PathsD kept;
        for (auto& path : remain)
            if (std::abs (cp::Area (path)) > 0.25)
                kept.push_back (std::move (path));
        remain = cp::Union (kept, cp::FillRule::NonZero, kPrecision);
    }
    // A rectangle found on the search grid can stop short of a slanted facade, and an angled
    // arm stops short of its neighbour's rectangle: push every side of every wing outward while
    // it still lies inside the floor and clear of the other wings (sides by up to 3 m).
    for (size_t i = 0; i < s.wings.size (); ++i)
        for (int side = 0; side < 4; ++side) {
            auto& w = s.wings[i];
            auto& f = s.frames[i];
            auto grown = [&] (double e) {
                double u0 = 0, u1 = w.length, v0 = 0, v1 = w.depth;
                (side == 0 ? u0 : side == 1 ? u1 : side == 2 ? v0 : v1) += side % 2 ? e : -e;
                return Counter ({ f.World (u0, v0), f.World (u1, v0), f.World (u1, v1), f.World (u0, v1) });
            };
            auto fits = [&] (double e) {
                const auto r = grown (e);
                if (Area (cp::Difference ({ ToPath (r) }, outline, cp::FillRule::NonZero, kPrecision)) > 0.01)
                    return false;
                for (size_t j = 0; j < s.wings.size (); ++j)
                    if (j != i && Area (cp::Intersect ({ ToPath (r) }, { ToPath (s.wings[j].rect) },
                                                       cp::FillRule::NonZero, kPrecision)) > 0.01)
                        return false;
                return true;
            };
            double lo = 0, hi = side < 2 ? 20.0 : (std::min) (3.0, o.maxWingDepth - w.depth);
            if (hi < 0.05 || !fits (0.02))
                continue;
            for (int k = 0; k < 24; ++k) {
                const double mid = (lo + hi) / 2;
                (fits (mid) ? lo : hi) = mid;
            }
            if (lo < 0.02)
                continue;
            if (side == 0)
                f.o = f.World (-lo, 0);
            if (side == 2)
                f.o = f.World (0, -lo);
            (side < 2 ? w.length : w.depth) += lo;
            w.rect =
                Counter ({ f.World (0, 0), f.World (w.length, 0), f.World (w.length, w.depth), f.World (0, w.depth) });
            w.a = f.World (0, w.depth / 2), w.b = f.World (w.length, w.depth / 2);
        }
    Steps (s, outline, o);
    if (!s.wings.empty ()) {
        cp::PathsD all;
        for (const auto& w : s.wings)
            all.push_back (ToPath (w.rect));
        remain = cp::Difference (outline, cp::Union (all, cp::FillRule::NonZero, kPrecision), cp::FillRule::NonZero,
                                 kPrecision);
    }
    // Junctions: an arm grows out of an earlier wing through one of its own sides.
    for (size_t i = 1; i < s.wings.size (); ++i) {
        auto& w = s.wings[i];
        double bestShare = 0;
        int bestParent = -1, bestSide = -1, bestParentSide = -1;
        for (size_t j = 0; j < i; ++j)
            for (int a = 0; a < 4; ++a)
                for (int b = 0; b < 4; ++b) {
                    const double share = Shared (SideOf (s.frames[i], w, a), SideOf (s.frames[j], s.wings[j], b));
                    if (share > bestShare + 1e-6)
                        bestShare = share, bestParent = static_cast<int> (j), bestSide = a, bestParentSide = b;
                }
        if (bestParent < 0 || bestShare < 1.0) {
            // An angled arm only comes near its neighbour: the gap is left to the mould.
            const Vec ends[2] = { w.a, w.b };
            for (size_t j = 0; j < i && bestParent < 0; ++j) {
                cp::PathsD other { ToPath (s.wings[j].rect) };
                other = cp::InflatePaths (other, (std::max) (1.0, 0.6 * w.depth), cp::JoinType::Miter,
                                          cp::EndType::Polygon, 2.0, kPrecision);
                for (int e = 0; e < 2 && bestParent < 0; ++e)
                    if (Inside (other, ends[e])) {
                        bestParent = static_cast<int> (j);
                        if (e == 1) {
                            auto& f = s.frames[i];
                            f.o = f.World (w.length, 0), f.u = { -f.u.x, -f.u.y };
                            w.a = f.World (0, w.depth / 2), w.b = f.World (w.length, w.depth / 2);
                        }
                        const auto& p = s.wings[j];
                        const double u = s.frames[j].Local (w.a).x;
                        w.joint = u < 0.6 * p.depth || u > p.length - 0.6 * p.depth ? 'L' : 'T';
                    }
            }
            if (bestParent < 0)
                continue;
        }
        else {
            const auto side = SideOf (s.frames[i], w, bestSide);
            const double sideLength = std::hypot (side.b.x - side.a.x, side.b.y - side.a.y);
            const bool along = bestSide == 0 || bestSide == 2; // a long side of this wing
            if (along && bestShare >= 0.8 * sideLength && sideLength <= o.maxWingDepth + 1e-6) {
                Turn (w, s.frames[i]);
                bestSide = bestSide == 0 ? 3 : 1;
            }
            else if (along) {
                w.parent = bestParent, w.joint = 'P'; // side by side: a deep block cut in two bars
                continue;
            }
            // Point the axis away from the parent: u = 0 at the joint.
            auto& f = s.frames[i];
            if (bestSide == 1) {
                f.o = f.World (w.length, w.depth), f.u = { -f.u.x, -f.u.y }, f.v = { -f.v.x, -f.v.y };
                w.a = f.World (0, w.depth / 2), w.b = f.World (w.length, w.depth / 2);
            }
            const auto& p = s.wings[bestParent];
            const auto& pf = s.frames[bestParent];
            const bool parentLong = bestParentSide == 0 || bestParentSide == 2;
            if (!parentLong)
                w.joint = 'S';
            else {
                // Flush with the parent's end: a corner; otherwise a middle junction.
                double lo = 1e18, hi = -1e18;
                for (const auto& q : w.rect) {
                    const double u = pf.Local (q).x;
                    lo = (std::min) (lo, u), hi = (std::max) (hi, u);
                }
                w.joint = lo < 0.6 || hi > p.length - 0.6 ? 'L' : 'T';
            }
        }
        w.parent = bestParent;
        const auto& pf = s.frames[bestParent];
        const double c = std::abs (Dot (s.frames[i].u, pf.u));
        w.angle = std::acos ((std::min) (1.0, c)) * 180 / std::numbers::pi;
    }
    // Components (separate buildings of the massing floor) are named one by one.
    // Wings touching each other (not only through `parent`) belong to one building.
    std::vector<int> link (s.wings.size ());
    for (size_t i = 0; i < link.size (); ++i)
        link[i] = static_cast<int> (i);
    auto find = [&] (int i) {
        while (link[i] != i)
            i = link[i] = link[link[i]];
        return i;
    };
    for (size_t i = 0; i < s.wings.size (); ++i) {
        if (s.wings[i].parent >= 0)
            link[find (static_cast<int> (i))] = find (s.wings[i].parent);
        cp::PathsD grown { ToPath (s.wings[i].rect) };
        grown = cp::InflatePaths (grown, 0.1, cp::JoinType::Miter, cp::EndType::Polygon, 2.0, kPrecision);
        for (size_t j = 0; j < i; ++j)
            if (Area (cp::Intersect (grown, { ToPath (s.wings[j].rect) }, cp::FillRule::NonZero, kPrecision)) > 0.1)
                link[find (static_cast<int> (i))] = find (static_cast<int> (j));
    }
    std::vector<int> group (s.wings.size (), -1);
    std::vector<int> ids (s.wings.size (), -1);
    int groups = 0;
    for (size_t i = 0; i < s.wings.size (); ++i) {
        const int root = find (static_cast<int> (i));
        if (ids[root] < 0)
            ids[root] = groups++;
        group[i] = ids[root];
    }
    std::ostringstream typology;
    for (int g = 0; g < groups; ++g) {
        std::vector<int> members;
        for (size_t i = 0; i < s.wings.size (); ++i)
            if (group[i] == g)
                members.push_back (static_cast<int> (i));
        // A courtyard: a hole of the outline inside this component's wings.
        cp::PathsD own;
        for (int i : members)
            own.push_back (ToPath (s.wings[i].rect));
        own = cp::Union (own, cp::FillRule::NonZero, kPrecision);
        bool courtyard = false;
        for (const auto& path : own)
            courtyard = courtyard || cp::Area (path) < 0;
        if (!courtyard)
            for (const auto& path : outline)
                if (cp::Area (path) < 0 && !path.empty () && Inside (own, { path.front ().x, path.front ().y }))
                    courtyard = true;
        typology << (g ? " | " : "") << Name (s, members, courtyard);
    }
    s.typology = typology.str ();
    s.rest = FromPaths (remain);
    if (s.wings.empty ())
        notes.push_back ({ Diagnostic::Error, "skeleton.empty",
                           "No wing fits: every part of this floor is shallower than the minimum flat depth." });
    return s;
}
} // namespace geomsrv::archviz::floorscheme::detail
