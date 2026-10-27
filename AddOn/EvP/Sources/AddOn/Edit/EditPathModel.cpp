#include "Edit/EditPath.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::edit {

namespace {

constexpr double kMiterLimit = 4.0; // a sharper corner's outer edge is bevelled, as a kerb is cut
constexpr size_t kUntangleWindow = 400;

Vec3 LeftOf (Vec3 d)
{
    return { -d.y, d.x, 0.0 };
}

// Where segments a-b and c-d cross in plan, strictly inside both; `u` is along a-b.
bool Crossing (Vec3 a, Vec3 b, Vec3 c, Vec3 d, double& u)
{
    const double rx = b.x - a.x, ry = b.y - a.y, sx = d.x - c.x, sy = d.y - c.y;
    const double den = rx * sy - ry * sx;
    if (std::abs (den) < 1.0e-14 || rx * rx + ry * ry < 1.0e-18 || sx * sx + sy * sy < 1.0e-18)
        return false;
    const double qx = c.x - a.x, qy = c.y - a.y;
    u = (qx * sy - qy * sx) / den;
    const double v = (qx * ry - qy * rx) / den;
    constexpr double e = 1.0e-9;
    return u > e && u < 1.0 - e && v > e && v < 1.0 - e;
}

// An edge offset further than a bend's radius folds back over itself. The fold is cut
// out where the edge crosses itself: the points inside the loop collapse onto the
// crossing, so the edge stays one sample per station and the strip between the edges
// keeps its pairs.
void Untangle (std::vector<Vec3>& edge)
{
    const size_t n = edge.size ();
    for (size_t i = 0; i + 3 < n; ++i)
        for (size_t j = i + 2; j + 1 < n && j <= i + kUntangleWindow; ++j) {
            double u = 0.0;
            if (!Crossing (edge[i], edge[i + 1], edge[j], edge[j + 1], u))
                continue;
            const Vec3 at = Lerp (edge[i], edge[i + 1], u);
            for (size_t k = i + 1; k <= j; ++k)
                edge[k] = at;
            i = j - 1;
            break;
        }
}

// Two lines meeting without an arc: each edge turns where its two offset lines meet.
// The inner edge stops at that corner -- the samples of either leg that would run past
// it collapse onto it -- and the outer edge is mitred, or bevelled past the limit by a
// second sample at the same station.
void SharpCorners (const Path& path, const AlignmentGeometry& g, PathOutline& o)
{
    for (size_t k = 0; k + 1 < g.elements.size (); ++k) {
        const AlignmentElement &e0 = g.elements[k], &e1 = g.elements[k + 1];
        Vec3 d0, d1;
        if (e0.kind != ElementKind::Line || e1.kind != ElementKind::Line || !Normalize (e0.b - e0.a, d0) ||
            !Normalize (e1.b - e1.a, d1))
            continue;
        const double c = std::clamp (Dot (d0, d1), -1.0, 1.0);
        if (c > 1.0 - 1.0e-12)
            continue;
        const double half = std::acos (c) * 0.5;
        const bool leftTurn = d0.x * d1.y - d0.y * d1.x > 0.0;
        Vec3 bisector;
        if (!Normalize (LeftOf (d0) + LeftOf (d1), bisector))
            continue;
        const double corner = e1.start, miter = 1.0 / std::cos (half), trim = std::tan (half);
        size_t at = o.stations.size ();
        for (size_t i = 0; i < o.stations.size (); ++i)
            if (std::abs (o.stations[i] - corner) < 1.0e-6) {
                at = i;
                break;
            }
        StationFrame f;
        if (at == o.stations.size () || !Evaluate (path, g, corner, f))
            continue;
        double dropLeft = 0.0, dropRight = 0.0;
        EdgeDrops (f.section, dropLeft, dropRight);
        const Vec3 leftMiter = f.centre + bisector * (f.section.widthLeft * miter) + Vec3 { 0.0, 0.0, dropLeft };
        const Vec3 rightMiter = f.centre - bisector * (f.section.widthRight * miter) + Vec3 { 0.0, 0.0, dropRight };
        auto& inner = leftTurn ? o.left : o.right;
        const double innerWidth = leftTurn ? f.section.widthLeft : f.section.widthRight;
        // A corner whose inner offsets meet beyond either leg folds the path onto itself;
        // its inner edge keeps the legs' own offsets rather than a far-off corner.
        const bool meets = innerWidth * trim <= std::min (e0.length, e1.length) + 1.0e-9;
        const Vec3 innerCorner = !meets ? inner[at] : leftTurn ? leftMiter : rightMiter;
        if (meets)
            for (size_t i = 0; i < o.stations.size (); ++i)
                if (std::abs (o.stations[i] - corner) < innerWidth * trim)
                    inner[i] = innerCorner;
        if (miter <= kMiterLimit) {
            (leftTurn ? o.right : o.left)[at] = leftTurn ? rightMiter : leftMiter;
            continue;
        }
        // The bevel: this sample keeps the first leg's outer offset, a twin takes the second's.
        const double outerWidth = leftTurn ? f.section.widthRight : f.section.widthLeft;
        const double outerDrop = leftTurn ? dropRight : dropLeft;
        const Vec3 side = leftTurn ? LeftOf (d1) * -1.0 : LeftOf (d1);
        const Vec3 twin = f.centre + side * outerWidth + Vec3 { 0.0, 0.0, outerDrop };
        const auto pos = static_cast<std::ptrdiff_t> (at + 1);
        o.stations.insert (o.stations.begin () + pos, corner);
        o.centre.insert (o.centre.begin () + pos, o.centre[at]);
        o.range.insert (o.range.begin () + pos, o.range[at]);
        o.left.insert (o.left.begin () + pos, leftTurn ? innerCorner : twin);
        o.right.insert (o.right.begin () + pos, leftTurn ? twin : innerCorner);
    }
}

} // namespace

