#include "ArchViz/RoofSlope.hpp"
#include "ArchViz/HudShell.hpp"
#include <clipper2/clipper.h>
#include <clipper2/clipper.triangulation.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace geomsrv::archviz::roofslope {
namespace {
namespace cp = Clipper2Lib;
constexpr double kTolerance = 1e-6, kMinSpan = 0.01;
constexpr int kPrecision = 6;
using Box = std::array<double, 4>;
double Aspect (Box b)
{
    const double w = b[2] - b[0], h = b[3] - b[1];
    return (std::max) (w, h) / (std::min) (w, h);
}
cp::PathD Path (const Ring& ring)
{
    cp::PathD out;
    for (const auto& p : ring)
        out.emplace_back (p.x, p.y);
    return out;
}
cp::PathsD Paths (const std::vector<Ring>& rings)
{
    cp::PathsD out;
    for (const auto& r : rings)
        out.push_back (Path (r));
    return out;
}
cp::PathD Rect (Box b)
{
    return { { b[0], b[1] }, { b[2], b[1] }, { b[2], b[3] }, { b[0], b[3] } };
}
double Area (const cp::PathsD& paths)
{
    return std::abs (cp::Area (paths));
}
double Cross (Vec a, Vec b, Vec c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
double Distance (Vec a, Vec b)
{
    return std::hypot (a.x - b.x, a.y - b.y);
}
double Bearing (const std::vector<Ring>& rings)
{
    double real = 0, imaginary = 0, longest = 0, reference = 0;
    std::vector<std::pair<double, double>> edges;
    for (const auto& ring : rings)
        for (size_t i = 0; i < ring.size (); ++i) {
            const auto a = ring[i], b = ring[(i + 1) % ring.size ()];
            const double length = Distance (a, b), angle = std::atan2 (b.y - a.y, b.x - a.x);
            if (length < kTolerance)
                continue;
            edges.emplace_back (length * length, angle);
            real += length * length * std::cos (4 * angle);
            imaginary += length * length * std::sin (4 * angle);
            if (length > longest)
                longest = length, reference = angle;
        }
    if (std::hypot (real, imaginary) < 1e-8)
        return reference;
    double family = std::atan2 (imaginary, real) / 4;
    constexpr double pi = 3.141592653589793;
    std::vector<std::pair<double, double>> nearby;
    double weight = 0;
    for (const auto& [w, angle] : edges) {
        const double offset = std::remainder (angle - family, pi / 2);
        if (std::abs (offset) < pi / 36)
            nearby.emplace_back (offset, w), weight += w;
    }
    std::sort (nearby.begin (), nearby.end ());
    double sum = 0;
    for (const auto& [offset, w] : nearby) {
        sum += w;
        if (sum >= weight / 2) {
            family += offset;
            break;
        }
    }
    return family + std::round ((reference - family) / (pi / 2)) * pi / 2;
}
Vec Mid (Vec a, Vec b)
{
    return { (a.x + b.x) / 2, (a.y + b.y) / 2 };
}
bool Close (double a, double b)
{
    return std::abs (a - b) < kTolerance;
}
bool Within (const Ring& ring, Vec at)
{
    return cp::PointInPolygon (cp::PointD (at.x, at.y), Path (ring)) == cp::PointInPolygonResult::IsInside;
}
std::string Number (double value, int decimals)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision (decimals) << value;
    return out.str ();
}
bool ValidOutline (const cp::PathsD& paths, std::string& error)
{
    // Triangulate refuses crossing paths. Union alone would silently repair them.
    cp::PathsD triangles;
    for (const auto& ring : paths) {
        if (ring.size () < 3 || std::abs (cp::Area (ring)) < 1e-8) {
            error = "Footprint contains a degenerate ring.";
            return false;
        }
        for (const auto& p : ring)
            if (!std::isfinite (p.x) || !std::isfinite (p.y)) {
                error = "Footprint coordinates must be finite.";
                return false;
            }
    }
    if (paths.empty () || cp::Triangulate (paths, kPrecision, triangles) != cp::TriangulateResult::success ||
        Area (cp::Union (paths, cp::FillRule::NonZero, kPrecision)) < 1e-8) {
        error = "Footprint is self-intersecting or its openings are invalid.";
        return false;
    }
    return true;
}
int NodeAt (Data& d, Vec xy, double z, const std::vector<Node>& previous)
{
    for (size_t i = 0; i < d.nodes.size (); ++i)
        if (Distance (d.nodes[i].xy, xy) < kTolerance)
            return int (i);
    Node node { xy, z };
    for (const auto& n : previous)
        if (Distance (n.xy, xy) < kTolerance) {
            node = n;
            break;
        }
    d.nodes.push_back (node);
    return int (d.nodes.size () - 1);
}
bool AddCell (Data& d, const cp::PathD& shape, Box box, const std::vector<Node>& previous,
              const std::vector<Cell>& oldCells, std::string& error)
{
    cp::PathD path = cp::TrimCollinear (shape, kPrecision);
    if (cp::Area (path) < 0)
        std::reverse (path.begin (), path.end ());
    const double area = cp::Area (path);
    if (area < 1e-9)
        return true;
    if (area >= d.cap - 1e-7) {
        error = "Every drainage cell must be strictly below " + Number (d.cap, 2) + " m2.";
        return false;
    }
    Cell cell;
    cell.box = box;
    cell.area = area;
    cell.rectangle = Close (area, (box[2] - box[0]) * (box[3] - box[1]));
    Vec drain { (box[0] + box[2]) / 2, (box[1] + box[3]) / 2 };
    for (const auto& p : Rect (box)) {
        cell.rim.push_back (NodeAt (d, { p.x, p.y }, d.ridgeZ, previous));
    }
    // Keep the cell's edited drain across divider moves. The caller translates rim
    // nodes first; a moved drain must still be visible from the whole cell boundary.
    for (const auto& old : oldCells)
        if (old.box == box && old.drain >= 0) {
            drain = previous[old.drain].xy;
            break;
        }
    cell.drainInside = cp::PointInPolygon (cp::PointD (drain.x, drain.y), path) == cp::PointInPolygonResult::IsInside;
    cell.drain = NodeAt (d, drain, d.topZ, previous);
    d.cells.push_back (std::move (cell));
    return true;
}
bool Mesh (Data& d, std::string& error)
{
    d.triangles.clear ();
    const auto outline = Paths (d.outline);
    for (size_t ci = 0; ci < d.cells.size (); ++ci) {
        auto& c = d.cells[ci];
        const Node& drain = d.nodes[c.drain];
        c.drainInside = false;
        for (const auto& p : cp::Intersect ({ Rect (c.box) }, outline, cp::FillRule::NonZero, kPrecision))
            if (cp::PointInPolygon (cp::PointD (drain.xy.x, drain.xy.y), p) == cp::PointInPolygonResult::IsInside)
                c.drainInside = !c.drainInside;
        if (!std::isfinite (drain.z) || drain.z < d.topZ - kTolerance) {
            error = "Drain elevation cannot be below the slab top.";
            return false;
        }
        double covered = 0;
        for (size_t i = 0; i < c.rim.size (); ++i) {
            const int ai = c.rim[i], bi = c.rim[(i + 1) % c.rim.size ()];
            const auto& a = d.nodes[ai];
            const auto& b = d.nodes[bi];
            const double cross = Cross (a.xy, b.xy, drain.xy);
            if (cross < 1e-8 || !std::isfinite (a.z) || !std::isfinite (b.z) || a.z <= drain.z + 1e-6 ||
                b.z <= drain.z + 1e-6) {
                error = "Keep the drain inside its cell and lower than every perimeter point.";
                return false;
            }
            const double gx = ((b.z - a.z) * (drain.xy.y - a.xy.y) - (drain.z - a.z) * (b.xy.y - a.xy.y)) / cross;
            const double gy = ((b.xy.x - a.xy.x) * (drain.z - a.z) - (drain.xy.x - a.xy.x) * (b.z - a.z)) / cross;
            // Make the four-point X first, then clip each genuinely planar face.
            const cp::PathsD pieces =
                cp::Intersect ({ { { a.xy.x, a.xy.y }, { b.xy.x, b.xy.y }, { drain.xy.x, drain.xy.y } } }, outline,
                               cp::FillRule::NonZero, kPrecision);
            cp::PathsD triangles;
            if (!pieces.empty () && cp::Triangulate (pieces, kPrecision, triangles) != cp::TriangulateResult::success) {
                error = "Unable to triangulate a clipped slope face.";
                return false;
            }
            for (const auto& tri : triangles) {
                Triangle t;
                t.nodes = { ai, bi, c.drain };
                t.cell = int (ci), t.gx = gx, t.gy = gy, t.area = std::abs (cp::Area (tri));
                for (int k = 0; k < 3; ++k)
                    t.vertices[k] = { { tri[k].x, tri[k].y },
                                      a.z + gx * (tri[k].x - a.xy.x) + gy * (tri[k].y - a.xy.y) };
                d.triangles.push_back (t);
                covered += t.area;
            }
        }
        // Each face is clipped independently on Clipper's micron grid. Boundary
        // intersections can round oppositely to the rectangle's own clip; bound
        // that error by perimeter * grid, not only by area (long thin cells).
        const double clipTolerance = (std::max) (1e-5, 4e-6 * ((c.box[2] - c.box[0]) + (c.box[3] - c.box[1])));
        if (std::abs (covered - c.area) > clipTolerance) {
            error = "Drain fan does not cover rectangle " + std::to_string (ci + 1) + ": clipped " +
                    Number (c.area, 9) + " m2, faces " + Number (covered, 9) + " m2.";
            return false;
        }
    }
    // A corner on the middle of a neighbour's side must lie on that side's plane.
    // Refuse incompatible Z edits rather than silently adding a step or fifth face.
    for (const auto& cell : d.cells)
        for (size_t j = 0; j < 4; ++j) {
            const Node &a = d.nodes[cell.rim[j]], &b = d.nodes[cell.rim[(j + 1) % 4]];
            const double length = Distance (a.xy, b.xy);
            for (const auto& other : d.cells)
                for (int n : other.rim) {
                    const Node& p = d.nodes[n];
                    const double t = ((p.xy.x - a.xy.x) * (b.xy.x - a.xy.x) + (p.xy.y - a.xy.y) * (b.xy.y - a.xy.y)) /
                                     (length * length);
                    if (t > 1e-6 && t < 1 - 1e-6 && std::abs (Cross (a.xy, b.xy, p.xy)) / length < kTolerance &&
                        std::abs (p.z - (a.z + t * (b.z - a.z))) > kTolerance) {
                        error = "This height breaks a shared ridge. Edit its connected endpoints together.";
                        return false;
                    }
                }
        }
    return true;
}
std::vector<double> Cuts (std::vector<double> values, double step)
{
    std::sort (values.begin (), values.end ());
    values.erase (std::unique (values.begin (), values.end (), Close), values.end ());
    // Tiny corner events are clipping detail, not drainage strips. A minimum
    // candidate spacing proportional to the drainage size prevents hundreds of
    // tiny square cells around bevels/arcs. The exact outline still clips faces.
    const double spacing = (std::max) (0.05, step * 0.15);
    std::vector<double> significant { values.front () };
    for (size_t i = 1; i + 1 < values.size (); ++i)
        if (values[i] - significant.back () >= spacing && values.back () - values[i] >= spacing)
            significant.push_back (values[i]);
    if (!Close (significant.back (), values.back ()))
        significant.push_back (values.back ());
    values = std::move (significant);
    std::vector<double> cuts;
    for (size_t i = 0; i + 1 < values.size (); ++i) {
        const int count = int (std::ceil ((values[i + 1] - values[i]) / step));
        for (int j = 0; j < count; ++j)
            cuts.push_back (values[i] + (values[i + 1] - values[i]) * j / count);
    }
    cuts.push_back (values.back ());
    return cuts;
}
bool Populate (Data& d, const std::vector<Box>& boxes, const std::vector<Node>& previous,
               const std::vector<Cell>& oldCells, std::string& error)
{
    const auto outline = Paths (d.outline);
    d.nodes.clear ();
    d.cells.clear ();
    cp::PathsD cover;
    double sum = 0;
    for (const auto& b : boxes) {
        if (b[2] - b[0] < kMinSpan || b[3] - b[1] < kMinSpan) {
            error = "A divider leaves a cell narrower than 10 mm.";
            return false;
        }
        if (Aspect (b) > kMaxAspect + 1e-6) {
            error = "Roof rectangles must stay near-square (maximum aspect ratio 1.5:1).";
            return false;
        }
        const auto pieces = cp::Intersect ({ Rect (b) }, outline, cp::FillRule::NonZero, kPrecision);
        double cellArea = 0;
        for (const auto& piece : pieces) {
            cover.push_back (piece);
            sum += cp::Area (piece);
            cellArea += cp::Area (piece);
        }
        if (cellArea > 1e-9) {
            if (!AddCell (d, Rect (b), b, previous, oldCells, error))
                return false;
            d.cells.back ().area = cellArea;
            d.cells.back ().rectangle = Close (cellArea, cp::Area (Rect (b)));
        }
        if (d.cells.size () > 1000) {
            error = "More than 1000 roof cells. Reduce the input scope.";
            return false;
        }
    }
    const auto united = cp::Union (cover, cp::FillRule::NonZero, kPrecision);
    const double tolerance = (std::max) (1e-4, Area (outline) * 1e-7);
    if (Area (cp::Xor (outline, united, cp::FillRule::NonZero, kPrecision)) > tolerance ||
        std::abs (sum - Area (united)) > tolerance) {
        error = "This edit would leave an overlap, gap or lost opening.";
        return false;
    }
    return Mesh (d, error);
}
} // namespace

