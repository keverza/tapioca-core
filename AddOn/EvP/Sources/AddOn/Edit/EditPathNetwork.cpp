#include "Edit/EditPathNetwork.hpp"
#include "Edit/EditDocument.hpp"
#include <clipper2/clipper.h>
#include <clipper2/clipper.triangulation.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace geomsrv::edit {

namespace {

namespace cp = Clipper2Lib;

constexpr int kPrecision = 3;          // millimetres
constexpr double kArcTolerance = 0.01; // metres a rounding may stray from the true arc
constexpr double kOn = 0.003;          // a corner this close to a path's edge is on it
constexpr double kNeighbour = 0.05;    // how far along the outline a corner's legs are read
constexpr double kMinTurn = 0.14;      // radians (8 degrees): less is a straight run
constexpr double kMaxReentrant = 50.0; // metres: a re-entrant corner's largest radius
constexpr double kPi = 3.14159265358979323846;

cp::PathD Polygon (const PathOutline& o)
{
    cp::PathD ring;
    for (const auto& p : o.left)
        ring.push_back ({ p.x, p.y });
    for (size_t i = o.right.size (); i-- > 0;)
        ring.push_back ({ o.right[i].x, o.right[i].y });
    return ring;
}

cp::PathsD Inflate (const cp::PathsD& paths, double delta)
{
    return cp::InflatePaths (paths, delta, cp::JoinType::Round, cp::EndType::Polygon, 2.0, kPrecision, kArcTolerance);
}

struct Source {
    const Path* path = nullptr;
    AlignmentGeometry geometry;
    PathOutline outline;
    double half = 0.0; // its narrowest half width
};

// The height at a plan point: the path it lies in (or nearest, at a junction's rounding).
double HeightAt (const std::vector<Source>& sources, Vec3 p)
{
    double best = std::numeric_limits<double>::infinity (), z = 0.0;
    for (const auto& s : sources) {
        double h = 0.0, beyond = 0.0;
        if (SurfaceHeight (*s.path, s.geometry, p, h, beyond) && beyond < best)
            best = beyond, z = h;
    }
    return z;
}

double SegmentDistance (Vec3 p, Vec3 a, Vec3 b)
{
    const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    const double t = l2 > 0.0 ? std::clamp (((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0, 1.0) : 0.0;
    return std::hypot (p.x - a.x - t * dx, p.y - a.y - t * dy);
}

// Where `p` lies on a path's outline: on its left or right edge (with the sample it is at,
// when it is one), else across an end. False when it is not on it.
bool OnOutline (const PathOutline& o, Vec3 p, KerbSide& side, int& sample)
{
    sample = -1;
    for (const auto [edge, s] : { std::pair { &o.left, KerbSide::Left }, std::pair { &o.right, KerbSide::Right } }) {
        bool on = false;
        for (size_t k = 0; k < edge->size (); ++k) {
            if (DistanceXY ((*edge)[k], p) <= kOn) {
                side = s, sample = int (k);
                return true;
            }
            on = on || (k + 1 < edge->size () && SegmentDistance (p, (*edge)[k], (*edge)[k + 1]) <= kOn);
        }
        if (on) {
            side = s;
            return true;
        }
    }
    if (!o.left.empty () && SegmentDistance (p, o.right.front (), o.left.front ()) <= kOn) {
        side = KerbSide::Start;
        return true;
    }
    if (!o.left.empty () && SegmentDistance (p, o.left.back (), o.right.back ()) <= kOn) {
        side = KerbSide::End;
        return true;
    }
    return false;
}

// The control point a path's own corner is at: an end, or a sharp bend at that station.
Id CornerPoint (const Source& s, const PathOutline& o, Vec3 p, KerbSide side)
{
    const auto& edge = side == KerbSide::Right ? o.right : o.left;
    const auto& points = s.path->points;
    for (size_t k = 0; k < edge.size (); ++k) {
        if (DistanceXY (edge[k], p) > kOn)
            continue;
        const double station = o.stations[k];
        if (station <= 1.0e-6)
            return points.front ().id;
        if (station >= s.geometry.length - 1.0e-6)
            return points.back ().id;
        for (size_t i = 1; i + 1 < points.size (); ++i)
            if (!(points[i].radius > 0.0) && std::abs (s.geometry.pointStations[i] - station) < 1.0e-6)
                return points[i].id;
    }
    return kNoId;
}

double RadiusOf (const Path& owner, const KerbCorner& key, double fallback)
{
    for (const auto& k : owner.kerbs)
        if (k.Same (key))
            return k.radius;
    return fallback;
}

// The corners of the sharp area, each said what it is and how it is rounded.
std::vector<PavedCorner> FindCorners (const cp::PathsD& area, const std::vector<Source>& sources)
{
    std::vector<PavedCorner> out;
    for (const auto& ring : area) {
        const size_t n = ring.size ();
        if (n < 3)
            continue;
        for (size_t i = 0; i < n; ++i) {
            const Vec3 v { ring[i].x, ring[i].y, 0.0 };
            // Its legs, read a little way along the outline either side.
            const auto along = [&] (int dir) {
                size_t j = i;
                for (size_t k = 0; k + 1 < n; ++k) {
                    j = dir > 0 ? (j + 1) % n : (j + n - 1) % n;
                    if (std::hypot (ring[j].x - v.x, ring[j].y - v.y) >= kNeighbour)
                        break;
                }
                return Vec3 { ring[j].x, ring[j].y, 0.0 };
            };
            Vec3 toPrev, toNext;
            if (!Normalize (along (-1) - v, toPrev) || !Normalize (along (1) - v, toNext))
                continue;
            const Vec3 in { -toPrev.x, -toPrev.y, 0.0 };
            const double turn = std::atan2 (in.x * toNext.y - in.y * toNext.x, Dot (in, toNext));
            if (std::abs (turn) < kMinTurn)
                continue;
            PavedCorner c;
            c.convex = turn > 0.0; // the area is on the left of every ring
            c.half = std::acos (std::clamp (Dot (toPrev, toNext), -1.0, 1.0)) * 0.5;
            if (!Normalize (toPrev + toNext, c.bisector))
                continue;
            // Whose it is.
            struct Owner {
                size_t source;
                KerbSide side;
            };
            std::vector<Owner> owners;
            for (size_t s = 0; s < sources.size (); ++s) {
                KerbSide side = KerbSide::Left;
                int sample = -1;
                if (OnOutline (sources[s].outline, v, side, sample))
                    owners.push_back ({ s, side });
            }
            if (owners.empty ())
                continue;
            double half = std::numeric_limits<double>::infinity ();
            for (const auto& o : owners)
                half = std::min (half, sources[o.source].half);
            if (owners.size () >= 2) {
                Owner a = owners[0], b = owners[1];
                if (sources[b.source].path->id < sources[a.source].path->id)
                    std::swap (a, b);
                const Path &pa = *sources[a.source].path, &pb = *sources[b.source].path;
                c.path = pa.id;
                c.key.other = pb.id;
                c.key.side = a.side;
                c.key.otherSide = b.side;
                c.key.radius = RadiusOf (pa, c.key, std::max (pa.edgeRadius, pb.edgeRadius));
            }
            else {
                const Source& s = sources[owners[0].source];
                const KerbSide side = owners[0].side;
                c.key.point = CornerPoint (s, s.outline, v,
                                           side == KerbSide::Start || side == KerbSide::End ? KerbSide::Left : side);
                if (c.key.point == kNoId)
                    continue; // where a range changes width, or a tight bend's fold: not a kerb corner
                c.path = s.path->id;
                c.key.side = side;
                c.key.radius = RadiusOf (*s.path, c.key, s.path->edgeRadius);
            }
            c.maxRadius = c.convex ? 0.98 * (std::isfinite (half) ? half : 0.0) : kMaxReentrant;
            c.key.radius = std::min (c.key.radius, c.maxRadius);
            c.at = { v.x, v.y, HeightAt (sources, v) };
            out.push_back (c);
        }
    }
    return out;
}

Vec3 Turned (Vec3 d, double angle)
{
    const double c = std::cos (angle), s = std::sin (angle);
    return { d.x * c - d.y * s, d.x * s + d.y * c, 0.0 };
}

// The wedge a corner's rounding stays in: between its legs (a little wider, for curved ones),
// out to where its tangents reach.
cp::PathD Wedge (const PavedCorner& c, Vec3 toPrev, Vec3 toNext, double reach)
{
    const double spread = std::max (0.0, std::min (0.14, (3.05 - 2.0 * c.half) * 0.5));
    const bool prevLeft = c.bisector.x * toPrev.y - c.bisector.y * toPrev.x > 0.0;
    const Vec3 a = Turned (toPrev, prevLeft ? spread : -spread), b = Turned (toNext, prevLeft ? -spread : spread);
    const Vec3 p[4] = { c.at, c.at + a * reach, c.at + c.bisector * (reach * 1.5), c.at + b * reach };
    cp::PathD w;
    for (const auto& q : p)
        w.push_back ({ q.x, q.y });
    if (cp::Area (w) < 0.0)
        std::reverse (w.begin (), w.end ());
    return w;
}

// Each corner rounded by its own radius: what an opening (outside corners) or a closing
// (re-entrant ones) by that radius changes, kept to the corner's wedge.
cp::PathsD Round (const cp::PathsD& sharp, const std::vector<PavedCorner>& corners)
{
    std::map<std::pair<double, bool>, cp::PathsD> changes;
    cp::PathsD cut, fill;
    for (const auto& c : corners) {
        const double r = c.key.radius;
        if (!(r > 1.0e-4) || !(c.half > 1.0e-3))
            continue;
        auto found = changes.find ({ r, c.convex });
        if (found == changes.end ()) {
            const cp::PathsD morph = c.convex ? Inflate (Inflate (sharp, -r), r) : Inflate (Inflate (sharp, r), -r);
            const cp::PathsD change = c.convex ? cp::Difference (sharp, morph, cp::FillRule::NonZero, kPrecision)
                                               : cp::Difference (morph, sharp, cp::FillRule::NonZero, kPrecision);
            found = changes.emplace (std::pair { r, c.convex }, change).first;
        }
        // The legs from the bisector and the half angle.
        const Vec3 toPrev = Turned (c.bisector, c.half), toNext = Turned (c.bisector, -c.half);
        const double reach = std::min (r / std::tan (c.half), 12.0 * r) * 1.05 + 0.01;
        const cp::PathsD part = cp::Intersect (found->second, cp::PathsD { Wedge (c, toPrev, toNext, reach) },
                                               cp::FillRule::NonZero, kPrecision);
        (c.convex ? cut : fill).insert ((c.convex ? cut : fill).end (), part.begin (), part.end ());
    }
    if (cut.empty () && fill.empty ())
        return sharp;
    cp::PathsD area = cut.empty () ? sharp : cp::Difference (sharp, cut, cp::FillRule::NonZero, kPrecision);
    return fill.empty () ? area : cp::Union (area, fill, cp::FillRule::NonZero, kPrecision);
}

} // namespace

bool SurfaceHeight (const Path& path, const AlignmentGeometry& g, Vec3 p, double& z, double& beyond)
{
    double station = 0.0, offset = 0.0;
    StationFrame f;
    if (!StationOf (g, p, station, offset) || !Evaluate (path, g, station, f))
        return false;
    const bool left = offset >= 0.0;
    const double width = left ? f.section.widthLeft : f.section.widthRight;
    double dropLeft = 0.0, dropRight = 0.0;
    EdgeDrops (f.section, dropLeft, dropRight);
    const double share = width > 1.0e-9 ? std::min (std::abs (offset) / width, 1.0) : 0.0;
    z = f.centre.z + (left ? dropLeft : dropRight) * share;
    beyond = std::abs (offset) - width;
    return true;
}

PavedArea Pave (const std::vector<Path>& paths, double step, bool surface)
{
    PavedArea out;
    std::vector<Source> sources;
    cp::PathsD united;
    for (const auto& path : paths) {
        Source s;
        s.path = &path;
        std::string error;
        if (!ValidatePath (path, s.geometry, error) || !Outline (path, step, s.outline, error) ||
            s.outline.left.size () < 2)
            continue;
        s.half = std::numeric_limits<double>::infinity ();
        for (const auto& r : path.sections)
            s.half = std::min ({ s.half, r.widthLeft, r.widthRight });
        // Its own outline, clean: a tight bend's fold resolved.
        const cp::PathsD own = cp::Union (cp::PathsD { Polygon (s.outline) }, cp::FillRule::NonZero, kPrecision);
        united.insert (united.end (), own.begin (), own.end ());
        sources.push_back (std::move (s));
    }
    if (united.empty ())
        return out;
    // One area: crossings and touching ends made junctions; then every corner its own rounding.
    united = cp::Union (united, cp::FillRule::NonZero, kPrecision);
    out.corners = FindCorners (united, sources);
    united = Round (united, out.corners);
    // Straight runs keep their ends only: the samples along them say nothing (1 mm).
    united = cp::SimplifyPaths (united, 0.001, true);
    const auto lift = [&] (const cp::PointD& q) {
        const Vec3 p { q.x, q.y, 0.0 };
        return Vec3 { q.x, q.y, HeightAt (sources, p) };
    };
    for (const auto& ring : united) {
        if (ring.size () < 3)
            continue;
        std::vector<Vec3> r;
        for (const auto& q : ring)
            r.push_back (lift (q));
        out.rings.push_back (std::move (r));
    }
    if (!surface)
        return out;
    // The surface: each outline with its holes triangulated.
    for (const auto& outer : united) {
        if (cp::Area (outer) <= 0.0)
            continue;
        cp::PathsD region { outer };
        for (const auto& hole : united)
            if (cp::Area (hole) < 0.0 && !hole.empty () &&
                cp::PointInPolygon (hole.front (), outer) != cp::PointInPolygonResult::IsOutside)
                region.push_back (hole);
        cp::PathsD triangles;
        if (cp::Triangulate (region, kPrecision, triangles, false) != cp::TriangulateResult::success)
            continue; // a refused region stays outlined, unfilled
        for (auto& t : triangles) {
            if (t.size () != 3)
                continue;
            if (cp::Area (t) < 0.0)
                std::reverse (t.begin (), t.end ());
            for (const auto& q : t) {
                out.triangles.push_back (static_cast<uint32_t> (out.vertices.size ()));
                out.vertices.push_back (lift (q));
            }
        }
    }
    return out;
}

Vec3 KerbGripAt (const PavedCorner& c, double minInset)
{
    const double r = c.key.radius;
    const double inset = r > 0.0 && c.half > 1.0e-6 ? r / std::sin (c.half) - r : 0.0;
    return c.at + c.bisector * std::max (inset, minInset);
}

double KerbRadiusFromGrip (const PavedCorner& c, Vec3 p, double minInset)
{
    const double d = Dot (Vec3 { p.x - c.at.x, p.y - c.at.y, 0.0 }, c.bisector);
    if (!(d > minInset) || !(c.half > 1.0e-6) || c.half > kPi * 0.5 - 1.0e-6)
        return 0.0;
    return d / (1.0 / std::sin (c.half) - 1.0);
}

bool SetKerbRadius (Document& doc, Id path, const KerbCorner& corner, double radius, std::string& error)
{
    Path* p = FindPath (doc, path);
    if (p == nullptr) {
        error = "the path no longer exists";
        return false;
    }
    if (!(radius >= 0.0) || !std::isfinite (radius)) {
        error = "a kerb radius cannot be negative";
        return false;
    }
    for (auto& k : p->kerbs)
        if (k.Same (corner)) {
            k.radius = radius;
            return true;
        }
    KerbCorner k = corner;
    k.radius = radius;
    p->kerbs.push_back (k);
    return true;
}

} // namespace geomsrv::edit
