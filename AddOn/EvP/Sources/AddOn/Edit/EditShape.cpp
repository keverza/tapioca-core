#include "Edit/EditShape.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace geomsrv::edit {

namespace {
constexpr double kStraight = 1.0e-6; // legs this close to opposite run straight on: no corner
} // namespace

bool CornerOf (const Entity& e, size_t i, CornerGeometry& c)
{
    const size_t n = e.vertices.size ();
    if (n < 3 || i >= n || (!e.closed && (i == 0 || i + 1 >= n)))
        return false;
    c.at = i;
    c.prev = (i + n - 1) % n;
    c.next = (i + 1) % n;
    c.corner = e.vertices[i].p;
    const Vec3 a = e.vertices[c.prev].p - c.corner, b = e.vertices[c.next].p - c.corner;
    c.legPrev = std::hypot (a.x, a.y);
    c.legNext = std::hypot (b.x, b.y);
    if (c.legPrev < kMinSegment || c.legNext < kMinSegment)
        return false;
    c.toPrev = { a.x / c.legPrev, a.y / c.legPrev, 0.0 };
    c.toNext = { b.x / c.legNext, b.y / c.legNext, 0.0 };
    const double angle = std::acos (std::clamp (Dot (c.toPrev, c.toNext), -1.0, 1.0));
    if (angle > 3.14159265358979323846 - kStraight || angle < kStraight)
        return false;
    c.half = angle * 0.5;
    return Normalize (c.toPrev + c.toNext, c.bisector);
}

double TangentLength (const CornerGeometry& c, double radius)
{
    return radius / std::tan (c.half);
}

double MaxRadius (const Entity& e, const std::vector<size_t>& set)
{
    if (set.empty ())
        return 0.0;
    const auto inSet = [&] (size_t i) { return std::find (set.begin (), set.end (), i) != set.end (); };
    double best = std::numeric_limits<double>::infinity ();
    for (size_t i : set) {
        CornerGeometry c;
        if (!CornerOf (e, i, c))
            return 0.0;
        const double cot = 1.0 / std::tan (c.half);
        // Each leg: this rounding and the neighbour's -- the same radius when it is in the
        // set too, its own otherwise.
        for (const auto [other, leg] : { std::pair { c.prev, c.legPrev }, std::pair { c.next, c.legNext } }) {
            CornerGeometry o;
            const bool corner = CornerOf (e, other, o);
            if (corner && inSet (other))
                best = std::min (best, leg / (cot + 1.0 / std::tan (o.half)));
            else {
                const double taken = corner ? TangentLength (o, e.vertices[other].radius) : 0.0;
                best = std::min (best, std::max (0.0, leg - taken) / cot);
            }
        }
    }
    return std::isfinite (best) ? best : 0.0;
}

double DrawnRadius (const Entity& e, size_t i)
{
    CornerGeometry c;
    if (i >= e.vertices.size () || !(e.vertices[i].radius > 0.0) || !CornerOf (e, i, c))
        return 0.0;
    double t = TangentLength (c, e.vertices[i].radius);
    for (const auto [other, leg] : { std::pair { c.prev, c.legPrev }, std::pair { c.next, c.legNext } }) {
        CornerGeometry o;
        const double theirs = e.vertices[other].radius > 0.0 && CornerOf (e, other, o)
                                  ? TangentLength (o, e.vertices[other].radius)
                                  : 0.0;
        const double mine = TangentLength (c, e.vertices[i].radius);
        if (mine + theirs > leg)
            t = std::min (t, leg * mine / (mine + theirs)); // both give way in proportion
    }
    return t * std::tan (c.half);
}

std::vector<Vec3> Shape (const Entity& e, double maxAngle)
{
    std::vector<Vec3> out;
    const size_t n = e.vertices.size ();
    if (!(maxAngle > 0.0))
        maxAngle = 0.15;
    for (size_t i = 0; i < n; ++i) {
        const double r = DrawnRadius (e, i);
        CornerGeometry c;
        if (!(r > 0.0) || !CornerOf (e, i, c)) {
            out.push_back (e.vertices[i].p);
            continue;
        }
        const double t = TangentLength (c, r);
        const Vec3 p = c.corner, t1 = p + c.toPrev * t, t2 = p + c.toNext * t;
        const Vec3 centre = p + c.bisector * (r / std::sin (c.half));
        const double a1 = std::atan2 (t1.y - centre.y, t1.x - centre.x);
        double sweep = std::atan2 (t2.y - centre.y, t2.x - centre.x) - a1;
        sweep = std::remainder (sweep, 2.0 * 3.14159265358979323846);
        const int steps = std::max (2, int (std::ceil (std::abs (sweep) / maxAngle)));
        for (int k = 0; k <= steps; ++k) {
            const double a = a1 + sweep * k / steps;
            out.push_back ({ centre.x + r * std::cos (a), centre.y + r * std::sin (a), p.z });
        }
        out[out.size () - steps - 1] = { t1.x, t1.y, p.z };
        out.back () = { t2.x, t2.y, p.z };
    }
    return out;
}

Vec3 CornerGripAt (const Entity& e, size_t i, double minInset)
{
    CornerGeometry c;
    if (!CornerOf (e, i, c))
        return i < e.vertices.size () ? e.vertices[i].p : Vec3 {};
    const double r = DrawnRadius (e, i);
    const double inset = r > 0.0 ? r / std::sin (c.half) - r : 0.0;
    return c.corner + c.bisector * std::max (inset, minInset);
}

double RadiusFromGrip (const Entity& e, size_t i, Vec3 p, double minInset)
{
    CornerGeometry c;
    if (!CornerOf (e, i, c))
        return 0.0;
    const double d = Dot (Vec3 { p.x - c.corner.x, p.y - c.corner.y, 0.0 }, c.bisector);
    if (!(d > minInset))
        return 0.0;
    return d / (1.0 / std::sin (c.half) - 1.0);
}

bool SetCornerRadius (Document& doc, Id entity, const std::vector<Id>& vertices, double radius, std::string& error)
{
    Entity* e = Find (doc, entity);
    if (e == nullptr) {
        error = "that object no longer exists";
        return false;
    }
    if (!(radius >= 0.0) || !std::isfinite (radius)) {
        error = "a corner radius cannot be negative";
        return false;
    }
    std::vector<size_t> set;
    for (Id v : vertices) {
        const int i = VertexIndex (*e, v);
        CornerGeometry c;
        if (i < 0 || !CornerOf (*e, size_t (i), c)) {
            error = "only a corner can be rounded: not an end, and not a straight run";
            return false;
        }
        if (e->vertices[size_t (i)].lockXY) {
            error = "that corner is locked";
            return false;
        }
        set.push_back (size_t (i));
    }
    const double fits = MaxRadius (*e, set);
    if (radius > fits * (1.0 + 1.0e-9) + 1.0e-12) {
        char text[96];
        std::snprintf (text, sizeof text, "the radius does not fit its legs: at most %.3f m", fits);
        error = text;
        return false;
    }
    for (size_t i : set)
        e->vertices[i].radius = radius;
    return true;
}

} // namespace geomsrv::edit
