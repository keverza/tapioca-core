#include "SunStudy/SunStudySolarCut.hpp"

#include "SunStudy/SunStudyPointProfile.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace evp::sunstudy {

namespace {

// ⚠️ THE CUT CLEARS THE SLICE'S RAYS, IT DOES NOT TOUCH THEM. The envelope is
// spanned by the rays themselves, so an exact cut leaves what remains grazing
// every edge ray -- and a re-trace finds the point still blocked. The cutting
// surface is lowered by kSolarCutClearance and the first and last sectors are
// widened by kSolarCutAzimuthMargin, so the sun passes with room to spare.
constexpr double kSolarCutClearance = 0.05;       // metres, vertically
constexpr double kSolarCutAzimuthMargin = 0.0175; // radians, about one degree

using Point = std::array<double, 3>;
using Poly = std::vector<Point>;

struct HalfSpace {
    double n[3] = {};
    double d = 0.0;
    double Eval (const Point& x) const
    {
        return n[0] * x[0] + n[1] * x[1] + n[2] * x[2] + d;
    }
    HalfSpace Negated () const
    {
        return { { -n[0], -n[1], -n[2] }, -d };
    }
};

// One azimuth sector of the envelope: between two vertical half-planes
// through the point, and the plane of the two sun rays that bound it.
struct Sector {
    HalfSpace left, right, above;
    Point sunFrom, sunTo;   // unit, toward the sun
    Point sideFrom, sideTo; // the vertical half-planes' horizontal directions
    Point e1, e2;           // an orthonormal basis of the envelope plane
};

Point Sub (const Point& a, const Point& b)
{
    return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
}

Point Cross (const Point& a, const Point& b)
{
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}

double Dot (const Point& a, const Point& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

bool Normalise (Point& v)
{
    const double length = std::sqrt (Dot (v, v));
    if (!(length > 1.0e-12) || !std::isfinite (length))
        return false;
    for (double& c : v)
        c /= length;
    return true;
}

// Sutherland-Hodgman against one half-space, keeping f >= 0. A convex input
// stays convex and keeps its winding.
Poly Clip (const Poly& in, const HalfSpace& h)
{
    Poly out;
    const size_t n = in.size ();
    if (n < 3)
        return out;
    out.reserve (n + 2);
    for (size_t i = 0; i < n; ++i) {
        const Point& a = in[i];
        const Point& b = in[(i + 1) % n];
        const double fa = h.Eval (a), fb = h.Eval (b);
        if (fa >= 0.0)
            out.push_back (a);
        if ((fa >= 0.0) != (fb >= 0.0)) {
            const double t = fa / (fa - fb);
            out.push_back ({ a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), a[2] + t * (b[2] - a[2]) });
        }
    }
    return out;
}

double Area2 (const Poly& poly)
{
    Point sum { 0.0, 0.0, 0.0 };
    for (size_t i = 1; i + 1 < poly.size (); ++i) {
        const Point c = Cross (Sub (poly[i], poly[0]), Sub (poly[i + 1], poly[0]));
        for (int k = 0; k < 3; ++k)
            sum[size_t (k)] += c[size_t (k)];
    }
    return std::sqrt (Dot (sum, sum));
}

// The polygon's normal from every corner, never from its first three: a band
// that narrows to a point repeats a corner, and three corners then span nothing.
Point FaceNormal (const Poly& poly)
{
    Point sum { 0.0, 0.0, 0.0 };
    for (size_t i = 1; i + 1 < poly.size (); ++i) {
        const Point c = Cross (Sub (poly[i], poly[0]), Sub (poly[i + 1], poly[0]));
        for (int k = 0; k < 3; ++k)
            sum[size_t (k)] += c[size_t (k)];
    }
    return sum;
}

bool Valid (const Poly& poly)
{
    return poly.size () >= 3 && Area2 (poly) > 1.0e-10;
}

// Fan triangles of a convex polygon, optionally reversed.
void Emit (const Poly& poly, uint32_t mesh, bool reverse, std::vector<SolarCutTriangle>& out)
{
    for (size_t i = 1; i + 1 < poly.size (); ++i) {
        SolarCutTriangle triangle;
        triangle.mesh = mesh;
        const Point* corners[3] = { &poly[0], &poly[reverse ? i + 1 : i], &poly[reverse ? i : i + 1] };
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                triangle.v[c][k] = (*corners[c])[size_t (k)];
        out.push_back (triangle);
    }
}

double SignedVolume (const SolarCutTriangle& t, const double origin[3])
{
    const Point a { t.v[0][0] - origin[0], t.v[0][1] - origin[1], t.v[0][2] - origin[2] };
    const Point b { t.v[1][0] - origin[0], t.v[1][1] - origin[1], t.v[1][2] - origin[2] };
    const Point c { t.v[2][0] - origin[0], t.v[2][1] - origin[1], t.v[2][2] - origin[2] };
    return Dot (a, Cross (b, c)) / 6.0;
}

bool Corners (const geomsrv::Mesh& mesh, size_t t, Poly& poly)
{
    poly.assign (3, Point {});
    for (int c = 0; c < 3; ++c) {
        const size_t index = 3ull * mesh.triangles[3 * t + size_t (c)];
        if (index + 2 >= mesh.vertices.size ())
            return false;
        for (int k = 0; k < 3; ++k)
            poly[size_t (c)][size_t (k)] = mesh.vertices[index + size_t (k)];
    }
    return true;
}

// The envelope plane's section through a closed mesh, filled: the mesh's
// crossing segments in the plane's 2D frame, cut into horizontal bands at
// every segment end, paired inside each band (even-odd), and each trapezoid
// clipped to the sector. False when a band has an odd crossing count (an open
// mesh): no cap is invented for it.
bool Cap (const geomsrv::Mesh& mesh, const HalfSpace& plane, const Point& e1, const Point& e2,
          const std::vector<HalfSpace>& clips, const Point& origin, Poly& scratch, std::vector<Poly>& caps)
{
    struct Segment {
        double u0, v0, u1, v1;
    };
    std::vector<Segment> segments;
    std::vector<double> breaks;
    for (size_t t = 0; t < mesh.TriangleCount (); ++t) {
        if (!Corners (mesh, t, scratch))
            continue;
        double f[3];
        for (int c = 0; c < 3; ++c)
            f[c] = plane.Eval (scratch[size_t (c)]);
        Point hits[2];
        int count = 0;
        for (int c = 0; c < 3 && count < 2; ++c) {
            const Point& a = scratch[size_t (c)];
            const Point& b = scratch[size_t ((c + 1) % 3)];
            const double fa = f[c], fb = f[(c + 1) % 3];
            if ((fa >= 0.0) != (fb >= 0.0)) {
                const double s = fa / (fa - fb);
                hits[count++] = { a[0] + s * (b[0] - a[0]), a[1] + s * (b[1] - a[1]), a[2] + s * (b[2] - a[2]) };
            }
        }
        if (count != 2)
            continue;
        const Point r0 = Sub (hits[0], origin), r1 = Sub (hits[1], origin);
        Segment segment { Dot (r0, e1), Dot (r0, e2), Dot (r1, e1), Dot (r1, e2) };
        if (std::abs (segment.v1 - segment.v0) < 1.0e-12)
            continue; // parallel to the bands: it bounds none of them
        segments.push_back (segment);
        breaks.push_back (segment.v0);
        breaks.push_back (segment.v1);
    }
    if (segments.empty ())
        return true;
    std::sort (breaks.begin (), breaks.end ());
    breaks.erase (
        std::unique (breaks.begin (), breaks.end (), [] (double a, double b) { return std::abs (a - b) < 1.0e-9; }),
        breaks.end ());
    const auto at = [&] (double u, double v) {
        return Point { origin[0] + u * e1[0] + v * e2[0], origin[1] + u * e1[1] + v * e2[1],
                       origin[2] + u * e1[2] + v * e2[2] };
    };
    bool closed = true;
    struct Active {
        double atMid, at0, at1;
    };
    std::vector<Active> active;
    for (size_t b = 0; b + 1 < breaks.size (); ++b) {
        const double v0 = breaks[b], v1 = breaks[b + 1], vm = 0.5 * (v0 + v1);
        active.clear ();
        for (const Segment& s : segments) {
            const double lo = (std::min) (s.v0, s.v1), hi = (std::max) (s.v0, s.v1);
            if (vm < lo || vm > hi)
                continue;
            const auto uAt = [&s] (double v) { return s.u0 + (s.u1 - s.u0) * (v - s.v0) / (s.v1 - s.v0); };
            active.push_back ({ uAt (vm), uAt (v0), uAt (v1) });
        }
        if (active.empty ())
            continue;
        if ((active.size () & 1) != 0) {
            closed = false;
            continue;
        }
        std::sort (active.begin (), active.end (), [] (const Active& a, const Active& c) { return a.atMid < c.atMid; });
        for (size_t i = 0; i + 1 < active.size (); i += 2) {
            Poly quad { at (active[i].at0, v0), at (active[i + 1].at0, v0), at (active[i + 1].at1, v1),
                        at (active[i].at1, v1) };
            for (const HalfSpace& clip : clips)
                quad = Clip (quad, clip);
            if (Valid (quad))
                caps.push_back (std::move (quad));
        }
    }
    return closed;
}

} // namespace