Vec World (const Data& d, Vec p)
{
    return { d.origin.x + p.x * std::cos (d.bearing) - p.y * std::sin (d.bearing),
             d.origin.y + p.x * std::sin (d.bearing) + p.y * std::cos (d.bearing) };
}
Vec Local (const Data& d, Vec p)
{
    const double x = p.x - d.origin.x, y = p.y - d.origin.y;
    return { x * std::cos (d.bearing) + y * std::sin (d.bearing),
             -x * std::sin (d.bearing) + y * std::cos (d.bearing) };
}
bool Plan (const std::vector<Ring>& world, double top, double percent, double cap, Data& out, std::string& error,
           std::optional<double> bearing)
{
    if (world.empty () || world[0].empty () || !std::isfinite (top) || !std::isfinite (percent) || percent <= 0 ||
        percent > 100 || !std::isfinite (cap) || cap <= 0 || cap > 200) {
        error = "Supply a valid slab top, slope (0, 100]% and cell cap (0, 200] m2.";
        return false;
    }
    size_t pointCount = 0;
    for (const auto& ring : world) {
        pointCount += ring.size ();
        if (ring.size () < 3 || pointCount > 20000) {
            error = "Footprint needs closed rings of at least three points, with at most 20000 points in total.";
            return false;
        }
        for (const auto& point : ring)
            if (!std::isfinite (point.x) || !std::isfinite (point.y)) {
                error = "Footprint coordinates must be finite.";
                return false;
            }
    }
    Data d;
    d.origin = world[0][0];
    d.topZ = top, d.targetPercent = percent, d.cap = cap;
    d.bearing = Bearing (world);
    if (bearing) {
        if (!std::isfinite (*bearing)) {
            error = "Layout rotation must be finite.";
            return false;
        }
        d.bearing = *bearing;
    }
    std::vector<double> xs, ys;
    for (const auto& ring : world) {
        Ring local;
        for (Vec p : ring) {
            p = Local (d, p);
            local.push_back (p), xs.push_back (p.x), ys.push_back (p.y);
        }
        int depth = 0;
        for (const auto& other : world)
            if (&other != &ring && cp::PointInPolygon (cp::PointD (ring[0].x, ring[0].y), Path (other)) ==
                                       cp::PointInPolygonResult::IsInside)
                ++depth;
        if ((cp::Area (Path (local)) > 0) != (depth % 2 == 0))
            std::reverse (local.begin (), local.end ());
        d.outline.push_back (local);
    }
    const auto outline = Paths (d.outline);
    if (!ValidOutline (outline, error))
        return false;
    // Boundary events, not a fixed world grid. Split the remainder evenly and merge
    // full neighbouring boxes; cutout coordinates always survive as topology events.
    const auto x = Cuts (xs, std::sqrt (cap * 0.95)), y = Cuts (ys, std::sqrt (cap * 0.95));
    if (x.size () * y.size () > 20000) {
        error = "Footprint decomposition exceeds 20000 event cells. Reduce arc sampling or input scope.";
        return false;
    }
    std::vector<Box> boxes;
    for (size_t j = 0; j + 1 < y.size (); ++j)
        for (size_t i = 0; i + 1 < x.size (); ++i) {
            Box b { x[i], y[j], x[i + 1], y[j + 1] };
            if (b[2] - b[0] < kTolerance || b[3] - b[1] < kTolerance)
                continue;
            auto parts = cp::Intersect ({ Rect (b) }, outline, cp::FillRule::NonZero, kPrecision);
            if (Area (parts) > 1e-9) {
                boxes.push_back (b);
            }
        }
    for (int axis : { 0, 1 })
        for (size_t i = 0; i < boxes.size (); ++i)
            for (size_t j = i + 1; j < boxes.size ();) {
                auto a = boxes[i], b = boxes[j];
                const int other = 1 - axis;
                if (Close (a[other], b[other]) && Close (a[other + 2], b[other + 2]) &&
                    (Close (a[axis + 2], b[axis]) || Close (b[axis + 2], a[axis]))) {
                    a[axis] = (std::min) (a[axis], b[axis]);
                    a[axis + 2] = (std::max) (a[axis + 2], b[axis + 2]);
                    const auto parts = cp::Intersect ({ Rect (a) }, outline, cp::FillRule::NonZero, kPrecision);
                    bool centerInside = false;
                    int components = 0;
                    const cp::PointD center { (a[0] + a[2]) / 2, (a[1] + a[3]) / 2 };
                    for (const auto& p : parts) {
                        components += cp::Area (p) > 0;
                        if (cp::PointInPolygon (center, p) == cp::PointInPolygonResult::IsInside)
                            centerInside = !centerInside;
                    }
                    if (cp::Area (Rect (a)) < cap * 0.999 && Aspect (a) <= kMaxAspect && centerInside &&
                        components == 1) {
                        boxes[i] = a;
                        boxes.erase (boxes.begin () + j);
                        continue;
                    }
                }
                ++j;
            }
    // Corner events can leave a shallow strip even before any merge. Split its
    // long axis evenly; clipping retains the exact footprint, not the grid strips.
    std::vector<Box> balanced;
    for (const auto& b : boxes) {
        const double w = b[2] - b[0], h = b[3] - b[1];
        const int nx = int (std::ceil (w / (h * kMaxAspect))), ny = int (std::ceil (h / (w * kMaxAspect)));
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                balanced.push_back (
                    { b[0] + w * i / nx, b[1] + h * j / ny, b[0] + w * (i + 1) / nx, b[1] + h * (j + 1) / ny });
        if (balanced.size () > 1000) {
            error = "More than 1000 near-square roof rectangles. Reduce the input scope.";
            return false;
        }
    }
    boxes = std::move (balanced);
    double inv = 0, inv2 = 0;
    for (const auto& b : boxes)
        for (double run : { (b[2] - b[0]) / 2, (b[3] - b[1]) / 2 })
            inv += 1 / run, inv2 += 1 / (run * run);
    d.ridgeZ = top + (std::max) (0.001, percent / 100 * inv / inv2);
    if (!Populate (d, boxes, {}, {}, error))
        return false;
    out = std::move (d);
    return true;
}
bool Rebuild (Data& d, std::string& error)
{
    return Mesh (d, error);
}
bool SetElevation (Data& d, int node, double z, std::string& error)
{
    if (node < 0 || size_t (node) >= d.nodes.size () || d.nodes[node].lockZ || !std::isfinite (z)) {
        error = "Select an unlocked elevation point and enter a finite height.";
        return false;
    }
    Data candidate = d;
    candidate.nodes[node].z = z;
    if (!Mesh (candidate, error))
        return false;
    d = std::move (candidate);
    return true;
}
bool MoveDrain (Data& d, int cell, Vec xy, std::string& error)
{
    if (cell < 0 || size_t (cell) >= d.cells.size () || d.nodes[d.cells[cell].drain].lockXY || !std::isfinite (xy.x) ||
        !std::isfinite (xy.y)) {
        error = "Select an unlocked drain position.";
        return false;
    }
    bool inside = false;
    for (const auto& ring : d.outline)
        if (Within (ring, xy))
            inside = !inside;
    if (!inside) {
        error = "Place the drain on the actual roof, not in an opening or clipped-out area.";
        return false;
    }
    Data candidate = d;
    candidate.nodes[d.cells[cell].drain].xy = xy;
    if (!Mesh (candidate, error))
        return false;
    d = std::move (candidate);
    return true;
}
bool MoveDivider (Data& d, int cell, int side, double value, std::string& error)
{
    if (cell < 0 || size_t (cell) >= d.cells.size () || side < 0 || side > 3 || !std::isfinite (value)) {
        error = "Select a rectangle divider.";
        return false;
    }
    const int axis = side % 2;
    const double old = d.cells[cell].box[side];
    if (Close (value, old))
        return true;
    std::vector<Box> boxes;
    std::vector<Cell> oldCells = d.cells;
    std::vector<Node> nodes = d.nodes;
    bool low = false, high = false;
    for (auto& c : oldCells) {
        auto b = c.box;
        for (int s : { axis, axis + 2 })
            if (Close (b[s], old)) {
                (s < 2 ? low : high) = true;
                c.box[s] = value;
            }
        if (c.box != b) {
            const double fraction =
                (axis == 0 ? nodes[c.drain].xy.x - b[0] : nodes[c.drain].xy.y - b[1]) / (b[axis + 2] - b[axis]);
            (axis == 0 ? nodes[c.drain].xy.x : nodes[c.drain].xy.y) =
                c.box[axis] + fraction * (c.box[axis + 2] - c.box[axis]);
            if (nodes[c.drain].lockXY) {
                error = "A drain on this divider is XY-locked.";
                return false;
            }
        }
        if (std::find (boxes.begin (), boxes.end (), c.box) == boxes.end ())
            boxes.push_back (c.box);
    }
    if (!low || !high) {
        error = "The slab boundary is fixed. Drag an internal divider.";
        return false;
    }
    for (int n = 0; n < int (nodes.size ()); ++n) {
        const double at = axis == 0 ? nodes[n].xy.x : nodes[n].xy.y;
        if (Close (at, old)) {
            if (nodes[n].lockXY) {
                error = "A point on this divider is XY-locked.";
                return false;
            }
            (axis == 0 ? nodes[n].xy.x : nodes[n].xy.y) = value;
        }
    }
    Data candidate = d;
    if (!Populate (candidate, boxes, nodes, oldCells, error))
        return false;
    d = std::move (candidate);
    return true;
}
bool RemoveDivider (Data& d, int cell, int side, std::string& error)
{
    if (cell < 0 || size_t (cell) >= d.cells.size () || side < 0 || side > 3) {
        error = "Select an internal rectangle edge to delete. The source slab is never deleted.";
        return false;
    }
    const auto a = d.cells[cell].box;
    const int axis = side % 2, other = 1 - axis;
    for (size_t i = 0; i < d.cells.size (); ++i) {
        const auto b = d.cells[i].box;
        if (i == size_t (cell) || !Close (a[side], b[(side + 2) % 4]) || !Close (a[other], b[other]) ||
            !Close (a[other + 2], b[other + 2]))
            continue;
        for (int n : d.cells[cell].rim)
            if (d.nodes[n].lockXY || d.nodes[n].lockZ) {
                error = "Unlock this edge before deleting it.";
                return false;
            }
        for (int n : d.cells[i].rim)
            if (d.nodes[n].lockXY || d.nodes[n].lockZ) {
                error = "The neighbouring rectangle is locked.";
                return false;
            }
        for (int c : { cell, int (i) })
            if (d.nodes[d.cells[c].drain].lockXY || d.nodes[d.cells[c].drain].lockZ) {
                error = "Unlock the drains before merging their rectangles.";
                return false;
            }
        Box merged { (std::min) (a[0], b[0]), (std::min) (a[1], b[1]), (std::max) (a[2], b[2]),
                     (std::max) (a[3], b[3]) };
        if ((merged[2] - merged[0]) * (merged[3] - merged[1]) >= d.cap || Aspect (merged) > kMaxAspect) {
            error = "Deleting this edge would exceed the roof area/aspect limits.";
            return false;
        }
        std::vector<Box> boxes;
        for (size_t j = 0; j < d.cells.size (); ++j)
            if (j != i && j != size_t (cell))
                boxes.push_back (d.cells[j].box);
        boxes.push_back (merged);
        auto candidate = d;
        if (!Populate (candidate, boxes, d.nodes, d.cells, error))
            return false;
        d = std::move (candidate);
        return true;
    }
    error = "This edge is a footprint boundary or meets several neighbours. Use virtual footprint grips.";
    return false;
}
bool AddDivider (Data& d, int cell, int side, Vec at, std::string& error)
{
    if (cell < 0 || size_t (cell) >= d.cells.size () || side < 0 || side > 3 || !std::isfinite (at.x) ||
        !std::isfinite (at.y)) {
        error = "Right-click a rectangle edge to add a perpendicular divider.";
        return false;
    }
    const auto& selected = d.cells[cell];
    auto controls = selected.rim;
    controls.push_back (selected.drain);
    for (int n : controls)
        if (d.nodes[n].lockXY || d.nodes[n].lockZ) {
            error = "Unlock this rectangle before subdividing it.";
            return false;
        }
    const int axis = 1 - side % 2;
    const double value = axis == 0 ? at.x : at.y;
    auto first = selected.box, second = selected.box;
    if (value <= first[axis] + kMinSpan || value >= first[axis + 2] - kMinSpan) {
        error = "Add the divider away from the edge endpoints (more than 10 mm).";
        return false;
    }
    first[axis + 2] = second[axis] = value;
    std::vector<Box> boxes;
    for (size_t i = 0; i < d.cells.size (); ++i)
        if (i != size_t (cell))
            boxes.push_back (d.cells[i].box);
    for (auto child : { first, second }) {
        const int longAxis = child[2] - child[0] >= child[3] - child[1] ? 0 : 1;
        const double length = child[longAxis + 2] - child[longAxis];
        const double width = child[3 - longAxis] - child[1 - longAxis];
        const int count = int (std::ceil (length / (width * kMaxAspect)));
        if (count > 1000) {
            error = "This cut would create too many narrow cells.";
            return false;
        }
        for (int i = 0; i < count; ++i) {
            auto box = child;
            box[longAxis] += length * i / count;
            box[longAxis + 2] = child[longAxis] + length * (i + 1) / count;
            boxes.push_back (box);
        }
    }
    Data candidate = d;
    auto previous = d.nodes;
    // The old drain becomes a ridge if a new crossing coincides with it.
    previous[selected.drain].z = d.ridgeZ;
    for (const auto& box : boxes)
        for (const auto& p : Rect (box))
            for (size_t i = 0; i < selected.rim.size (); ++i) {
                const auto &a = d.nodes[selected.rim[i]], &b = d.nodes[selected.rim[(i + 1) % selected.rim.size ()]];
                const Vec xy { p.x, p.y };
                const double length = Distance (a.xy, b.xy);
                const double t =
                    ((xy.x - a.xy.x) * (b.xy.x - a.xy.x) + (xy.y - a.xy.y) * (b.xy.y - a.xy.y)) / (length * length);
                if (t > 1e-6 && t < 1 - 1e-6 && std::abs (Cross (a.xy, b.xy, xy)) / length < kTolerance)
                    previous.push_back ({ xy, a.z + t * (b.z - a.z) });
            }
    if (!Populate (candidate, boxes, previous, d.cells, error))
        return false;
    d = std::move (candidate);
    return true;
}
bool EditOutline (Data& d, int ring, int vertex, Vec at, bool insert, bool remove, std::string& error)
{
    for (const auto& n : d.nodes)
        if (n.lockXY || n.lockZ) {
            error = "Unlock all controls before replanning the virtual footprint.";
            return false;
        }
    if (ring < 0 || size_t (ring) >= d.outline.size () || vertex < 0 || size_t (vertex) >= d.outline[ring].size ()) {
        error = "Select a footprint vertex or segment.";
        return false;
    }
    auto rings = d.outline;
    auto& r = rings[ring];
    if (remove) {
        if (r.size () <= 3) {
            error = "A closed footprint needs at least three vertices.";
            return false;
        }
        r.erase (r.begin () + vertex);
    }
    else if (insert)
        r.insert (r.begin () + vertex + 1, at);
    else
        r[vertex] = at;
    for (auto& contour : rings)
        for (auto& p : contour)
            p = World (d, p);
    Data candidate;
    if (!Plan (rings, d.topZ, d.targetPercent, d.cap, candidate, error))
        return false;
    candidate.textHeight = d.textHeight;
    d = std::move (candidate);
    return true;
}
bool LiftCell (Data& d, int cell, double delta, std::string& error)
{
    if (cell < 0 || size_t (cell) >= d.cells.size () || !std::isfinite (delta)) {
        error = "Select a face and enter a finite push/pull distance.";
        return false;
    }
    Data candidate = d;
    auto nodes = candidate.cells[cell].rim;
    nodes.push_back (candidate.cells[cell].drain);
    for (int n : nodes) {
        if (candidate.nodes[n].lockZ) {
            error = "A point on this face is Z-locked.";
            return false;
        }
        candidate.nodes[n].z += delta;
    }
    if (!Mesh (candidate, error))
        return false;
    d = std::move (candidate);
    return true;
}
bool Reorient (Data& d, double bearing, std::string& error)
{
    for (const auto& n : d.nodes)
        if (n.lockXY || n.lockZ) {
            error = "Unlock all controls before rotating and replanning the layout.";
            return false;
        }
    auto rings = d.outline;
    for (auto& ring : rings)
        for (auto& p : ring)
            p = World (d, p);
    Data candidate;
    if (!Plan (rings, d.topZ, d.targetPercent, d.cap, candidate, error, bearing))
        return false;
    candidate.textHeight = d.textHeight;
    d = std::move (candidate);
    return true;
}
Snap Infer (const Data& d, Vec at, Vec anchor, double tolerance, int exclude)
{
    Snap out { at };
    double best = tolerance;
    auto offer = [&] (Vec p, const char* label) {
        const double distance = Distance (p, at);
        if (distance < best) {
            best = distance;
            out = { p, label };
        }
    };
    for (int i = 0; i < int (d.nodes.size ()); ++i)
        if (i != exclude)
            offer (d.nodes[i].xy, "Endpoint");
    for (const auto& cell : d.cells)
        for (size_t i = 0; i < cell.rim.size (); ++i) {
            Vec a = d.nodes[cell.rim[i]].xy, b = d.nodes[cell.rim[(i + 1) % cell.rim.size ()]].xy;
            offer (Mid (a, b), "Midpoint");
        }
    // Nearby projected segment intersections. Bound the inference candidates,
    // not the geometry, to avoid an all-pairs scan on every pointer update.
    std::vector<std::pair<Vec, Vec>> segments;
    auto nearLine = [&] (Vec a, Vec b) {
        const double length = Distance (a, b);
        if (length > kTolerance && std::abs (Cross (a, b, at)) / length < tolerance && segments.size () < 48)
            segments.emplace_back (a, b);
    };
    for (const auto& ring : d.outline)
        for (size_t i = 0; i < ring.size (); ++i)
            nearLine (ring[i], ring[(i + 1) % ring.size ()]);
    for (const auto& c : d.cells)
        for (size_t i = 0; i < c.rim.size (); ++i)
            nearLine (d.nodes[c.rim[i]].xy, d.nodes[c.rim[(i + 1) % c.rim.size ()]].xy);
    for (size_t i = 0; i < segments.size (); ++i)
        for (size_t j = i + 1; j < segments.size (); ++j) {
            const auto [a, b] = segments[i];
            const auto [c, e] = segments[j];
            Vec u { b.x - a.x, b.y - a.y }, v { e.x - c.x, e.y - c.y };
            const double cross = u.x * v.y - u.y * v.x;
            if (std::abs (cross) < 1e-8)
                continue;
            const double t = ((c.x - a.x) * v.y - (c.y - a.y) * v.x) / cross;
            offer ({ a.x + t * u.x, a.y + t * u.y }, "Projected intersection");
        }
    if (!out.label.empty ())
        return out;
    for (size_t i = 0; i < d.nodes.size (); ++i) {
        if (int (i) == exclude)
            continue;
        const auto& p = d.nodes[i];
        if (std::abs (at.x - p.xy.x) < best) {
            best = std::abs (at.x - p.xy.x);
            out = { { p.xy.x, at.y }, "Perpendicular guide", p.xy, true };
        }
        if (std::abs (at.y - p.xy.y) < best) {
            best = std::abs (at.y - p.xy.y);
            out = { { at.x, p.xy.y }, "Parallel guide", p.xy, true };
        }
    }
    if (out.label.empty () && std::abs (at.x - anchor.x) < tolerance)
        out = { { anchor.x, at.y }, "Roof V axis", anchor, true };
    if (out.label.empty () && std::abs (at.y - anchor.y) < tolerance)
        out = { { at.x, anchor.y }, "Roof U axis", anchor, true };
    return out;
}

