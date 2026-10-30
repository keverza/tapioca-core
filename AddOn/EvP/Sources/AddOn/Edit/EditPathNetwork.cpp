#include "Edit/EditPathNetwork.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPathCrossing.hpp"
#include "Edit/EditPathSurface.hpp"
#include <clipper2/clipper.h>
#include <clipper2/clipper.triangulation.h>
#include <algorithm>
#include <map>
#include <cmath>
#include <limits>

namespace geomsrv::edit {

namespace {

namespace cp = Clipper2Lib;

constexpr int kPrecision = 5; // Boolean/mesh grid is finer than the 1 mm arc chord target.
const double kLattice = std::ldexp (1.0, std::ilogb (std::pow (10.0, kPrecision)) + 1);
const fillet2d::Settings kFilletSettings = [] {
    fillet2d::Settings settings;
    settings.lengthTolerance = 2 / kLattice;
    return settings;
}();
constexpr double kOn = 0.003;          // a corner this close to a path's edge is on it
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

struct Source {
    const Path* path = nullptr;
    AlignmentGeometry geometry;
    PathOutline outline;
    std::vector<std::pair<Vec3, double>> junctions;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0; // the outline's plan bounds
    double widest = 0;                     // its widest half width anywhere
    // Where any of its junctions can reach: a junction's influence ends at twice its radius.
    double jx0 = 0, jy0 = 0, jx1 = -1, jy1 = -1;
};

void BoundJunctions (Source& s)
{
    s.jx0 = s.jy0 = std::numeric_limits<double>::infinity ();
    s.jx1 = s.jy1 = -std::numeric_limits<double>::infinity ();
    for (const auto& [at, radius] : s.junctions) {
        const double reach = 2.0 * radius;
        s.jx0 = std::min (s.jx0, at.x - reach), s.jx1 = std::max (s.jx1, at.x + reach);
        s.jy0 = std::min (s.jy0, at.y - reach), s.jy1 = std::max (s.jy1, at.y + reach);
    }
}

void Bound (Source& s)
{
    s.x0 = s.x1 = s.outline.centre.front ().x;
    s.y0 = s.y1 = s.outline.centre.front ().y;
    for (const auto* edge : { &s.outline.left, &s.outline.right, &s.outline.centre })
        for (const auto& q : *edge) {
            s.x0 = std::min (s.x0, q.x), s.x1 = std::max (s.x1, q.x);
            s.y0 = std::min (s.y0, q.y), s.y1 = std::max (s.y1, q.y);
        }
    for (const auto& r : s.path->sections)
        s.widest =
            std::max ({ s.widest, r.widthLeft, r.widthRight, r.taper ? r.endLeft : 0.0, r.taper ? r.endRight : 0.0 });
}

// No less than what SurfaceHeight can report as `beyond` at `p`: the point is at least the
// bounds' distance from the centreline, and past an end `beyond` keeps the larger of the
// side and the along terms, so half the remaining distance is always a floor.
double BeyondFloor (const Source& s, Vec3 p)
{
    const double dx = std::max ({ s.x0 - p.x, 0.0, p.x - s.x1 }), dy = std::max ({ s.y0 - p.y, 0.0, p.y - s.y1 });
    return 0.5 * (std::hypot (dx, dy) - s.widest) - 1.0e-9;
}

// The height at a plan point: the path it lies in (or nearest, at a junction's rounding).
//
// The nearest is the path with the least `beyond`, the first of them in `sources` order on a
// tie, and the junctions blend in `sources` order. Finding that least value does not need the
// order, so the paths are tried nearest bound first and the search stops where no bound can
// beat it; the winner and the blend are then read in the original order, so the result is the
// one a plain pass over every path gives, to the bit.
double HeightAt (const std::vector<Source>& sources, Vec3 p)
{
    struct Probe {
        double floor = 0, influence = 0, h = 0, beyond = 0;
        bool done = false, ok = false;
    };
    thread_local std::vector<Probe> probes;
    thread_local std::vector<size_t> order;
    probes.assign (sources.size (), {});
    order.clear ();
    for (size_t i = 0; i < sources.size (); ++i) {
        const Source& s = sources[i];
        Probe& probe = probes[i];
        if (p.x >= s.jx0 && p.x <= s.jx1 && p.y >= s.jy0 && p.y <= s.jy1)
            for (const auto& [at, radius] : s.junctions) {
                const double t = std::clamp (2.0 - DistanceXY (p, at) / radius, 0.0, 1.0);
                probe.influence = std::max (probe.influence, t * t * (3 - 2 * t));
            }
        probe.floor = BeyondFloor (s, p);
        order.push_back (i);
    }
    const auto measure = [&] (size_t i) -> const Probe& {
        Probe& probe = probes[i];
        if (!probe.done) {
            probe.ok = SurfaceHeight (*sources[i].path, sources[i].geometry, p, probe.h, probe.beyond);
            probe.done = true;
        }
        return probe;
    };
    std::sort (order.begin (), order.end (), [&] (size_t a, size_t b) { return probes[a].floor < probes[b].floor; });
    double best = std::numeric_limits<double>::infinity ();
    for (size_t i : order) {
        if (probes[i].floor > best)
            break; // every later bound is larger still
        const Probe& probe = measure (i);
        if (probe.ok && probe.beyond < best)
            best = probe.beyond;
    }
    double z = 0.0;
    for (const auto& probe : probes)
        if (probe.done && probe.ok && probe.beyond == best) {
            z = probe.h;
            break;
        }
    double weighted = 0, weights = 0, blend = 0;
    for (size_t i = 0; i < sources.size (); ++i) {
        if (probes[i].influence <= 0)
            continue;
        const Probe& probe = measure (i);
        if (!probe.ok)
            continue;
        const Source& s = sources[i];
        double station = 0, offset = 0;
        Placement frame;
        if (!StationOf (s.geometry, p, station, offset) || !Locate (s.geometry, station, frame))
            continue;
        const Section section = SectionIn (s.path->sections, s.geometry.length, station);
        const double width = std::max (section.widthLeft, section.widthRight);
        const double distance = DistanceXY (p, frame.point) / width;
        const double weight = probe.influence / std::pow (1 + distance * distance, 2);
        weighted += weight * probe.h;
        weights += weight;
        blend = std::max (blend, probe.influence);
    }
    // A continuous partition of unity replaces the old deepest-strip winner
    // inside junctions. Feather back to the authored arms outside the junction.
    return weights > 0 ? z * (1 - blend) + weighted / weights * blend : z;
}

double SegmentDistance (Vec3 p, Vec3 a, Vec3 b)
{
    const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    const double t = l2 > 0.0 ? std::clamp (((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0, 1.0) : 0.0;
    return std::hypot (p.x - a.x - t * dx, p.y - a.y - t * dy);
}

// Where along segment a-b the plan point p projects, clamped to it (0..1).
double AlongSegment (Vec3 p, Vec3 a, Vec3 b)
{
    const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    return l2 > 0.0 ? std::clamp (((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0, 1.0) : 0.0;
}

// Where `p` lies on a path's outline: on its left or right edge (with the sample it is at,
// when it is one, and its station either way), else across an end. False when it is not on
// it. The station reads the range in force there, so a corner's cap is local to the width
// it sits in, not the path's narrowest somewhere else.
bool OnOutline (const PathOutline& o, Vec3 p, KerbSide& side, int& sample, double& station)
{
    sample = -1;
    station = 0.0;
    for (const auto [edge, s] : { std::pair { &o.left, KerbSide::Left }, std::pair { &o.right, KerbSide::Right } }) {
        double on = 2.0 * kOn, at = 0.0;
        for (size_t k = 0; k < edge->size (); ++k) {
            if (DistanceXY ((*edge)[k], p) <= kOn) {
                side = s, sample = int (k), station = o.stations[k];
                return true;
            }
            if (k + 1 >= edge->size ())
                continue;
            const double d = SegmentDistance (p, (*edge)[k], (*edge)[k + 1]);
            if (d <= on && d <= kOn) {
                on = d;
                at = o.stations[k] + AlongSegment (p, (*edge)[k], (*edge)[k + 1]) * (o.stations[k + 1] - o.stations[k]);
            }
        }
        if (on <= kOn) {
            side = s;
            station = at;
            return true;
        }
    }
    if (!o.left.empty () && SegmentDistance (p, o.right.front (), o.left.front ()) <= kOn) {
        side = KerbSide::Start;
        station = o.stations.front ();
        return true;
    }
    if (!o.left.empty () && SegmentDistance (p, o.left.back (), o.right.back ()) <= kOn) {
        side = KerbSide::End;
        station = o.stations.back ();
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

// The narrowest half width at `station`: a rounding is cut no deeper than the width it
// sits in, not the path's narrowest width somewhere else along it.
double LocalHalf (const Source& source, double station)
{
    if (source.path->sections.empty ())
        return 0.0;
    const Section section = SectionIn (source.path->sections, source.geometry.length, station);
    return std::min (section.widthLeft, section.widthRight);
}

// The corners of the sharp area, each said what it is and how it is rounded.
std::vector<PavedCorner> FindCorners (const cp::PathsD& area, const std::vector<Source>& sources)
{
    std::vector<PavedCorner> out;
    for (const auto& ring : area) {
        const size_t n = ring.size ();
        if (n < 3)
            continue;
        // Each vertex's legs, read a little way along the outline either side, and its turn.
        std::vector<Vec3> prevs (n), nexts (n);
        std::vector<bool> turning (n, false);
        for (size_t i = 0; i < n; ++i) {
            const Vec3 v { ring[i].x, ring[i].y, 0.0 };
            const auto along = [&] (int dir) {
                const size_t j = dir > 0 ? (i + 1) % n : (i + n - 1) % n;
                return Vec3 { ring[j].x, ring[j].y, 0.0 };
            };
            if (!Normalize (along (-1) - v, prevs[i]) || !Normalize (along (1) - v, nexts[i]))
                continue;
            const Vec3 in { -prevs[i].x, -prevs[i].y, 0.0 };
            turning[i] = std::abs (std::atan2 (in.x * nexts[i].y - in.y * nexts[i].x, Dot (in, nexts[i]))) >= kMinTurn;
        }
        for (size_t i = 0; i < n; ++i) {
            if (!turning[i])
                continue;
            const Vec3 v { ring[i].x, ring[i].y, 0.0 };
            const Vec3 toPrev = prevs[i], toNext = nexts[i];
            const Vec3 in { -toPrev.x, -toPrev.y, 0.0 };
            const double turn = std::atan2 (in.x * toNext.y - in.y * toNext.x, Dot (in, toNext));
            PavedCorner c;
            c.convex = turn > 0.0; // the area is on the left of every ring
            c.half = std::acos (std::clamp (Dot (toPrev, toNext), -1.0, 1.0)) * 0.5;
            if (!Normalize (toPrev + toNext, c.bisector))
                continue;
            // Whose it is.
            struct Owner {
                size_t source;
                KerbSide side;
                double station;
            };
            std::vector<Owner> owners;
            for (size_t s = 0; s < sources.size (); ++s) {
                KerbSide side = KerbSide::Left;
                int sample = -1;
                double station = 0.0;
                // OnOutline accepts nothing farther than kOn from the outline's samples.
                const Source& src = sources[s];
                if (v.x < src.x0 - kOn || v.x > src.x1 + kOn || v.y < src.y0 - kOn || v.y > src.y1 + kOn)
                    continue;
                if (OnOutline (src.outline, v, side, sample, station))
                    owners.push_back ({ s, side, station });
            }
            if (owners.empty ())
                continue;
            double half = std::numeric_limits<double>::infinity ();
            for (const auto& o : owners)
                half = std::min (half, LocalHalf (sources[o.source], o.station));
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
            // Width is a hardscape policy; shared-edge/nonlocal fitting belongs
            // to Fillet2D, not independent half-edge clamps at each corner.
            c.maxRadius = c.convex ? 0.98 * (std::isfinite (half) ? half : 0.0) : kMaxReentrant;
            c.radiusLimit = c.maxRadius;
            c.requestedRadius = c.key.radius;
            c.key.radius = std::min (c.key.radius, c.maxRadius);
            c.at = { v.x, v.y, HeightAt (sources, v) };
            out.push_back (c);
        }
    }
    return out;
}

// A kerb corner where a curved edge meets another (the user, 2026-10-24: "intersection of arc and
// straight line allows too small fillets"). The curve arrives as its sampled chords, and a fillet
// can only use the chord beside it, so a corner on a curve got a radius of a few centimetres. Each
// corner's sampled neighbours within its feasible tangent reach -- slight turns that are not
// corners of their own -- are dropped. Unfitted requested radii must not erase distant curves.
cp::PathD Thinned (const cp::PathD& ring, const std::vector<PavedCorner>& corners)
{
    const size_t n = ring.size ();
    const auto requested = [&] (const cp::PointD& p) {
        for (const auto& c : corners)
            if (c.at.x == p.x && c.at.y == p.y)
                return c.key.radius / std::tan (c.half);
        return 0.0;
    };
    const auto slight = [&] (size_t i) {
        const auto &a = ring[(i + n - 1) % n], &b = ring[i], &c = ring[(i + 1) % n];
        const double ux = b.x - a.x, uy = b.y - a.y, vx = c.x - b.x, vy = c.y - b.y;
        return std::abs (std::atan2 (ux * vy - uy * vx, ux * vx + uy * vy)) < kMinTurn;
    };
    std::vector<bool> keep (n, true);
    for (size_t i = 0; i < n && n > 4; ++i) {
        const double radius = requested (ring[i]);
        if (!(radius > 0))
            continue;
        for (const int dir : { -1, 1 }) {
            for (size_t step = 1; step + 2 < n; ++step) {
                const size_t j = (i + n + dir * step) % n;
                const double d = std::hypot (ring[j].x - ring[i].x, ring[j].y - ring[i].y);
                if (d >= radius || requested (ring[j]) > 0 || !slight (j))
                    break;
                keep[j] = false;
            }
        }
    }
    cp::PathD thinned;
    for (size_t i = 0; i < n; ++i)
        if (keep[i])
            thinned.push_back (ring[i]);
    return thinned.size () >= 3 ? thinned : ring;
}

bool Round (const cp::PathsD& sharpRings, PavedArea& out, cp::PathsD& rounded)
{
    cp::PathsD sharp;
    for (const auto& ring : sharpRings)
        sharp.push_back (Thinned (ring, out.corners));
    std::vector<fillet2d::InputRing> input;
    for (const auto& ring : sharp) {
        fillet2d::InputRing made;
        for (const auto& p : ring) {
            made.points.push_back ({ p.x, p.y });
            const auto found = std::find_if (out.corners.begin (), out.corners.end (),
                                             [&] (const PavedCorner& c) { return c.at.x == p.x && c.at.y == p.y; });
            made.corners.push_back (found == out.corners.end ()
                                        ? fillet2d::Request {}
                                        : fillet2d::Request { found->requestedRadius, found->maxRadius });
        }
        input.push_back (std::move (made));
    }
    if (!fillet2d::Build (input, kFilletSettings, out.boundary, out.warning))
        return false;
    std::vector<std::vector<fillet2d::Point>> sampled;
    if (!fillet2d::Tessellate (out.boundary, kFilletSettings, sampled, out.warning))
        return false;
    std::vector<fillet2d::InputRing> quantized;
    for (const auto& ring : sampled) {
        cp::PathD made;
        fillet2d::InputRing check;
        for (const auto& p : ring) {
            const fillet2d::Point q { std::round (p.x * kLattice) / kLattice, std::round (p.y * kLattice) / kLattice };
            if (!check.points.empty () && check.points.back () == q)
                continue;
            check.points.push_back (q);
            made.push_back ({ q.x, q.y });
        }
        quantized.push_back (std::move (check));
        rounded.push_back (std::move (made));
    }
    fillet2d::Region check;
    if (!fillet2d::Build (quantized, kFilletSettings, check, out.warning))
        return false;
    for (size_t r = 0; r < check.rings.size (); ++r) {
        if (check.rings[r].hole != out.boundary.rings[r].hole ||
            check.rings[r].parent != out.boundary.rings[r].parent) {
            out.warning = "sampled fillet changed region nesting";
            return false;
        }
        for (const auto& c : out.boundary.rings[r].corners) {
            const auto p = input[r].points[c.source];
            for (auto& corner : out.corners)
                if (corner.at.x == p.x && corner.at.y == p.y) {
                    corner.key.radius = c.radius;
                    corner.maxRadius = c.maxRadius;
                }
        }
    }
    return true;
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
    const double share = width > 1.0e-9 ? std::abs (offset) / width : 0.0;
    z = f.centre.z + (left ? dropLeft : dropRight) * share;
    beyond = std::abs (offset) - width;
    const Vec3 delta = p - f.centre;
    if (station <= 1e-8)
        beyond = std::max (beyond, -Dot (delta, f.tangent));
    else if (station >= g.length - 1e-8)
        beyond = std::max (beyond, Dot (delta, f.tangent));
    // Continue the tangent plane into a junction rounding, not a flat end-cap
    // plateau that packs all of the contour change into one narrow band.
    if (station <= 1e-8 || station >= g.length - 1e-8)
        z += f.grade * Dot (delta, f.tangent);
    // A joined sharp bend is one path but still has two tangent planes. Blend
    // those locally too, instead of reintroducing a nearest-leg cliff inside it.
    for (size_t i = 1; i < g.elements.size (); ++i) {
        const auto& a = g.elements[i - 1];
        const auto& b = g.elements[i];
        Vec3 ta, tb;
        if (a.kind != ElementKind::Line || b.kind != ElementKind::Line || !Normalize (a.b - a.a, ta) ||
            !Normalize (b.b - b.a, tb) || Dot (ta, tb) > 1 - 1e-9)
            continue;
        StationFrame corner;
        if (!Evaluate (path, g, b.start, corner))
            continue;
        const double width = std::max (corner.section.widthLeft, corner.section.widthRight);
        const double t = std::clamp (2 - DistanceXY (p, corner.centre) / (2 * width), 0.0, 1.0);
        if (t <= 0)
            continue;
        double h0 = 0, h1 = 0, grade0 = 0, grade1 = 0;
        Elevation (path.profile, std::max (0.0, b.start - 1e-7), h0, &grade0);
        Elevation (path.profile, std::min (g.length, b.start + 1e-7), h1, &grade1);
        const Vec3 d = p - corner.centre;
        double dl = 0, dr = 0;
        EdgeDrops (corner.section, dl, dr);
        const auto plane = [&] (Vec3 tangent, double grade, bool incoming) {
            const double along = Dot (d, tangent), side = d.x * -tangent.y + d.y * tangent.x;
            const double w = side >= 0 ? corner.section.widthLeft : corner.section.widthRight;
            const double height = corner.centre.z + along * grade + std::abs (side) / w * (side >= 0 ? dl : dr);
            const double beyond = incoming ? std::max (0.0, along) : std::min (0.0, along);
            const double weight = 1 / std::pow (1 + (side * side + beyond * beyond) / (width * width), 2);
            return std::pair { height, weight };
        };
        const auto [za, wa] = plane (ta, grade0, true);
        const auto [zb, wb] = plane (tb, grade1, false);
        const double blend = t * t * (3 - 2 * t);
        z = z * (1 - blend) + (za * wa + zb * wb) / (wa + wb) * blend;
    }
    return true;
}

namespace {
PavedArea PaveTogether (const std::vector<Path>& paths, double step, bool surface, PathOutlineCache* outlines)
{
    PavedArea out;
    // A gesture may coarsen the interior mesh, never the footprint it fills.
    // Hover/range outlines use 0.5 m: keep the same boundary at every coarser
    // surface quality, including the short-leg joins whose untangling needs it.
    const double outlineStep = step > 0.0 ? std::min (step, 0.5) : 0.5;
    std::vector<Source> sources;
    cp::PathsD united;
    for (const auto& path : paths) {
        Source s;
        s.path = &path;
        std::string error;
        if (!ValidatePath (path, s.geometry, error))
            continue;
        if (outlines != nullptr) {
            const PathOutline* cached = outlines->Get (path, outlineStep);
            if (cached == nullptr)
                continue;
            s.outline = *cached;
        }
        else if (!Outline (path, outlineStep, s.outline, error))
            continue;
        if (s.outline.left.size () < 2)
            continue;
        // Its own outline, clean: a tight bend's fold resolved.
        const cp::PathsD own = cp::Union (cp::PathsD { Polygon (s.outline) }, cp::FillRule::NonZero, kPrecision);
        united.insert (united.end (), own.begin (), own.end ());
        Bound (s);
        sources.push_back (std::move (s));
    }
    if (united.empty ())
        return out;
    std::sort (sources.begin (), sources.end (),
               [] (const Source& a, const Source& b) { return a.path->id < b.path->id; });
    for (const auto& j : FindJunctions (paths)) {
        const Path &a = paths[j.a], &b = paths[j.b];
        if (a.sections.empty () || b.sections.empty ())
            continue;
        AlignmentGeometry ga, gb;
        Placement fa, fb;
        std::string error;
        if (!BuildAlignment (a.points, ga, error) || !BuildAlignment (b.points, gb, error) ||
            !Locate (ga, j.stationA, fa) || !Locate (gb, j.stationB, fb))
            continue;
        const Section sa = SectionIn (a.sections, ga.length, j.stationA);
        const Section sb = SectionIn (b.sections, gb.length, j.stationB);
        const double sine = std::abs (fa.tangent.x * fb.tangent.y - fa.tangent.y * fb.tangent.x);
        const double widths = std::max (sa.widthLeft, sa.widthRight) + std::max (sb.widthLeft, sb.widthRight);
        const double reach = sine > 0.05 ? widths / sine : widths;
        double radius = std::max (a.edgeRadius, b.edgeRadius);
        for (const auto* path : { &a, &b })
            for (const auto& k : path->kerbs)
                if (k.other == (path->id == a.id ? b.id : a.id))
                    radius = std::max (radius, k.radius);
        for (auto& source : sources)
            if (source.path->id == a.id || source.path->id == b.id)
                source.junctions.push_back ({ j.at, reach + radius });
    }
    for (auto& source : sources)
        BoundJunctions (source);
    // One area: crossings and touching ends made junctions; then every corner its own rounding.
    united = cp::Union (united, cp::FillRule::NonZero, kPrecision);
    // Remove collinear sampling noise before choosing tangent directions/extents.
    united = cp::SimplifyPaths (united, kFilletSettings.chordError * .1, true);
    out.corners = FindCorners (united, sources);
    cp::PathsD rounded;
    if (Round (united, out, rounded))
        united = std::move (rounded);
    else {
        out.warning = "Kerb fillets refused: " + out.warning;
        out.boundary = {};
        for (auto& corner : out.corners)
            corner.key.radius = corner.maxRadius = 0;
    }
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
    if (sources.size () == 1 && BuildLinearSurface (out, *sources[0].path, sources[0].geometry))
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
        // Boolean outlines use a binary lattice. The decimal triangulation
        // overload rounds that boundary a second time, moving slanted edges;
        // refinement then exposes triangles outside the original footprint.
        // Triangulate the existing lattice without changing its plan vertices.
        const double lattice = kLattice;
        cp::Paths64 grid;
        for (const auto& ring : region) {
            cp::Path64 path;
            for (const auto& q : ring)
                path.push_back ({ int64_t (std::llround (q.x * lattice)), int64_t (std::llround (q.y * lattice)) });
            grid.push_back (std::move (path));
        }
        cp::Paths64 triangles;
        if (cp::Triangulate (grid, triangles, true) != cp::TriangulateResult::success)
            continue; // a refused region stays outlined, unfilled
        for (auto& t : triangles) {
            if (t.size () != 3)
                continue;
            if (cp::Area (t) < 0.0)
                std::reverse (t.begin (), t.end ());
            for (const auto& q : t) {
                out.triangles.push_back (static_cast<uint32_t> (out.vertices.size ()));
                out.vertices.push_back (lift ({ double (q.x) / lattice, double (q.y) / lattice }));
            }
        }
    }
    std::vector<Vec3> samples;
    for (const auto& source : sources) {
        const auto sample = [&] (double station) {
            StationFrame frame;
            if (Evaluate (*source.path, source.geometry, station, frame))
                samples.insert (samples.end (), { frame.centre, frame.leftEdge, frame.rightEdge });
        };
        // Insert semantic features, not every collinear drawing sample: inserting
        // hundreds of points into long triangle fans creates unnecessary slivers.
        for (const auto& b : source.path->profile) {
            sample (b.station);
            if (b.curve > 0) {
                sample (b.station - b.curve * 0.5);
                sample (b.station + b.curve * 0.5);
            }
        }
        for (const auto& section : source.path->sections)
            sample (section.station);
        for (double station : source.geometry.pointStations)
            sample (station);
    }
    RefinePavedSurface (out, samples, step, [&] (Vec3 p) { return HeightAt (sources, p); });
    return out;
}
} // namespace

PavedArea Pave (const std::vector<Path>& paths, double step, bool surface, PathOutlineCache* outlines)
{
    // Each editor layer is paved on its own: paths on different layers are not united, and
    // their surfaces overlap where they cross rather than merge.
    std::map<Id, std::vector<Path>> layers;
    for (const auto& p : paths)
        layers[p.member.layer].push_back (p);
    if (layers.size () <= 1)
        return PaveTogether (paths, step, surface, outlines);
    PavedArea out;
    for (const auto& [layer, group] : layers) {
        PavedArea one = PaveTogether (group, step, surface, outlines);
        const auto base = static_cast<uint32_t> (out.vertices.size ());
        out.vertices.insert (out.vertices.end (), one.vertices.begin (), one.vertices.end ());
        for (const uint32_t i : one.triangles)
            out.triangles.push_back (base + i);
        out.rings.insert (out.rings.end (), one.rings.begin (), one.rings.end ());
        out.corners.insert (out.corners.end (), one.corners.begin (), one.corners.end ());
        out.boundary.rings.insert (out.boundary.rings.end (), one.boundary.rings.begin (), one.boundary.rings.end ());
        if (!one.warning.empty ())
            out.warning += (out.warning.empty () ? "" : " ") + one.warning;
    }
    return out;
}

double PreviewStep (const std::vector<Path>& paths, size_t triangles, double finest, double coarsest)
{
    double area = 0.0;
    for (const auto& path : paths) {
        AlignmentGeometry g;
        std::string error;
        if (!ValidatePath (path, g, error))
            continue;
        // Each range's mean width over its length (a taper's is the mean of its two ends).
        for (size_t i = 0; i < path.sections.size (); ++i) {
            const Section& r = path.sections[i];
            const double end = i + 1 < path.sections.size () ? path.sections[i + 1].station : g.length;
            const double left = r.taper ? 0.5 * (r.widthLeft + r.endLeft) : r.widthLeft;
            const double right = r.taper ? 0.5 * (r.widthRight + r.endRight) : r.widthRight;
            area += std::max (0.0, end - r.station) * (left + right);
        }
    }
    if (!(area > 0.0) || triangles == 0)
        return finest;
    // On a half-metre ladder, so a drag that lengthens a path does not change the step and
    // reshape the whole preview at every move.
    const double step = std::ceil (2.0 * std::sqrt (kPreviewDensity * area / double (triangles))) * 0.5;
    return std::clamp (step, finest, std::max (finest, coarsest));
}

std::vector<StationSpan> PathAnnotationSpans (const std::vector<Path>& paths, Id id, double from, double to)
{
    std::vector<StationSpan> spans;
    if (!(to > from))
        return spans;
    spans.push_back ({ from, to });
    const auto exclude = [&] (double lo, double hi) {
        std::vector<StationSpan> kept;
        for (const auto& span : spans) {
            if (span.to <= lo || span.from >= hi)
                kept.push_back (span);
            else {
                if (span.from < lo)
                    kept.push_back ({ span.from, lo });
                if (span.to > hi)
                    kept.push_back ({ hi, span.to });
            }
        }
        spans = std::move (kept);
    };
    for (const auto& path : paths) {
        if (path.id != id)
            continue;
        AlignmentGeometry geometry;
        std::string error;
        if (!BuildAlignment (path.points, geometry, error))
            continue;
        // An annotation and its full arrow need a clear run, not a midpoint
        // directly on a control/grade handle or across a sharp direction change.
        const auto avoid = [&] (double station) {
            const Section section = SectionIn (path.sections, geometry.length, station);
            const double gap = std::max (0.25, 0.5 * std::max (section.widthLeft, section.widthRight));
            exclude (station - gap, station + gap);
        };
        for (double station : geometry.pointStations)
            avoid (station);
        for (const auto& grade : path.profile)
            avoid (grade.station);
    }
    for (const auto& j : FindJunctions (paths)) {
        const Path &a = paths[j.a], &b = paths[j.b];
        if (a.id != id && b.id != id)
            continue;
        const bool ownA = a.id == id;
        const double station = ownA ? j.stationA : j.stationB;
        AlignmentGeometry ga, gb;
        Placement fa, fb;
        std::string error;
        if (!BuildAlignment (a.points, ga, error) || !BuildAlignment (b.points, gb, error) ||
            !Locate (ga, j.stationA, fa) || !Locate (gb, j.stationB, fb))
            continue;
        if (a.sections.empty () || b.sections.empty ())
            continue;
        const Section sa = SectionIn (a.sections, ga.length, j.stationA);
        const Section sb = SectionIn (b.sections, gb.length, j.stationB);
        const double wa = std::max (sa.widthLeft, sa.widthRight), wb = std::max (sb.widthLeft, sb.widthRight);
        const double sine = std::abs (fa.tangent.x * fb.tangent.y - fa.tangent.y * fb.tangent.x);
        const double cosine = std::abs (Dot (fa.tangent, fb.tangent));
        const double reach = sine > 1e-3 ? ((ownA ? wb : wa) + (ownA ? wa : wb) * cosine) / sine : wa + wb;
        double radius = std::max (a.edgeRadius, b.edgeRadius);
        for (const auto& path : { &a, &b })
            for (const auto& k : path->kerbs)
                if (k.other == (path->id == a.id ? b.id : a.id))
                    radius = std::max (radius, k.radius);
        const double lo = station - reach - radius - 0.1, hi = station + reach + radius + 0.1;
        exclude (lo, hi);
    }
    return spans;
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
