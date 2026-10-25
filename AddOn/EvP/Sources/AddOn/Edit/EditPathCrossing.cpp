#include "Edit/EditPathCrossing.hpp"
#include <algorithm>
#include <cmath>
#include <string>

namespace geomsrv::edit {

namespace {

constexpr double kSample = 0.5;     // metres between centreline samples
constexpr double kSame = 0.05;      // crossings closer than this are one
constexpr double kOnSurface = 0.02; // an end this far beyond a path's edge still meets it
constexpr double kAtBreak = 1.0e-3; // a break this close to a junction is the junction's

struct Line {
    AlignmentGeometry g;
    std::vector<double> stations;
    std::vector<Vec3> points;
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0, reach = 0.0; // bounds, and the widest half
};

bool Sample (const Path& p, Line& out)
{
    std::string error;
    if (!ValidatePath (p, out.g, error))
        return false;
    for (double s : SampleStations (p, out.g, kSample)) {
        Placement at;
        if (Locate (out.g, s, at)) {
            out.stations.push_back (s);
            out.points.push_back (at.point);
        }
    }
    if (out.points.size () < 2)
        return false;
    out.x0 = out.x1 = out.points[0].x;
    out.y0 = out.y1 = out.points[0].y;
    for (const auto& q : out.points) {
        out.x0 = std::min (out.x0, q.x), out.x1 = std::max (out.x1, q.x);
        out.y0 = std::min (out.y0, q.y), out.y1 = std::max (out.y1, q.y);
    }
    for (const auto& r : p.sections)
        out.reach = std::max ({ out.reach, r.widthLeft, r.widthRight, r.endLeft, r.endRight });
    return true;
}

bool Near (const Line& a, const Line& b, double margin)
{
    return a.x0 <= b.x1 + margin && b.x0 <= a.x1 + margin && a.y0 <= b.y1 + margin && b.y0 <= a.y1 + margin;
}

// Segments a-b and c-d meeting in plan, their ends included; `u` along a-b, `v` along c-d.
bool Cross (Vec3 a, Vec3 b, Vec3 c, Vec3 d, double& u, double& v)
{
    const double rx = b.x - a.x, ry = b.y - a.y, sx = d.x - c.x, sy = d.y - c.y;
    const double den = rx * sy - ry * sx;
    if (std::abs (den) < 1.0e-14)
        return false;
    const double qx = c.x - a.x, qy = c.y - a.y;
    u = (qx * sy - qy * sx) / den;
    v = (qx * ry - qy * rx) / den;
    constexpr double e = 1.0e-9;
    return u >= -e && u <= 1.0 + e && v >= -e && v <= 1.0 + e;
}

// Path b's surface `offset` off its centre at `station`, above the centre: the crossfall's share.
double Rise (const Path& b, double length, double station, double offset)
{
    if (b.sections.empty ())
        return 0.0;
    const Section s = SectionIn (b.sections, length, station);
    double left = 0.0, right = 0.0;
    EdgeDrops (s, left, right);
    const double width = offset >= 0.0 ? s.widthLeft : s.widthRight;
    const double share = width > 1.0e-9 ? std::min (std::abs (offset) / width, 1.0) : 0.0;
    return (offset >= 0.0 ? left : right) * share;
}

// A station within `near` of an end is that end: the junction is the end's own break.
double ToEnd (double s, double length, double near)
{
    return s < near ? 0.0 : s > length - near ? length : s;
}

GradeBreak* BreakOf (Path& p, Id id)
{
    for (auto& b : p.profile)
        if (b.id == id)
            return &b;
    return nullptr;
}

// Puts a break for the junction with path `other` at station `s`: an end's, or one already
// there, or a new one on the tangent (`z` when it was moved here). A vertical curve over the
// station gives way. kNoId when the profile will not take one.
Id Put (std::vector<GradeBreak>& b, double s, Id id, const GradeBreak* moved, Id other)
{
    for (auto& x : b)
        if (std::abs (x.station - s) <= kAtBreak) {
            if (x.crossing == kNoId)
                x.crossing = other;
            return x.id;
        }
    for (auto& x : b)
        if (x.curve > 0.0 && std::abs (s - x.station) < x.curve * 0.5)
            x.curve = std::max (0.0, 2.0 * std::abs (s - x.station) - 2.0 * kAtBreak);
    std::string error;
    if (!InsertBreak (b, s, id, error))
        return kNoId;
    for (auto& x : b)
        if (x.id == id) {
            x.crossing = other;
            if (moved != nullptr)
                x.z = moved->z, x.fixed = moved->fixed, x.binding = moved->binding;
        }
    return id;
}

} // namespace

std::vector<Junction> FindJunctions (const std::vector<Path>& paths)
{
    std::vector<Line> lines (paths.size ());
    std::vector<bool> ok (paths.size ());
    for (size_t i = 0; i < paths.size (); ++i)
        ok[i] = Sample (paths[i], lines[i]);
    std::vector<Junction> out;
    const auto known = [&] (size_t a, size_t b, Vec3 at, double reach) {
        for (const auto& j : out)
            if (((j.a == a && j.b == b) || (j.a == b && j.b == a)) && DistanceXY (j.at, at) < reach)
                return true;
        return false;
    };
    // A path's end on another path: a T, or an L where the other's end is there too.
    for (size_t a = 0; a < paths.size (); ++a)
        for (const double end : { 0.0, ok[a] ? lines[a].g.length : 0.0 }) {
            if (!ok[a])
                break;
            const Vec3 p = end == 0.0 ? lines[a].points.front () : lines[a].points.back ();
            for (size_t b = 0; b < paths.size (); ++b) {
                if (b == a || !ok[b] || !Near (lines[a], lines[b], lines[b].reach + kOnSurface))
                    continue;
                double s = 0.0, offset = 0.0;
                Placement on;
                if (!StationOf (lines[b].g, p, s, offset) || !Locate (lines[b].g, s, on))
                    continue;
                const Section r = SectionIn (paths[b].sections, lines[b].g.length, s);
                const double half = offset >= 0.0 ? r.widthLeft : r.widthRight;
                // Abreast of b (not beyond its end) and on its surface.
                if (DistanceXY (on.point, p) > std::abs (offset) + 1.0e-3 || std::abs (offset) > half + kOnSurface)
                    continue;
                if (known (a, b, p, half + lines[a].reach))
                    continue;
                out.push_back ({ a, b, end, ToEnd (s, lines[b].g.length, half),
                                 Rise (paths[b], lines[b].g.length, s, offset), p });
            }
        }
    // Centrelines crossing, away from the ends already joined.
    for (size_t a = 0; a < paths.size (); ++a)
        for (size_t b = a + 1; b < paths.size (); ++b) {
            if (!ok[a] || !ok[b] || !Near (lines[a], lines[b], 0.0))
                continue;
            const Line &la = lines[a], &lb = lines[b];
            for (size_t k = 0; k + 1 < la.points.size (); ++k)
                for (size_t l = 0; l + 1 < lb.points.size (); ++l) {
                    double u = 0.0, v = 0.0;
                    if (!Cross (la.points[k], la.points[k + 1], lb.points[l], lb.points[l + 1], u, v))
                        continue;
                    const Vec3 at = Lerp (la.points[k], la.points[k + 1], u);
                    if (known (a, b, at, kSame + la.reach + lb.reach))
                        continue;
                    const double sa = la.stations[k] + u * (la.stations[k + 1] - la.stations[k]);
                    const double sb = lb.stations[l] + v * (lb.stations[l + 1] - lb.stations[l]);
                    out.push_back (
                        { a, b, ToEnd (sa, la.g.length, kAtBreak), ToEnd (sb, lb.g.length, kAtBreak), 0.0, at });
                }
        }
    return out;
}

void LinkCrossings (Document& doc, const Document& previous)
{
    bool linked = false;
    for (const auto& p : doc.paths)
        for (const auto& b : p.profile)
            linked = linked || b.crossing != kNoId;
    if (doc.paths.size () < 2 && !linked)
        return;
    const auto junctions = FindJunctions (doc.paths);
    const std::vector<Path> original = doc.paths;
    std::vector<std::vector<Id>> claimed (doc.paths.size ());
    // The break path `p` keeps for its junction with `q` at station `s`: its own crossing
    // break with `q` nearest there (moved to it), or a new one.
    const auto claim = [&] (size_t p, size_t q, double s) -> Id {
        auto& b = doc.paths[p].profile;
        const Id other = doc.paths[q].id;
        int best = -1;
        for (size_t i = 0; i < b.size (); ++i)
            if (b[i].crossing == other &&
                std::find (claimed[p].begin (), claimed[p].end (), b[i].id) == claimed[p].end () &&
                (best < 0 || std::abs (b[i].station - s) < std::abs (b[static_cast<size_t> (best)].station - s)))
                best = int (i);
        Id id = kNoId;
        if (best >= 0 && std::abs (b[static_cast<size_t> (best)].station - s) <= kAtBreak)
            id = b[static_cast<size_t> (best)].id;
        else if (best >= 0 && best > 0 && size_t (best) + 1 < b.size ()) {
            const GradeBreak moved = b[static_cast<size_t> (best)];
            b.erase (b.begin () + best);
            id = Put (b, s, moved.id, &moved, other);
        }
        else
            id = Put (b, s, NewId (doc), nullptr, other);
        if (id != kNoId)
            claimed[p].push_back (id);
        return id;
    };
    struct Pair {
        size_t a, b;
        Id ba, bb;
        double rise;
    };
    std::vector<Pair> pairs;
    for (const auto& j : junctions) {
        const Id ba = claim (j.a, j.b, j.stationA), bb = claim (j.b, j.a, j.stationB);
        if (ba != kNoId && bb != kNoId)
            pairs.push_back ({ j.a, j.b, ba, bb, j.rise });
    }
    // Junctions that have gone: their breaks go too, an end's stays as an end.
    for (size_t p = 0; p < doc.paths.size (); ++p) {
        auto& b = doc.paths[p].profile;
        for (size_t i = b.size (); i-- > 0;) {
            if (b[i].crossing == kNoId ||
                std::find (claimed[p].begin (), claimed[p].end (), b[i].id) != claimed[p].end ())
                continue;
            if (i == 0 || i + 1 == b.size ())
                b[i].crossing = kNoId;
            else
                b.erase (b.begin () + static_cast<std::ptrdiff_t> (i));
        }
    }
    // One height each: a's break sits `rise` above b's.
    const auto changed = [&] (size_t p, const GradeBreak& x) {
        if (const Path* before = FindPath (previous, doc.paths[p].id))
            for (const auto& y : before->profile)
                if (y.id == x.id)
                    return std::abs (y.z - x.z) > 1.0e-9;
        return false;
    };
    for (const auto& pr : pairs) {
        GradeBreak *ba = BreakOf (doc.paths[pr.a], pr.ba), *bb = BreakOf (doc.paths[pr.b], pr.bb);
        if (ba == nullptr || bb == nullptr)
            continue;
        const bool ca = ba->fixed || changed (pr.a, *ba), cb = bb->fixed || changed (pr.b, *bb);
        const bool aWins = ca != cb ? ca : doc.paths[pr.a].id < doc.paths[pr.b].id;
        if (aWins && !bb->fixed)
            bb->z = ba->z - pr.rise;
        else if (!aWins && !ba->fixed)
            ba->z = bb->z + pr.rise;
    }
    for (size_t p = 0; p < doc.paths.size (); ++p) {
        AlignmentGeometry g;
        std::string error;
        if (!ValidatePath (doc.paths[p], g, error))
            doc.paths[p] = original[p];
    }
}

} // namespace geomsrv::edit
