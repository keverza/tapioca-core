#include "Edit/EditTerrainBakeDivide.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>

namespace geomsrv::edit {
namespace {

// One line as stations along its traced points: `at` ascends from 0 to the length, a ring's last
// station being its length again (the point it closes on).
struct Line {
    const std::vector<Vec3>* traced = nullptr;
    bool closed = false;
    std::vector<double> along; // station of each traced point (and, for a ring, of its return)
    std::vector<double> at;
    bool shortened = false;

    Vec3 Point (double d) const
    {
        const auto& t = *traced;
        const size_t n = t.size ();
        if (d <= 0.0)
            return t.front ();
        if (d >= along.back ())
            return closed ? t.front () : t.back ();
        const size_t k = size_t (std::upper_bound (along.begin (), along.end (), d) - along.begin ()) - 1;
        const double span = along[k + 1] - along[k];
        const double f = span > 0.0 ? std::clamp ((d - along[k]) / span, 0.0, 1.0) : 0.0;
        return t[k] + (t[(k + 1) % n] - t[k]) * f;
    }
};

struct Segment {
    int line = -1; // -1: the outline
    size_t piece = 0;
    Vec3 a, b;
};

double Orient (Vec3 a, Vec3 b, Vec3 c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool Same (Vec3 a, Vec3 b)
{
    return std::abs (a.x - b.x) <= 1.0e-9 && std::abs (a.y - b.y) <= 1.0e-9;
}

// A crossing in plan: each segment strictly on both sides of the other. Touching -- a shared end,
// a point on the other line -- is not one: the triangulation takes it as a shared vertex.
bool Cross (const Segment& s, const Segment& t)
{
    if (Same (s.a, t.a) || Same (s.a, t.b) || Same (s.b, t.a) || Same (s.b, t.b))
        return false;
    const double o1 = Orient (s.a, s.b, t.a), o2 = Orient (s.a, s.b, t.b);
    const double o3 = Orient (t.a, t.b, s.a), o4 = Orient (t.a, t.b, s.b);
    return ((o1 > 0.0 && o2 < 0.0) || (o1 < 0.0 && o2 > 0.0)) && ((o3 > 0.0 && o4 < 0.0) || (o3 < 0.0 && o4 > 0.0));
}

// Neighbouring pieces of one line share a point and cannot cross.
bool Adjacent (const Segment& s, const Segment& t, const std::vector<Line>& lines)
{
    if (s.line != t.line || s.line < 0)
        return false;
    const size_t pieces = lines[size_t (s.line)].at.size () - 1;
    const size_t d = s.piece > t.piece ? s.piece - t.piece : t.piece - s.piece;
    return d <= 1 || (lines[size_t (s.line)].closed && d == pieces - 1);
}

// The pieces that cross something, by line; `where` is one crossing's plan point.
std::map<int, std::set<size_t>> Crossings (const std::vector<Line>& lines, const std::vector<Vec3>& outline,
                                           double cell, Vec3& where)
{
    std::vector<Segment> segments;
    for (size_t i = 0; i < outline.size (); ++i)
        segments.push_back ({ -1, i, outline[i], outline[(i + 1) % outline.size ()] });
    for (size_t l = 0; l < lines.size (); ++l)
        for (size_t j = 0; j + 1 < lines[l].at.size (); ++j)
            segments.push_back ({ int (l), j, lines[l].Point (lines[l].at[j]), lines[l].Point (lines[l].at[j + 1]) });
    // A plan grid: each segment in the cells its box covers.
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    const auto key = [] (int64_t x, int64_t y) { return (uint64_t (uint32_t (x)) << 32) | uint32_t (y); };
    for (uint32_t i = 0; i < segments.size (); ++i) {
        const auto& s = segments[i];
        const int64_t x0 = int64_t (std::floor (std::min (s.a.x, s.b.x) / cell));
        const int64_t x1 = int64_t (std::floor (std::max (s.a.x, s.b.x) / cell));
        const int64_t y0 = int64_t (std::floor (std::min (s.a.y, s.b.y) / cell));
        const int64_t y1 = int64_t (std::floor (std::max (s.a.y, s.b.y) / cell));
        for (int64_t x = x0; x <= x1; ++x)
            for (int64_t y = y0; y <= y1; ++y)
                grid[key (x, y)].push_back (i);
    }
    std::map<int, std::set<size_t>> crossing;
    std::set<std::pair<uint32_t, uint32_t>> tested;
    for (const auto& [cellKey, list] : grid)
        for (size_t p = 0; p < list.size (); ++p)
            for (size_t q = p + 1; q < list.size (); ++q) {
                const auto& s = segments[list[p]];
                const auto& t = segments[list[q]];
                if ((s.line < 0 && t.line < 0) || Adjacent (s, t, lines) ||
                    !tested.emplace (std::min (list[p], list[q]), std::max (list[p], list[q])).second || !Cross (s, t))
                    continue;
                for (const auto* u : { &s, &t })
                    if (u->line >= 0)
                        crossing[u->line].insert (u->piece);
                where = (s.a + s.b) * 0.5;
            }
    return crossing;
}

} // namespace

DividedContours DivideContours (const std::vector<std::vector<Vec3>>& traced, const std::vector<char>& closed,
                                const std::vector<Vec3>& outline, double step, double tolerance)
{
    DividedContours out;
    if (!(step > 0.0) || !std::isfinite (step) || traced.size () != closed.size ()) {
        out.error = "Divide by length needs a positive length.";
        return out;
    }
    std::vector<Line> lines (traced.size ());
    double lo[2] = { 1e300, 1e300 }, hi[2] = { -1e300, -1e300 };
    for (size_t l = 0; l < traced.size (); ++l) {
        auto& line = lines[l];
        line.traced = &traced[l];
        line.closed = closed[l] != 0;
        const auto& t = traced[l];
        const size_t n = t.size ();
        if (n < 2)
            continue;
        line.along.assign (1, 0.0);
        for (size_t i = 0; i < (line.closed ? n : n - 1); ++i)
            line.along.push_back (line.along.back () + Length (t[(i + 1) % n] - t[i]));
        const double total = line.along.back ();
        if (!(total > 0.0)) {
            line.along.clear ();
            continue;
        }
        size_t pieces = size_t (std::max (1.0, std::round (total / step)));
        if (line.closed)
            pieces = std::max<size_t> (pieces, 3);
        for (size_t j = 0; j <= pieces; ++j)
            line.at.push_back (j == pieces ? total : total * double (j) / double (pieces));
        for (const auto& p : t)
            lo[0] = std::min (lo[0], p.x), lo[1] = std::min (lo[1], p.y), hi[0] = std::max (hi[0], p.x),
            hi[1] = std::max (hi[1], p.y);
    }
    for (const auto& p : outline)
        lo[0] = std::min (lo[0], p.x), lo[1] = std::min (lo[1], p.y), hi[0] = std::max (hi[0], p.x),
        hi[1] = std::max (hi[1], p.y);
    const double extent = std::max (hi[0] - lo[0], hi[1] - lo[1]);
    const double cell = std::max ({ step, extent / 256.0, 1.0e-6 });
    const double shortest = std::max (tolerance, 1.0e-6);

    // Each round splits every crossing piece; a split at least halves it, so the rounds are few.
    for (int round = 0;; ++round) {
        Vec3 where;
        const auto crossing = Crossings (lines, outline, cell, where);
        if (crossing.empty ())
            break;
        bool split = false;
        for (const auto& [l, pieces] : crossing) {
            auto& line = lines[size_t (l)];
            std::vector<double> added;
            for (const size_t j : pieces) {
                const double a = line.at[j], b = line.at[j + 1];
                if (b - a > 2.0 * shortest) {
                    added.push_back ((a + b) * 0.5);
                    continue;
                }
                // Short already: the traced points of the stretch, which lie on the line itself.
                for (const double d : line.along)
                    if (d > a + 1.0e-12 && d < b - 1.0e-12)
                        added.push_back (d);
            }
            if (added.empty ())
                continue;
            split = line.shortened = true;
            line.at.insert (line.at.end (), added.begin (), added.end ());
            std::sort (line.at.begin (), line.at.end ());
            line.at.erase (std::unique (line.at.begin (), line.at.end ()), line.at.end ());
        }
        if (!split || round == 200) {
            char text[160];
            std::snprintf (text, sizeof text,
                           "Contours cross near (%.2f, %.2f) even as traced; divide by length cannot keep them apart.",
                           where.x, where.y);
            out.error = text;
            return out;
        }
    }

    for (const auto& line : lines) {
        std::vector<Vec3> points;
        if (line.at.empty ())
            points = *line.traced;
        else
            for (size_t j = 0; j < line.at.size () - (line.closed ? 1 : 0); ++j)
                points.push_back (line.Point (line.at[j]));
        if (!line.closed && !line.at.empty ())
            points.back () = line.traced->back (); // the end on the boundary, exactly
        out.shortened += line.shortened ? 1 : 0;
        out.lines.push_back (std::move (points));
    }
    out.ok = true;
    return out;
}

} // namespace geomsrv::edit
