#include "ArchViz/StorySliceGeometry.hpp"
#include "Geometry/PlanarTriangulation.hpp"

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>

namespace geomsrv {
namespace archviz {

namespace {

// The same grid StorySliceUnion quantised onto. Chaining has to weld on exactly
// that grid or the contours come apart at every junction the union just welded.
constexpr double kTol = 1e-4;

using Cell = std::pair<int64_t, int64_t>;

struct CellHash {
    size_t operator() (const Cell& c) const
    {
        const uint64_t a = static_cast<uint64_t> (c.first) * 0x9E3779B97F4A7C15ull;
        const uint64_t b = static_cast<uint64_t> (c.second) * 0xC2B2AE3D27D4EB4Full;
        return static_cast<size_t> (a ^ (b + 0x165667B19E3779F9ull + (a << 6) + (a >> 2)));
    }
};

Cell CellOf (double x, double y)
{
    return Cell { static_cast<int64_t> (std::llround (x / kTol)), static_cast<int64_t> (std::llround (y / kTol)) };
}

struct Edge {
    uint32_t a = 0, b = 0;
};

} // namespace

std::vector<SliceChain> ChainUnionSegments (const std::vector<UnionSegment>& segments)
{
    std::vector<SliceChain> out;
    if (segments.empty ())
        return out;

    std::vector<double> nodesXY;
    std::vector<Edge> edges;
    std::unordered_map<Cell, uint32_t, CellHash> nodeOf;

    auto nodeId = [&] (double x, double y) {
        const Cell c = CellOf (x, y);
        auto it = nodeOf.find (c);
        if (it != nodeOf.end ())
            return it->second;
        const uint32_t id = static_cast<uint32_t> (nodesXY.size () / 2);
        nodesXY.push_back (x);
        nodesXY.push_back (y);
        nodeOf.emplace (c, id);
        return id;
    };

    for (const UnionSegment& s : segments) {
        const uint32_t a = nodeId (s.x0, s.y0);
        const uint32_t b = nodeId (s.x1, s.y1);
        if (a != b)
            edges.push_back (Edge { a, b });
    }
    if (edges.empty ())
        return out;

    const size_t nNodes = nodesXY.size () / 2;
    std::vector<std::vector<uint32_t>> adj (nNodes);
    for (uint32_t i = 0; i < edges.size (); ++i) {
        adj[edges[i].a].push_back (i);
        adj[edges[i].b].push_back (i);
    }
    std::vector<bool> used (edges.size (), false);

    auto other = [&] (uint32_t e, uint32_t node) { return edges[e].a == node ? edges[e].b : edges[e].a; };

    auto walk = [&] (uint32_t start) {
        SliceChain chain;
        uint32_t cur = start;
        chain.xy.push_back (nodesXY[cur * 2]);
        chain.xy.push_back (nodesXY[cur * 2 + 1]);
        for (;;) {
            uint32_t via = UINT32_MAX;
            for (uint32_t e : adj[cur]) {
                if (!used[e]) {
                    via = e;
                    break;
                }
            }
            if (via == UINT32_MAX)
                break;
            used[via] = true;
            cur = other (via, cur);
            if (cur == start) {      // returned home -> closed ring
                chain.closed = true; // ⚠️ the repeat is NOT stored;
                break;               //   SliceChain has no closing point
            }
            chain.xy.push_back (nodesXY[cur * 2]);
            chain.xy.push_back (nodesXY[cur * 2 + 1]);
        }
        return chain;
    };

    // 1) Open chains first, from their loose (odd-degree) endpoints. Walking a
    //    closed loop first could consume an edge an open chain needed, and the
    //    open chain would then be reported as two fragments.
    for (uint32_t n = 0; n < nNodes; ++n) {
        if (adj[n].size () % 2 == 0)
            continue;
        for (;;) {
            bool anyFree = false;
            for (uint32_t e : adj[n]) {
                if (!used[e]) {
                    anyFree = true;
                    break;
                }
            }
            if (!anyFree)
                break;
            SliceChain chain = walk (n);
            if (chain.Count () >= 2)
                out.push_back (std::move (chain));
        }
    }
    // 2) Whatever is left is made of closed loops.
    for (uint32_t e = 0; e < edges.size (); ++e) {
        if (used[e])
            continue;
        SliceChain chain = walk (edges[e].a);
        if (chain.Count () >= 2)
            out.push_back (std::move (chain));
    }
    return out;
}

size_t BuildSliceRibbon (const std::vector<SliceChain>& chains, float z, std::vector<StorySliceVertex>& out)
{
    const size_t before = out.size ();

    for (const SliceChain& chain : chains) {
        const size_t n = chain.Count ();
        if (n < 2)
            continue;

        const size_t segCount = chain.closed ? n : n - 1;
        double arc = 0.0;

        for (size_t i = 0; i < segCount; ++i) {
            const size_t k = (i + 1) % n;
            const double x0 = chain.xy[i * 2], y0 = chain.xy[i * 2 + 1];
            const double x1 = chain.xy[k * 2], y1 = chain.xy[k * 2 + 1];
            const double dx = x1 - x0, dy = y1 - y0;
            const double len = std::sqrt (dx * dx + dy * dy);
            if (len <= 0.0)
                continue;

            const float tx = static_cast<float> (dx / len);
            const float ty = static_cast<float> (dy / len);
            const float nx = -ty; // unit normal, model space
            const float ny = tx;

            const float arc0 = static_cast<float> (arc);
            const float arc1 = static_cast<float> (arc + len);
            arc += len;

            // Four corners of the quad. `tx,ty` is negated at the start cap and
            // kept at the end cap, so both ends grow OUTWARD by half a width.
            const StorySliceVertex a {
                static_cast<float> (x0), static_cast<float> (y0), z, nx, ny, -tx, -ty, arc0, 1.0f
            };
            const StorySliceVertex b {
                static_cast<float> (x0), static_cast<float> (y0), z, -nx, -ny, -tx, -ty, arc0, -1.0f
            };
            const StorySliceVertex c {
                static_cast<float> (x1), static_cast<float> (y1), z, nx, ny, tx, ty, arc1, 1.0f
            };
            const StorySliceVertex d {
                static_cast<float> (x1), static_cast<float> (y1), z, -nx, -ny, tx, ty, arc1, -1.0f
            };

            out.push_back (a);
            out.push_back (b);
            out.push_back (c);
            out.push_back (c);
            out.push_back (b);
            out.push_back (d);
        }
    }
    return out.size () - before;
}

namespace {

// One edge of a closed ring, oriented so y0 < y1. The fill only ever asks where
// an edge crosses a horizontal line, and a consistent orientation makes that one
// expression instead of two.
struct FillEdge {
    double x0, y0, x1, y1;

