#include "Edit/EditPathContours.hpp"
#include "Edit/EditPathNetwork.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <string>
#include <unordered_map>

namespace geomsrv::edit {

namespace {

namespace cp = Clipper2Lib;

constexpr int kPrecision = 3; // millimetres, as the paved area's

// A path's surface as sampled: its outline, and its plan bounds for a quick miss.
struct Strip {
    const Path* path = nullptr;
    AlignmentGeometry geometry;
    PathOutline outline;
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};

std::vector<Strip> Strips (const std::vector<Path>& paths, double step)
{
    std::vector<Strip> out;
    for (const auto& path : paths) {
        Strip s;
        s.path = &path;
        std::string error;
        if (!ValidatePath (path, s.geometry, error) || !Outline (path, step, s.outline, error) ||
            s.outline.stations.size () < 2)
            continue;
        s.x0 = s.y0 = std::numeric_limits<double>::infinity ();
        s.x1 = s.y1 = -std::numeric_limits<double>::infinity ();
        for (const auto* side : { &s.outline.left, &s.outline.right })
            for (const Vec3& p : *side) {
                s.x0 = std::min (s.x0, p.x), s.x1 = std::max (s.x1, p.x);
                s.y0 = std::min (s.y0, p.y), s.y1 = std::max (s.y1, p.y);
            }
        out.push_back (std::move (s));
    }
    return out;
}

// Sample `k`'s three points across, numbered `3k` (left edge), `3k + 1` (centre), `3k + 2`.
Vec3 Corner (const PathOutline& o, uint64_t id)
{
    const size_t k = static_cast<size_t> (id / 3);
    return id % 3 == 0 ? o.left[k] : id % 3 == 1 ? o.centre[k] : o.right[k];
}

// Whether the ground at `p` is another path's: one it lies deeper in than in its own (the
// first in the list, on a tie), as the paved surface takes its heights.
bool Others (const std::vector<Strip>& strips, size_t own, Vec3 p)
{
    bool known = false;
    double z = 0.0, mine = 0.0;
    for (size_t j = 0; j < strips.size (); ++j) {
        const Strip& s = strips[j];
        if (j == own || p.x < s.x0 || p.x > s.x1 || p.y < s.y0 || p.y > s.y1)
            continue;
        if (!known && !SurfaceHeight (*strips[own].path, strips[own].geometry, p, z, mine))
            return false;
        known = true;
        double beyond = 0.0;
        if (SurfaceHeight (*s.path, s.geometry, p, z, beyond) &&
            (beyond < mine - 1.0e-9 || (std::abs (beyond - mine) <= 1.0e-9 && j < own)))
            return true;
    }
    return false;
}

// Where a cell's edge crosses the level: the edge names the point, so the cells either side
// of it meet there.
struct Cut {
    uint64_t key = 0;
    Vec3 p;
};

// Pieces of one level joined where they share a cut, into lines.
void Join (const std::vector<std::pair<Cut, Cut>>& pieces, Id path, double level, std::vector<Contour>& out)
{
    std::unordered_map<uint64_t, std::vector<size_t>> at;
    for (size_t i = 0; i < pieces.size (); ++i) {
        at[pieces[i].first.key].push_back (i);
        at[pieces[i].second.key].push_back (i);
    }
    std::vector<bool> used (pieces.size (), false);
    const auto next = [&] (uint64_t key, Cut& end) {
        for (const size_t j : at[key])
            if (!used[j]) {
                used[j] = true;
                end = pieces[j].first.key == key ? pieces[j].second : pieces[j].first;
                return true;
            }
        return false;
    };
    for (size_t i = 0; i < pieces.size (); ++i) {
        if (used[i])
            continue;
        used[i] = true;
        std::deque<Vec3> line { pieces[i].first.p, pieces[i].second.p };
        Cut end = pieces[i].second, start = pieces[i].first;
        while (next (end.key, end))
            line.push_back (end.p);
        while (next (start.key, start))
            line.push_front (start.p);
        Contour c;
        c.path = path;
        c.z = level;
        for (const Vec3& p : line)
            if (c.points.empty () || DistanceXY (c.points.back (), p) > 1.0e-9)
                c.points.push_back (p);
        if (c.points.size () >= 2)
            out.push_back (std::move (c));
    }
}

void Trace (const std::vector<Strip>& strips, size_t own, double level, std::vector<Contour>& out)
{
    const PathOutline& o = strips[own].outline;
    const uint64_t n = 3 * o.stations.size ();
    const auto cut = [&] (uint64_t i, uint64_t j) {
        const Vec3 a = Corner (o, i), b = Corner (o, j);
        Vec3 p = Lerp (a, b, (level - a.z) / (b.z - a.z));
        p.z = level;
        return Cut { std::min (i, j) * n + std::max (i, j), p };
    };
    std::vector<std::pair<Cut, Cut>> pieces;
    const auto triangle = [&] (uint64_t a, uint64_t b, uint64_t c) {
        const uint64_t t[3] = { a, b, c };
        bool up[3];
        int count = 0;
        for (int i = 0; i < 3; ++i)
            count += (up[i] = Corner (o, t[i]).z >= level) ? 1 : 0;
        if (count == 0 || count == 3)
            return;
        // The corner alone on its side of the level: the line cuts its two edges.
        int m = 0;
        while (up[m] == (count == 2))
            ++m;
        const Cut from = cut (t[m], t[(m + 1) % 3]), to = cut (t[m], t[(m + 2) % 3]);
        if (!Others (strips, own, Lerp (from.p, to.p, 0.5)))
            pieces.push_back ({ from, to });
    };
    for (uint64_t k = 0; k + 1 < o.stations.size (); ++k)
        for (const uint64_t side : { 0u, 2u }) { // the strip left of the centreline, then right
            const uint64_t a = 3 * k + side, b = 3 * k + 1, c = 3 * (k + 1) + 1, d = 3 * (k + 1) + side;
            triangle (a, b, c);
            triangle (a, c, d);
        }
    Join (pieces, strips[own].path->id, level, out);
}

// The surface's height where no path's strip is -- a junction's rounding: the path the point
// lies deepest in, or least outside, as the paved surface takes it (Pave).
bool Height (const std::vector<Strip>& strips, Vec3 p, double& z)
{
    double best = std::numeric_limits<double>::infinity ();
    for (const auto& s : strips) {
        double h = 0.0, beyond = 0.0;
        if (SurfaceHeight (*s.path, s.geometry, p, h, beyond) && beyond < best)
            best = beyond, z = h;
    }
    return std::isfinite (best);
}

// Ground no path's strip covers, contoured on a grid of `cell` metres (marching squares,
// a saddle split as its centre falls) and kept to that ground.
void TraceBetween (const std::vector<Strip>& strips, const cp::PathD& ground, const std::vector<double>& levels,
                   double cell, std::vector<Contour>& out)
{
    const cp::RectD box = cp::GetBounds (ground);
    const size_t nx = static_cast<size_t> (std::ceil (box.Width () / cell)) + 2;
    const size_t ny = static_cast<size_t> (std::ceil (box.Height () / cell)) + 2;
    if (nx * ny > 250000)
        return; // a rounding this large on this grid would be too slow to draw live
    const double x0 = box.left - 0.5 * cell, y0 = box.top - 0.5 * cell;
    std::vector<double> z (nx * ny, 0.0);
    std::vector<bool> known (nx * ny, false);
    for (size_t j = 0; j < ny; ++j)
        for (size_t i = 0; i < nx; ++i)
            known[j * nx + i] = Height (strips, { x0 + double (i) * cell, y0 + double (j) * cell, 0.0 }, z[j * nx + i]);
    std::vector<Contour> lines;
    for (const double level : levels) {
        std::vector<std::pair<Cut, Cut>> pieces;
        for (size_t j = 0; j + 1 < ny; ++j)
            for (size_t i = 0; i + 1 < nx; ++i) {
                // Corners counter-clockwise from the lower left; edge k runs from corner k on.
                const size_t c[4] = { j * nx + i, j * nx + i + 1, (j + 1) * nx + i + 1, (j + 1) * nx + i };
                if (!known[c[0]] || !known[c[1]] || !known[c[2]] || !known[c[3]])
                    continue;
                bool up[4];
                int count = 0;
                for (int k = 0; k < 4; ++k)
                    count += (up[k] = z[c[k]] >= level) ? 1 : 0;
                if (count == 0 || count == 4)
                    continue;
                const auto cut = [&] (int k) {
                    const size_t a = c[k], b = c[(k + 1) % 4];
                    const double t = (level - z[a]) / (z[b] - z[a]);
                    const Vec3 pa { x0 + double (a % nx) * cell, y0 + double (a / nx) * cell, level };
                    const Vec3 pb { x0 + double (b % nx) * cell, y0 + double (b / nx) * cell, level };
                    return Cut { 2 * std::min (a, b) + (b - a == 1 || a - b == 1 ? 0u : 1u), Lerp (pa, pb, t) };
                };
                int crossed[4], n = 0;
                for (int k = 0; k < 4; ++k)
                    if (up[k] != up[(k + 1) % 4])
                        crossed[n++] = k;
                if (n == 2) {
                    pieces.push_back ({ cut (crossed[0]), cut (crossed[1]) });
                    continue;
                }
                // A saddle: each corner on the other side from the cell's middle is cut off,
                // across its two edges.
                const bool middle = (z[c[0]] + z[c[1]] + z[c[2]] + z[c[3]]) * 0.25 >= level;
                for (int k = 0; k < 4; ++k)
                    if (up[k] != middle)
                        pieces.push_back ({ cut ((k + 3) % 4), cut (k) });
            }
        Join (pieces, kNoId, level, lines);
    }
    // Kept to the ground between the strips.
    cp::PathsD open;
    for (const auto& l : lines) {
        cp::PathD q;
        for (const Vec3& p : l.points)
            q.push_back ({ p.x, p.y });
        open.push_back (q);
    }
    std::vector<double> heights;
    for (const auto& l : lines)
        heights.push_back (l.z);
    for (size_t k = 0; k < lines.size (); ++k) {
        cp::ClipperD clipper (kPrecision);
        clipper.AddOpenSubject ({ open[k] });
        clipper.AddClip ({ ground });
        cp::PathsD closed, kept;
        clipper.Execute (cp::ClipType::Intersection, cp::FillRule::NonZero, closed, kept);
        for (const auto& q : kept) {
            Contour c;
            c.z = heights[k];
            for (const auto& p : q)
                c.points.push_back ({ p.x, p.y, c.z });
            if (c.points.size () >= 2)
                out.push_back (std::move (c));
        }
    }
}

void HeightSpan (const std::vector<Strip>& strips, double& lo, double& hi)
{
    lo = std::numeric_limits<double>::infinity ();
    hi = -lo;
    for (const auto& s : strips)
        for (const auto* side : { &s.outline.left, &s.outline.centre, &s.outline.right })
            for (const Vec3& p : *side)
                lo = std::min (lo, p.z), hi = std::max (hi, p.z);
}

} // namespace

std::vector<Contour> PathContours (const std::vector<Path>& paths, double interval, double step, size_t maxLevels)
{
    std::vector<Contour> out;
    if (!(interval > 0.0))
        return out;
    const auto strips = Strips (paths, step);
    if (strips.empty ())
        return out;
    double lo = 0.0, hi = 0.0;
    HeightSpan (strips, lo, hi);
    const auto levels = Levels (lo, hi, interval);
    if (levels.size () > maxLevels)
        return out;
    std::vector<Contour> traced;
    for (size_t i = 0; i < strips.size (); ++i)
        for (const double level : levels)
            Trace (strips, i, level, traced);
    // The paved area as rounded: the strips' lines are kept to it (an outside corner's
    // rounding cuts them), and the ground it adds between them -- a junction's roundings --
    // is contoured on its own.
    const PavedArea paved = Pave (paths, step, false);
    cp::PathsD area, strip;
    for (const auto& ring : paved.rings) {
        cp::PathD q;
        for (const Vec3& p : ring)
            q.push_back ({ p.x, p.y });
        area.push_back (q);
    }
    for (const auto& s : strips) {
        cp::PathD q;
        for (const Vec3& p : s.outline.left)
            q.push_back ({ p.x, p.y });
        for (size_t i = s.outline.right.size (); i-- > 0;)
            q.push_back ({ s.outline.right[i].x, s.outline.right[i].y });
        strip.push_back (q);
    }
    strip = cp::Union (strip, cp::FillRule::NonZero, kPrecision);
    for (const auto& c : traced) {
        cp::ClipperD clipper (kPrecision);
        cp::PathD q;
        for (const Vec3& p : c.points)
            q.push_back ({ p.x, p.y });
        clipper.AddOpenSubject ({ q });
        clipper.AddClip (area);
        cp::PathsD closed, kept;
        clipper.Execute (cp::ClipType::Intersection, cp::FillRule::NonZero, closed, kept);
        for (const auto& k : kept) {
            Contour piece;
            piece.path = c.path;
            piece.z = c.z;
            for (const auto& p : k)
                piece.points.push_back ({ p.x, p.y, c.z });
            if (piece.points.size () >= 2)
                out.push_back (std::move (piece));
        }
    }
    const double cell = std::min (step, 0.25);
    for (const auto& ground : cp::Difference (area, strip, cp::FillRule::NonZero, kPrecision))
        if (cp::Area (ground) > 0.01) // not the millimetre slivers where outlines meet
            TraceBetween (strips, ground, levels, cell, out);
    return out;
}

bool PathHeightRange (const std::vector<Path>& paths, double& lo, double& hi, double step)
{
    const auto strips = Strips (paths, step);
    HeightSpan (strips, lo, hi);
    return !strips.empty ();
}

double NiceStep (double range, int divisions)
{
    const double raw = range / std::max (1, divisions);
    if (!(raw > 0.0) || !std::isfinite (raw))
        return 0.0;
    const double magnitude = std::pow (10.0, std::floor (std::log10 (raw)));
    const double f = raw / magnitude;
    const double nice = f < 1.5 ? 1.0 : f < 2.25 ? 2.0 : f < 3.5 ? 2.5 : f < 7.5 ? 5.0 : 10.0;
    return nice * magnitude;
}

std::vector<double> Levels (double lo, double hi, double step)
{
    std::vector<double> out;
    if (!(step > 0.0) || !(hi >= lo))
        return out;
    const double first = std::ceil (lo / step - 1.0e-9), last = std::floor (hi / step + 1.0e-9);
    for (double m = first; m <= last && out.size () < 100000; m += 1.0)
        out.push_back (m * step);
    return out;
}

int StepDecimals (double step)
{
    for (int d = 0; d < 4; ++d) {
        const double v = step * std::pow (10.0, d);
        if (std::abs (v - std::round (v)) < 1.0e-6)
            return d;
    }
    return 4;
}

} // namespace geomsrv::edit