bool ComputeSolarCut (const geomsrv::Snapshot& snapshot, const SolarCutInput& input, SolarCutResult& result,
                      const std::function<bool (const double direction[3])>& lit,
                      const std::function<bool ()>& cancelled, double reach)
{
    result = SolarCutResult ();
    const Point origin { input.point[0], input.point[1], input.point[2] };
    for (const SunStep& step : input.steps) {
        Point direction { step.direction[0], step.direction[1], step.direction[2] };
        if (!Normalise (direction)) {
            result.error = "a sun direction in the slice is not finite";
            return false;
        }
        if (direction[2] <= 0.0 || !FacesSun (input.normal, direction.data ()))
            continue;
        result.facingHours += input.hoursPerStep;
        if (lit && lit (direction.data ()))
            result.litHours += input.hoursPerStep;
        SunStep facing = step;
        std::copy (direction.begin (), direction.end (), facing.direction);
        result.facingSteps.push_back (facing);
    }
    // The sectors: one per pair of consecutive facing suns.
    std::vector<Sector> sectors;
    for (size_t s = 0; s + 1 < result.facingSteps.size (); ++s) {
        Sector sector;
        for (int k = 0; k < 3; ++k) {
            sector.sunFrom[size_t (k)] = result.facingSteps[s].direction[k];
            sector.sunTo[size_t (k)] = result.facingSteps[s + 1].direction[k];
        }
        Point ui { sector.sunFrom[0], sector.sunFrom[1], 0.0 }, uj { sector.sunTo[0], sector.sunTo[1], 0.0 };
        if (!Normalise (ui) || !Normalise (uj))
            continue;
        const double turnCross = ui[0] * uj[1] - ui[1] * uj[0];
        if (std::abs (turnCross) < 1.0e-9)
            continue; // the same azimuth: a line, not a sector
        const double turn = turnCross > 0.0 ? 1.0 : -1.0;
        // The slice's outer edges, widened: the first sun turned back, the last on.
        const auto rotate = [] (const Point& u, double angle) {
            return Point { u[0] * std::cos (angle) - u[1] * std::sin (angle),
                           u[0] * std::sin (angle) + u[1] * std::cos (angle), 0.0 };
        };
        if (s == 0)
            ui = rotate (ui, -turn * kSolarCutAzimuthMargin);
        if (s + 2 == result.facingSteps.size ())
            uj = rotate (uj, turn * kSolarCutAzimuthMargin);
        sector.sideFrom = ui;
        sector.sideTo = uj;
        Point n = Cross (sector.sunFrom, sector.sunTo);
        if (!Normalise (n))
            continue;
        if (n[2] < 0.0)
            n = { -n[0], -n[1], -n[2] };
        const auto through = [&origin] (const Point& normal) {
            return HalfSpace { { normal[0], normal[1], normal[2] }, -Dot (normal, origin) };
        };
        sector.above = through (n);
        sector.above.d += kSolarCutClearance * n[2]; // lowered: f >= 0 a little under the rays
        sector.left = through ({ -turn * ui[1], turn * ui[0], 0.0 });
        sector.right = through ({ turn * uj[1], -turn * uj[0], 0.0 });
        sector.e1 = sector.sunFrom;
        sector.e2 = Cross (n, sector.e1);
        Normalise (sector.e2);
        sectors.push_back (sector);
    }
    if (sectors.empty ())
        return true; // fewer than two facing suns: a line, nothing to cut

    double farthest = 0.0;
    Poly scratch;
    for (size_t m = 0; m < snapshot.meshes.size (); ++m) {
        if (cancelled && cancelled ())
            return false;
        if (m == input.ownMesh || m >= input.blocks.size () || input.blocks[m] == 0)
            continue;
        const geomsrv::Mesh& mesh = snapshot.meshes[m];
        std::vector<SolarCutTriangle> removed;
        std::vector<Poly> kept;
        double total = 0.0;
        for (size_t t = 0; t < mesh.TriangleCount (); ++t) {
            if (!Corners (mesh, t, scratch))
                continue;
            SolarCutTriangle whole;
            for (int c = 0; c < 3; ++c)
                for (int k = 0; k < 3; ++k)
                    whole.v[c][k] = scratch[size_t (c)][size_t (k)];
            total += SignedVolume (whole, input.point);
            std::vector<Poly> remaining { scratch }, next;
            for (const Sector& sector : sectors) {
                next.clear ();
                for (const Poly& poly : remaining) {
                    Poly outside = Clip (poly, sector.left.Negated ());
                    if (Valid (outside))
                        next.push_back (std::move (outside));
                    const Poly inLeft = Clip (poly, sector.left);
                    Poly beyond = Clip (inLeft, sector.right.Negated ());
                    if (Valid (beyond))
                        next.push_back (std::move (beyond));
                    const Poly inside = Clip (inLeft, sector.right);
                    if (!Valid (inside))
                        continue;
                    const Poly up = Clip (inside, sector.above);
                    if (Valid (up))
                        Emit (up, uint32_t (m), false, removed);
                    Poly down = Clip (inside, sector.above.Negated ());
                    if (Valid (down))
                        kept.push_back (std::move (down));
                }
                remaining.swap (next);
            }
            for (Poly& poly : remaining)
                kept.push_back (std::move (poly));
        }
        if (removed.empty ())
            continue;
        // Section loops: the cut faces, outward (downward) for the removed part.
        SolarCutObject object;
        object.mesh = m;
        object.guid = mesh.guid;
        object.elemType = mesh.elemType;
        object.total = std::abs (total);
        std::vector<SolarCutTriangle> caps;
        // Each face wound so its normal is `outward`: out of the removed part.
        const auto faces = [&] (const HalfSpace& plane, const Point& e1, const Point& e2,
                                const std::vector<HalfSpace>& clips, const Point& outward) {
            // The section plane through a point on it: the origin moved onto a lowered plane.
            const double offset = plane.Eval (origin);
            const Point at { origin[0] - offset * plane.n[0], origin[1] - offset * plane.n[1],
                             origin[2] - offset * plane.n[2] };
            std::vector<Poly> found;
            object.closed = Cap (mesh, plane, e1, e2, clips, at, scratch, found) && object.closed;
            for (const Poly& face : found)
                Emit (face, uint32_t (m), Dot (FaceNormal (face), outward) < 0.0, caps);
        };
        const Point up { 0.0, 0.0, 1.0 };
        for (size_t k = 0; k < sectors.size (); ++k) {
            const Sector& sector = sectors[k];
            const Point n { sector.above.n[0], sector.above.n[1], sector.above.n[2] };
            faces (sector.above, sector.e1, sector.e2, { sector.left, sector.right }, { -n[0], -n[1], -n[2] });
            // The outer sides: where the slice's wedge ends, the removed part is
            // closed by a vertical face (inner sides between sectors cancel).
            const auto forward = [&origin] (const Point& u) {
                return HalfSpace { { u[0], u[1], 0.0 }, -(u[0] * origin[0] + u[1] * origin[1]) };
            };
            if (k == 0)
                faces (sector.left, sector.sideFrom, up, { sector.above, forward (sector.sideFrom) },
                       { -sector.left.n[0], -sector.left.n[1], -sector.left.n[2] });
            if (k + 1 == sectors.size ())
                faces (sector.right, sector.sideTo, up, { sector.above, forward (sector.sideTo) },
                       { -sector.right.n[0], -sector.right.n[1], -sector.right.n[2] });
        }
        // The removed part's volume, its boundary oriented as the mesh is: a
        // mesh wound inward has a negative total, and its pieces flip with it.
        const double orientation = total < 0.0 ? -1.0 : 1.0;
        double volume = 0.0;
        for (const auto& piece : removed)
            volume += orientation * SignedVolume (piece, input.point);
        for (const auto& cap : caps)
            volume += SignedVolume (cap, input.point);
        object.removed = object.closed ? (std::max) (0.0, volume) : 0.0;
        result.removedVolume += object.removed;
        for (const auto* list : { &removed, &caps })
            for (const auto& triangle : *list)
                for (const auto& corner : triangle.v)
                    farthest =
                        (std::max) (farthest, std::sqrt (Dot (Sub ({ corner[0], corner[1], corner[2] }, origin),
                                                              Sub ({ corner[0], corner[1], corner[2] }, origin))));
        // As it would remain: the kept surfaces and the caps facing up into the
        // cut, so its shadows can be re-traced.
        geomsrv::Mesh remains;
        remains.guid = mesh.guid;
        remains.elemType = mesh.elemType;
        std::vector<SolarCutTriangle> keptTriangles;
        for (const Poly& poly : kept)
            Emit (poly, uint32_t (m), false, keptTriangles);
        for (const auto& cap : caps) {
            SolarCutTriangle flipped = cap;
            std::swap (flipped.v[1], flipped.v[2]);
            keptTriangles.push_back (flipped);
        }
        for (const auto& triangle : keptTriangles) {
            const uint32_t first = uint32_t (remains.vertices.size () / 3);
            for (const auto& corner : triangle.v)
                remains.vertices.insert (remains.vertices.end (), { corner[0], corner[1], corner[2] });
            remains.triangles.insert (remains.triangles.end (), { first, first + 1, first + 2 });
        }
        result.kept.emplace_back (m, std::move (remains));
        result.removed.insert (result.removed.end (), removed.begin (), removed.end ());
        result.caps.insert (result.caps.end (), caps.begin (), caps.end ());
        result.objects.push_back (std::move (object));
    }
    // The merged cutting surface, to just past the farthest cut.
    const double length = farthest > 0.0 ? farthest * 1.05 + 1.0 : reach;
    for (const Sector& sector : sectors) {
        SolarCutTriangle fan;
        for (int k = 0; k < 3; ++k) {
            fan.v[0][k] = origin[size_t (k)];
            fan.v[1][k] = origin[size_t (k)] + length * sector.sunFrom[size_t (k)];
            fan.v[2][k] = origin[size_t (k)] + length * sector.sunTo[size_t (k)];
        }
        result.envelope.push_back (fan);
    }
    std::stable_sort (result.objects.begin (), result.objects.end (),
                      [] (const SolarCutObject& a, const SolarCutObject& b) { return a.removed > b.removed; });
    return true;
}

} // namespace evp::sunstudy
