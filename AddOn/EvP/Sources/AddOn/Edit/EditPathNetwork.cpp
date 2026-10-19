#include "Edit/EditPathNetwork.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPathCrossing.hpp"
#include "Edit/EditPathSurface.hpp"
#include <clipper2/clipper.h>
#include <clipper2/clipper.triangulation.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace geomsrv::edit {

namespace {

namespace cp = Clipper2Lib;

constexpr int kPrecision = 3;          // millimetres
constexpr double kArcTolerance = 0.01; // metres a rounding may stray from the true arc
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
};

// The height at a plan point: the path it lies in (or nearest, at a junction's rounding).
double HeightAt (const std::vector<Source>& sources, Vec3 p)
{
    double best = std::numeric_limits<double>::infinity (), z = 0.0;
    double weighted = 0, weights = 0, blend = 0;
    for (const auto& s : sources) {
        double h = 0.0, beyond = 0.0;
        if (!SurfaceHeight (*s.path, s.geometry, p, h, beyond))
            continue;
        if (beyond < best)
            best = beyond, z = h;
        double influence = 0;
        for (const auto& [at, radius] : s.junctions) {
            const double t = std::clamp (2.0 - DistanceXY (p, at) / radius, 0.0, 1.0);
            influence = std::max (influence, t * t * (3 - 2 * t));
        }
        if (influence <= 0)
            continue;
        double station = 0, offset = 0;
        Placement frame;
        if (!StationOf (s.geometry, p, station, offset) || !Locate (s.geometry, station, frame))
            continue;
        const auto* section = SectionAt (s.path->sections, station);
        const double width = std::max (section->widthLeft, section->widthRight);
        const double distance = DistanceXY (p, frame.point) / width;
        const double weight = influence / std::pow (1 + distance * distance, 2);
        weighted += weight * h;
        weights += weight;
        blend = std::max (blend, influence);
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

// The narrowest half width of the range in force at `station`: a rounding is cut no deeper
// than the width it sits in, not the path's narrowest width somewhere else along it.
double LocalHalf (const Path& path, double station)
{
    const Section* section = SectionAt (path.sections, station);
    return section == nullptr ? 0.0 : std::min (section->widthLeft, section->widthRight);
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
        // How far along the outline each leg runs to the next corner: a corner's rounding keeps
        // to its own half of each leg, so it never reaches into a neighbour's.
        std::vector<double> run (n + 1, 0.0);
        for (size_t k = 0; k < n; ++k)
            run[k + 1] = run[k] + std::hypot (ring[(k + 1) % n].x - ring[k].x, ring[(k + 1) % n].y - ring[k].y);
        const auto leg = [&] (size_t i, int dir) {
            for (size_t k = 1; k < n; ++k) {
                const size_t j = dir > 0 ? (i + k) % n : (i + n - k) % n;
                if (turning[j]) {
                    const double d = dir > 0 ? run[j] - run[i] : run[i] - run[j];
                    return d > 0.0 ? d : d + run[n];
                }
            }
            return run[n] * 0.5;
        };
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
                if (OnOutline (sources[s].outline, v, side, sample, station))
                    owners.push_back ({ s, side, station });
            }
            if (owners.empty ())
                continue;
            double half = std::numeric_limits<double>::infinity ();
            for (const auto& o : owners)
                half = std::min (half, LocalHalf (*sources[o.source].path, o.station));
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
            // The largest radius whose tangent points stay within its half of each leg; an
            // outside corner's also cuts no deeper than most of the half width it sits in.
            const double adjacent = std::min (std::hypot (ring[(i + n - 1) % n].x - v.x, ring[(i + n - 1) % n].y - v.y),
                                              std::hypot (ring[(i + 1) % n].x - v.x, ring[(i + 1) % n].y - v.y));
            const double legs = 0.49 * std::min ({ leg (i, -1), leg (i, 1), adjacent }) * std::tan (c.half);
            c.maxRadius = std::min (legs, c.convex ? 0.98 * (std::isfinite (half) ? half : 0.0) : kMaxReentrant);
            c.requestedRadius = c.key.radius;
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

// Ordered from the incoming edge's tangent to the outgoing edge's tangent.
// No independent patch edges are introduced along the united boundary.
cp::PathD FilletArc (const PavedCorner& c, Vec3 previous)
{
    const double r = c.key.radius, t = r / std::tan (c.half);
    Vec3 da = Turned (c.bisector, c.half), db = Turned (c.bisector, -c.half);
    if (Dot (da, previous - c.at) < Dot (db, previous - c.at))
        std::swap (da, db);
    const Vec3 a = c.at + da * t, b = c.at + db * t;
    const Vec3 centre = c.at + c.bisector * (r / std::sin (c.half));
    const double from = std::atan2 (a.y - centre.y, a.x - centre.x);
    const double sweep = std::remainder (std::atan2 (b.y - centre.y, b.x - centre.x) - from, 2.0 * kPi);
    const double step = 2.0 * std::acos (std::clamp (1.0 - kArcTolerance / r, -1.0, 1.0));
    const int pieces = std::clamp (int (std::ceil (std::abs (sweep) / std::max (step, 1.0e-3))), 2, 256);
    cp::PathD piece;
    for (int k = 0; k <= pieces; ++k) {
        const double angle = from + sweep * k / pieces;
        if (k == 0)
            piece.push_back ({ a.x, a.y });
        else if (k == pieces)
            piece.push_back ({ b.x, b.y });
        else
            piece.push_back ({ centre.x + r * std::cos (angle), centre.y + r * std::sin (angle) });
    }
    return piece;
}

// Round only after footprint union. Replacing each ring corner with its arc
// avoids the narrow gaps/islands produced by separately rounded Boolean patches.
cp::PathsD Round (const cp::PathsD& sharp, const std::vector<PavedCorner>& corners)
{
    cp::PathsD rounded;
    bool changed = false;
    for (const auto& ring : sharp) {
        cp::PathD out;
        for (size_t i = 0; i < ring.size (); ++i) {
            const auto found = std::find_if (corners.begin (), corners.end (), [&] (const PavedCorner& c) {
                return c.at.x == ring[i].x && c.at.y == ring[i].y && c.key.radius > 1e-4 && c.half > 1e-3 &&
                       c.half < kPi * .5 - 1e-3;
            });
            if (found == corners.end ()) {
                out.push_back (ring[i]);
                continue;
            }
            const auto& prev = ring[(i + ring.size () - 1) % ring.size ()];
            const auto arc = FilletArc (*found, { prev.x, prev.y, 0 });
            out.insert (out.end (), arc.begin (), arc.end ());
            changed = true;
        }
        rounded.push_back (std::move (out));
    }
    return changed ? cp::Union (rounded, cp::FillRule::NonZero, kPrecision) : sharp;
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
        // Its own outline, clean: a tight bend's fold resolved.
        const cp::PathsD own = cp::Union (cp::PathsD { Polygon (s.outline) }, cp::FillRule::NonZero, kPrecision);
        united.insert (united.end (), own.begin (), own.end ());
        sources.push_back (std::move (s));
    }
    if (united.empty ())
        return out;
    std::sort (sources.begin (), sources.end (),
               [] (const Source& a, const Source& b) { return a.path->id < b.path->id; });
    for (const auto& j : FindJunctions (paths)) {
        const Path &a = paths[j.a], &b = paths[j.b];
        const auto* sa = SectionAt (a.sections, j.stationA);
        const auto* sb = SectionAt (b.sections, j.stationB);
        if (!sa || !sb)
            continue;
        AlignmentGeometry ga, gb;
        Placement fa, fb;
        std::string error;
        if (!BuildAlignment (a.points, ga, error) || !BuildAlignment (b.points, gb, error) ||
            !Locate (ga, j.stationA, fa) || !Locate (gb, j.stationB, fb))
            continue;
        const double sine = std::abs (fa.tangent.x * fb.tangent.y - fa.tangent.y * fb.tangent.x);
        const double widths = std::max (sa->widthLeft, sa->widthRight) + std::max (sb->widthLeft, sb->widthRight);
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
    // One area: crossings and touching ends made junctions; then every corner its own rounding.
    united = cp::Union (united, cp::FillRule::NonZero, kPrecision);
    // Remove collinear sampling noise before choosing tangent directions/extents.
    united = cp::SimplifyPaths (united, 0.001, true);
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
        const double lattice = std::ldexp (1.0, std::ilogb (std::pow (10.0, kPrecision)) + 1);
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

std::vector<StationSpan> PathAnnotationSpans (const std::vector<Path>& paths, Id id, double from, double to)
{
    std::vector<StationSpan> spans;
    if (!(to > from))
        return spans;
    spans.push_back ({ from, to });
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
        const auto* sa = SectionAt (a.sections, j.stationA);
        const auto* sb = SectionAt (b.sections, j.stationB);
        if (!sa || !sb)
            continue;
        const double wa = std::max (sa->widthLeft, sa->widthRight), wb = std::max (sb->widthLeft, sb->widthRight);
        const double sine = std::abs (fa.tangent.x * fb.tangent.y - fa.tangent.y * fb.tangent.x);
        const double cosine = std::abs (Dot (fa.tangent, fb.tangent));
        const double reach = sine > 1e-3 ? ((ownA ? wb : wa) + (ownA ? wa : wb) * cosine) / sine : wa + wb;
        double radius = std::max (a.edgeRadius, b.edgeRadius);
        for (const auto& path : { &a, &b })
            for (const auto& k : path->kerbs)
                if (k.other == (path->id == a.id ? b.id : a.id))
                    radius = std::max (radius, k.radius);
        const double lo = station - reach - radius - 0.1, hi = station + reach + radius + 0.1;
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