    double XAt (double y) const
    {
        const double dy = y1 - y0;
        if (dy == 0.0)
            return x0;
        return x0 + (x1 - x0) * (y - y0) / dy;
    }
};

} // namespace

size_t BuildSliceFill (const std::vector<SliceChain>& chains, float z, std::vector<StorySliceFillVertex>& out)
{
    const size_t before = out.size ();

    std::vector<FillEdge> edges;
    std::vector<double> ys;
    for (const SliceChain& chain : chains) {
        if (!chain.closed)
            continue; // an open chain bounds nothing
        const size_t n = chain.Count ();
        if (n < 3)
            continue;
        for (size_t i = 0; i < n; ++i) {
            const size_t k = (i + 1) % n;
            const double ax = chain.xy[i * 2], ay = chain.xy[i * 2 + 1];
            const double bx = chain.xy[k * 2], by = chain.xy[k * 2 + 1];
            if (ay == by)
                continue; // horizontal: crosses no scanline
            FillEdge e = (ay < by) ? FillEdge { ax, ay, bx, by } : FillEdge { bx, by, ax, ay };
            edges.push_back (e);
            ys.push_back (ay);
            ys.push_back (by);
        }
    }
    if (edges.empty ())
        return 0;

    std::sort (ys.begin (), ys.end ());
    ys.erase (std::unique (ys.begin (), ys.end ()), ys.end ());

    std::vector<std::pair<double, const FillEdge*>> crossings;
    for (size_t band = 0; band + 1 < ys.size (); ++band) {
        const double yLo = ys[band];
        const double yHi = ys[band + 1];
        if (yHi - yLo <= 0.0)
            continue;
        const double yMid = (yLo + yHi) * 0.5;

        // ⚠️ SORTED BY THE MIDPOINT X, NOT BY THE X AT EITHER EDGE OF THE BAND.
        // Two edges can share an endpoint exactly on `yLo` and still be in a
        // definite left-right order everywhere strictly inside the band. Sorting
        // on a shared value is a coin flip, and a flipped pair fills the gap
        // between two regions instead of the regions themselves.
        crossings.clear ();
        for (const FillEdge& e : edges) {
            if (e.y0 <= yLo && e.y1 >= yHi)
                crossings.emplace_back (e.XAt (yMid), &e);
        }
        if (crossings.size () < 2)
            continue;
        std::sort (crossings.begin (), crossings.end (),
                   [] (const std::pair<double, const FillEdge*>& a, const std::pair<double, const FillEdge*>& b) {
                       return a.first < b.first;
                   });

        // Even-odd: consecutive PAIRS of crossings bound the inside.
        for (size_t i = 0; i + 1 < crossings.size (); i += 2) {
            const FillEdge* left = crossings[i].second;
            const FillEdge* right = crossings[i + 1].second;
            const double lLo = left->XAt (yLo), lHi = left->XAt (yHi);
            const double rLo = right->XAt (yLo), rHi = right->XAt (yHi);
            if (rLo - lLo <= 0.0 && rHi - lHi <= 0.0)
                continue; // degenerate sliver

            const StorySliceFillVertex a { static_cast<float> (lLo), static_cast<float> (yLo), z };
            const StorySliceFillVertex b { static_cast<float> (rLo), static_cast<float> (yLo), z };
            const StorySliceFillVertex c { static_cast<float> (rHi), static_cast<float> (yHi), z };
            const StorySliceFillVertex d { static_cast<float> (lHi), static_cast<float> (yHi), z };

            out.push_back (a);
            out.push_back (b);
            out.push_back (c);
            out.push_back (a);
            out.push_back (c);
            out.push_back (d);
        }
    }
    return out.size () - before;
}

bool BuildIndexedSliceFill (const std::vector<SliceChain>& chains, double z, StorySliceFillMesh& out,
                            std::string& error, int decimalPrecision)
{
    namespace cp = Clipper2Lib;
    if (decimalPrecision < 0 || decimalPrecision > 8) {
        error = "slice fill decimal precision must be within 0..8";
        return false;
    }
    if (!std::isfinite (z)) {
        error = "slice fill elevation must be finite";
        return false;
    }
    cp::PathsD paths;
    double originX = 0, originY = 0;
    for (const auto& chain : chains) {
        if (!chain.closed)
            continue;
        if (chain.xy.size () % 2 != 0) {
            error = "slice fill requires complete XY pairs";
            return false;
        }
        for (double value : chain.xy) {
            if (!std::isfinite (value) || std::abs (value) > 1e9) {
                error = "slice fill coordinates must be finite and within +/-1e9 metres";
                return false;
            }
        }
        if (chain.Count () < 3)
            continue;
        if (paths.empty ()) {
            originX = chain.xy[0];
            originY = chain.xy[1];
        }
        cp::PathD path;
        path.reserve (chain.Count ());
        for (size_t i = 0; i < chain.Count (); ++i)
            path.emplace_back (chain.xy[2 * i] - originX, chain.xy[2 * i + 1] - originY);
        paths.push_back (std::move (path));
    }

    cp::ClipperD clipper (decimalPrecision);
    cp::PolyTreeD tree;
    clipper.AddSubject (paths);
    if (!clipper.Execute (cp::ClipType::Union, cp::FillRule::EvenOdd, tree)) {
        error = "slice fill even-odd normalization refused";
        return false;
    }
    StorySliceFillMesh result;
    // Each non-hole owns only its immediate hole children. Grandchildren are
    // separate filled islands, never extra holes passed to the triangulator.
    const auto collect = [&] (const auto& self, const cp::PolyPathD& node) -> bool {
        if (!node.Polygon ().empty () && !node.IsHole ()) {
            std::vector<std::vector<double>> coordinates;
            const auto append = [&] (const cp::PathD& path) {
                std::vector<double> xy;
                xy.reserve (path.size () * 2);
                for (const auto& point : path)
                    xy.insert (xy.end (), { point.x, point.y });
                coordinates.push_back (std::move (xy));
            };
            append (node.Polygon ());
            for (const auto& child : node)
                if (child->IsHole ())
                    append (child->Polygon ());
            std::vector<engine::PlanarRing> rings;
            size_t count = 0;
            for (const auto& xy : coordinates) {
                rings.push_back ({ xy.data (), xy.size () / 2 });
                count += xy.size () / 2;
            }
            engine::PlanarTriangulation cover;
            if (!engine::TriangulatePlanar (rings, false, cover, error))
                return false;
            // Many valid holes take the constrained route; the adapter checks
            // their declared area. A repaired single outer has no such gate.
            if ((cover.cover != engine::PlanarCover::Exact &&
                 !(cover.cover == engine::PlanarCover::Repaired && rings.size () > 1)) ||
                !cover.generatedXY.empty ()) {
                error = "slice fill requires an exact normalized cover without generated crossings";
                return false;
            }
            const size_t base = result.points.size () / 3;
            if (count > (std::numeric_limits<uint32_t>::max) () - base) {
                error = "slice fill exceeds the shared vertex index range";
                return false;
            }
            for (const auto& xy : coordinates)
                for (size_t i = 0; i < xy.size (); i += 2)
                    result.points.insert (result.points.end (), { xy[i] + originX, xy[i + 1] + originY, z });
            for (uint32_t index : cover.triangles)
                result.indices.push_back (static_cast<uint32_t> (base + index));
        }
        for (const auto& child : node)
            if (!self (self, *child))
                return false;
        return true;
    };
    if (!collect (collect, tree))
        return false;
    out = std::move (result);
    error.clear ();
    return true;
}

bool MeasureSliceFill (const StorySliceFillMesh& mesh, double& area, std::string& error)
{
    const auto fail = [&] (const char* reason) {
        error = reason;
        return false;
    };
    if (mesh.points.size () % 3 || mesh.indices.size () % 3)
        return fail ("slice fill measurement requires complete XYZ points and triangles");
    for (double coordinate : mesh.points)
        if (!std::isfinite (coordinate))
            return fail ("slice fill measurement requires finite coordinates");
    for (uint32_t index : mesh.indices)
        if (index >= mesh.points.size () / 3)
            return fail ("slice fill measurement index is out of range");
    double total = 0, correction = 0;
    for (size_t i = 0; i < mesh.indices.size (); i += 3) {
        const size_t a = size_t (mesh.indices[i]) * 3, b = size_t (mesh.indices[i + 1]) * 3,
                     c = size_t (mesh.indices[i + 2]) * 3;
        const double cross = (mesh.points[b] - mesh.points[a]) * (mesh.points[c + 1] - mesh.points[a + 1]) -
                             (mesh.points[c] - mesh.points[a]) * (mesh.points[b + 1] - mesh.points[a + 1]);
        if (!std::isfinite (cross) || cross <= 0)
            return fail ("slice fill measurement requires positive finite triangles");
        const double value = cross * 0.5 - correction, next = total + value;
        correction = (next - total) - value;
        total = next;
    }
    if (!std::isfinite (total))
        return fail ("slice fill measured area is not finite");
    area = total;
    error.clear ();
    return true;
}

double UnionArea (const std::vector<SliceChain>& chains)
{
    StorySliceFillMesh mesh;
    std::string error;
    double area = std::numeric_limits<double>::quiet_NaN ();
    if (!BuildIndexedSliceFill (chains, 0, mesh, error) || !MeasureSliceFill (mesh, area, error))
        return std::numeric_limits<double>::quiet_NaN ();
    return area;
}

} // namespace archviz
} // namespace geomsrv