overlaylayers::Layer Layer (const Data& d)
{
    namespace ol = overlaylayers;
    ol::Layer layer;
    layer.name = kLayer;
    layer.lifetime = ol::LayerLifetime::Command;
    layer.views = ol::Views::Both;
    layer.occlusion = ol::Behind::Fade;
    ol::Mesh mesh;
    mesh.rgba = 0x839987FF;
    mesh.styled = true;
    mesh.style.opacity = 0.24f;
    auto xyz = [&] (int n) {
        Vec p = World (d, d.nodes[n].xy);
        return std::array<double, 3> { p.x, p.y, d.nodes[n].z };
    };
    auto line = [&] (std::array<double, 3> a, std::array<double, 3> b, uint32_t color, float width) {
        ol::Polyline p;
        p.points = { a[0], a[1], a[2], b[0], b[1], b[2] };
        p.rgba = color, p.widthPixels = width;
        layer.polylines.push_back (std::move (p));
    };
    struct Edge {
        std::array<double, 3> a, b;
        std::array<int, 3> face;
        int count = 0;
        bool breakline = false;
    };
    std::map<std::array<int64_t, 4>, Edge> edges;
    std::map<std::array<int, 3>, const Triangle*> labels;
    for (const auto& t : d.triangles) {
        const uint32_t first = uint32_t (mesh.points.size () / 3);
        std::array<std::array<double, 3>, 3> vertices;
        for (int k = 0; k < 3; ++k) {
            Vec p = World (d, t.vertices[k].xy);
            vertices[k] = { p.x, p.y, t.vertices[k].z };
            mesh.points.insert (mesh.points.end (), vertices[k].begin (), vertices[k].end ());
        }
        mesh.indices.insert (mesh.indices.end (), { first, first + 1, first + 2 });
        for (int j = 0; j < 3; ++j) {
            auto a = vertices[j], b = vertices[(j + 1) % 3];
            if (a > b)
                std::swap (a, b);
            std::array<int64_t, 4> key { std::llround (a[0] * 1e6), std::llround (a[1] * 1e6),
                                         std::llround (b[0] * 1e6), std::llround (b[1] * 1e6) };
            auto& e = edges[key];
            if (e.count == 0)
                e.a = a, e.b = b, e.face = t.nodes;
            e.breakline = e.breakline || e.face != t.nodes;
            ++e.count;
        }
        if (!labels.contains (t.nodes) || labels[t.nodes]->area < t.area)
            labels[t.nodes] = &t;
    }
    for (const auto& [face, triangle] : labels) {
        const auto& t = *triangle;
        const double slope = std::hypot (t.gx, t.gy);
        Vec center {};
        double z = 0;
        for (const auto& n : t.vertices)
            center.x += n.xy.x / 3, center.y += n.xy.y / 3, z += n.z / 3;
        Vec wc = World (d, center);
        double length = (std::min) (1.5, std::sqrt (t.area) * 0.3);
        const Vec down { -t.gx / slope, -t.gy / slope };
        const double winding = Cross (t.vertices[0].xy, t.vertices[1].xy, t.vertices[2].xy) > 0 ? 1 : -1;
        for (int j = 0; j < 3; ++j) {
            const Vec a = t.vertices[j].xy, b = t.vertices[(j + 1) % 3].xy;
            const double derivative = winding * ((b.x - a.x) * down.y - (b.y - a.y) * down.x);
            if (derivative < -1e-10)
                length = (std::min) (length, 0.75 * winding * Cross (a, b, center) / -derivative);
        }
        Vec end = World (d, { center.x - t.gx / slope * length, center.y - t.gy / slope * length });
        line ({ wc.x, wc.y, z + 0.005 }, { end.x, end.y, z - slope * length + 0.005 }, 0x5A7F62FF, 1.5f);
        layer.polylines.back ().endArrow = ol::Terminator::Arrow;
        ol::Text text;
        text.text = Number (slope * 100, 2) + "%";
        text.at[0] = wc.x, text.at[1] = wc.y, text.at[2] = z + 0.01;
        text.planar = true, text.sizeMetres = (std::max) (0.30, d.textHeight);
        text.rgba = 0x37523DFF, text.haloRgba = 0, text.haloPixels = 0;
        text.font = "tapioca:roof-inter";
        const double cs = std::cos (d.bearing), sn = std::sin (d.bearing);
        const double gx = t.gx * cs - t.gy * sn, gy = t.gx * sn + t.gy * cs;
        // Model-space text runs along the actual fall arrow, kept upright in plan.
        const double sign = gx > 0 || (std::abs (gx) < 1e-9 && gy > 0) ? 1 : -1;
        text.direction[0] = sign * gx / slope, text.direction[1] = sign * gy / slope;
        text.direction[2] = sign * slope;
        text.at[0] -= text.direction[1] * text.sizeMetres * 0.7;
        text.at[1] += text.direction[0] * text.sizeMetres * 0.7;
        const double normal = std::sqrt (1 + gx * gx + gy * gy);
        text.normal[0] = -gx / normal, text.normal[1] = -gy / normal, text.normal[2] = 1 / normal;
        layer.texts.push_back (text);
    }
    for (const auto& [key, edge] : edges)
        if (edge.count == 1 || edge.breakline)
            line (edge.a, edge.b, 0x37523DFF, 1.2f);
    ol::PointSet points;
    points.rgba = 0x5A7F62FF;
    points.sizeMetres = 0.15f;
    for (const auto& c : d.cells) {
        if (!c.drainInside)
            continue;
        auto p = xyz (c.drain);
        points.points.insert (points.points.end (), p.begin (), p.end ());
    }
    if (!points.points.empty ())
        layer.points.push_back (std::move (points));
    layer.meshes.push_back (std::move (mesh));
    layer.panels.push_back (PanelStyle ());
    return layer;
}
overlaylayers::Panel PanelStyle ()
{
    // OVERLAY-INVARIANTS.md: change editor content, not the shared HUD's look.
    auto panel = hudshell::PlainLook ();
    panel.title = "Roof slopes";
    panel.widthPixels = 360;
    return panel;
}
overlaylayers::Panel EditorStyle ()
{
    return hudshell::PlainLook ();
}
void Editor::Begin ()
{
    if (!gesture)
        before = data, gesture = true, startedRevision = revision;
}
void Editor::Commit ()
{
    if (!gesture)
        return;
    if (startedRevision == revision) {
        gesture = false;
        before = {};
        return;
    }
    undo.push_back (std::move (before));
    if (undo.size () > 64)
        undo.erase (undo.begin ());
    redo.clear ();
    gesture = false;
}
void Editor::Cancel ()
{
    if (gesture)
        data = std::move (before), gesture = false, ++revision;
    message = "Manipulation cancelled.";
}
bool Editor::Undo (bool backwards)
{
    if (gesture)
        Cancel ();
    auto& from = backwards ? undo : redo;
    auto& to = backwards ? redo : undo;
    if (from.empty ())
        return false;
    to.push_back (std::move (data));
    data = std::move (from.back ());
    from.pop_back ();
    selectedCell = selectedNode = selectedSide = -1;
    ++revision;
    return true;
}
bool Editor::Apply (Data candidate, const std::string& error)
{
    if (!error.empty ()) {
        message = error;
        return false;
    }
    data = std::move (candidate);
    ++revision;
    message.clear ();
    return true;
}
} // namespace geomsrv::archviz::roofslope