bool ValidatePath (const Path& path, AlignmentGeometry& geometry, std::string& error)
{
    if (!(path.edgeRadius >= 0.0) || !std::isfinite (path.edgeRadius)) {
        error = "an edge radius cannot be negative";
        return false;
    }
    for (const auto& k : path.kerbs)
        if (!(k.radius >= 0.0) || !std::isfinite (k.radius)) {
            error = "a kerb radius cannot be negative";
            return false;
        }
    if (!BuildAlignment (path.points, geometry, error))
        return false;
    if (!ValidateProfile (path.profile, geometry.length, error))
        return false;
    return ValidateSections (path.sections, geometry.length, error);
}

bool Evaluate (const Path& path, const AlignmentGeometry& g, double station, StationFrame& out)
{
    Placement at;
    double z = 0.0, grade = 0.0;
    if (path.sections.empty () || !Locate (g, station, at) || !Elevation (path.profile, station, z, &grade))
        return false;
    const Section section = SectionIn (path.sections, g.length, station);
    out.station = station;
    out.centre = { at.point.x, at.point.y, z };
    out.tangent = at.tangent;
    out.left = at.left;
    out.curvature = at.curvature;
    out.grade = grade;
    out.section = section;
    double dropLeft = 0.0, dropRight = 0.0;
    EdgeDrops (section, dropLeft, dropRight);
    out.leftEdge = out.centre + at.left * section.widthLeft + Vec3 { 0.0, 0.0, dropLeft };
    out.rightEdge = out.centre - at.left * section.widthRight + Vec3 { 0.0, 0.0, dropRight };
    return true;
}

bool Outline (const Path& path, double maxStep, PathOutline& out, std::string& error)
{
    out = {};
    AlignmentGeometry g;
    if (!ValidatePath (path, g, error))
        return false;
    std::vector<double> stations = SampleStations (path, g, maxStep);
    // Mesh spacing may grow during a drag; it must not turn an offset arc into a
    // long chord (or a false sharp junction). Bound both offset edges to 1 mm,
    // using the outer radius, independently of the surface refinement budget.
    double width = 0.0;
    for (const auto& section : path.sections)
        width = std::max ({ width, section.widthLeft, section.widthRight, section.taper ? section.endLeft : 0.0,
                            section.taper ? section.endRight : 0.0 });
    for (const auto& element : g.elements) {
        if (element.kind != ElementKind::Arc)
            continue;
        const double angle = 4.0 * std::asin (std::sqrt (std::min (0.5, 0.001 / (2.0 * (element.radius + width)))));
        const double count = std::ceil (std::abs (element.sweep) / angle);
        if (!(count <= 4096.0) || double (stations.size ()) + count > 250000.0) {
            error = "path offset arcs exceed the tessellation work limit";
            return false;
        }
        const auto pieces = static_cast<size_t> (count);
        for (size_t i = 1; i < pieces; ++i)
            stations.push_back (element.start + element.length * double (i) / double (pieces));
    }
    for (size_t i = 1; i < path.sections.size (); ++i)
        if (!path.sections[i - 1].taper)
            stations.push_back (std::max (0.0, path.sections[i].station - 1.0e-3));
    std::sort (stations.begin (), stations.end ());
    stations.erase (
        std::unique (stations.begin (), stations.end (), [] (double a, double b) { return b - a < 1.0e-9; }),
        stations.end ());
    for (double s : stations) {
        StationFrame f;
        if (!Evaluate (path, g, s, f))
            continue;
        out.stations.push_back (s);
        out.centre.push_back (f.centre);
        out.left.push_back (f.leftEdge);
        out.right.push_back (f.rightEdge);
        out.range.push_back (f.section.id);
    }
    SharpCorners (path, g, out);
    Untangle (out.left);
    Untangle (out.right);
    // A bend's control point is its corner, where the legs meet, not its arc's middle.
    for (size_t i = 0; i < path.points.size () && i < g.pointStations.size (); ++i) {
        StationFrame f;
        if (Evaluate (path, g, g.pointStations[i], f))
            out.points.push_back ({ path.points[i].p.x, path.points[i].p.y, f.centre.z });
    }
    for (const auto& b : path.profile) {
        StationFrame f;
        if (Evaluate (path, g, b.station, f))
            out.breaks.push_back ({ f.centre.x, f.centre.y, b.z });
    }
    return true;
}

std::vector<double> SampleStations (const Path& path, const AlignmentGeometry& g, double maxStep)
{
    std::vector<double> s = ElementStations (g);
    for (const auto& b : path.profile) {
        s.push_back (b.station);
        if (b.curve > 0.0) {
            s.push_back (b.station - b.curve * 0.5);
            s.push_back (b.station + b.curve * 0.5);
        }
    }
    for (const auto& r : path.sections)
        s.push_back (r.station);
    s.push_back (0.0);
    s.push_back (g.length);
    for (auto& x : s)
        x = std::clamp (x, 0.0, g.length);
    std::sort (s.begin (), s.end ());
    s.erase (std::unique (s.begin (), s.end (), [] (double a, double b) { return b - a < 1.0e-6; }), s.end ());
    if (!(maxStep > 0.0))
        return s;
    std::vector<double> out;
    for (size_t i = 0; i + 1 < s.size (); ++i) {
        const int pieces = std::max (1, static_cast<int> (std::ceil ((s[i + 1] - s[i]) / maxStep - 1.0e-9)));
        for (int k = 0; k < pieces; ++k)
            out.push_back (s[i] + (s[i + 1] - s[i]) * k / pieces);
    }
    out.push_back (s.back ());
    return out;
}

} // namespace geomsrv::edit
