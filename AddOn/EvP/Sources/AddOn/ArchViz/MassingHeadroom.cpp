#include "ArchViz/MassingHeadroom.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace geomsrv::archviz::massingheadroom {
namespace {
namespace cp = Clipper2Lib;
struct Point {
    double x, y, height;
};
std::vector<Point> Half (const std::vector<Point>& polygon, double limit, bool above)
{
    std::vector<Point> out;
    for (size_t i = 0; i < polygon.size (); ++i) {
        const auto& p = polygon[i];
        const auto& q = polygon[(i + 1) % polygon.size ()];
        const double a = above ? p.height - limit : limit - p.height;
        const double b = above ? q.height - limit : limit - q.height;
        if (a >= 0)
            out.push_back (p);
        if ((a < 0) != (b < 0)) {
            const double t = a / (a - b);
            out.push_back ({ p.x + t * (q.x - p.x), p.y + t * (q.y - p.y), limit });
        }
    }
    return out;
}
cp::PathD XY (const std::vector<Point>& polygon)
{
    cp::PathD path;
    for (const auto& p : polygon)
        path.emplace_back (p.x, p.y);
    return path;
}
std::vector<SliceChain> Chains (const cp::PathsD& paths, double ox, double oy)
{
    std::vector<SliceChain> out;
    for (const auto& path : paths) {
        SliceChain chain;
        chain.closed = true;
        for (const auto& p : path)
            chain.xy.insert (chain.xy.end (), { p.x + ox, p.y + oy });
        out.push_back (std::move (chain));
    }
    return out;
}
} // namespace
bool Partition (const std::vector<SliceChain>& source, double floorZ, const std::vector<double>& vertices,
                const std::vector<uint32_t>& indices, Split& result, size_t& work, std::string& error)
{
    error.clear ();
    const auto fail = [&] (const char* text) {
        error = text;
        return false;
    };
    if (!std::isfinite (floorZ) || std::abs (floorZ) > 1e9 || vertices.empty () || vertices.size () % 3 ||
        vertices.size () > 600000 || indices.empty () || indices.size () % 3 || indices.size () > 600000)
        return fail ("Headroom needs a bounded current closed roof body.");
    work += indices.size () / 3;
    if (work > 2000000)
        return fail ("Headroom partition exceeds its triangle-work budget.");
    for (double value : vertices)
        if (!std::isfinite (value) || std::abs (value) > 1e9)
            return fail ("Invalid headroom body coordinate.");
    for (uint32_t index : indices)
        if (index >= vertices.size () / 3)
            return fail ("Invalid headroom body index.");
    Split out;
    if (source.empty ()) {
        result = std::move (out);
        return true;
    }
    if (source[0].Count () < 3)
        return fail ("Invalid headroom floor contour.");
    const double ox = source[0].xy[0], oy = source[0].xy[1];
    cp::PathsD paths;
    size_t points = 0;
    for (const auto& chain : source) {
        if (!chain.closed || chain.xy.size () % 2 || chain.Count () < 3)
            return fail ("Headroom floor contour is open or invalid.");
        cp::PathD path;
        for (size_t i = 0; i < chain.Count (); ++i) {
            const double x = chain.xy[i * 2], y = chain.xy[i * 2 + 1];
            if (!std::isfinite (x) || !std::isfinite (y) || std::abs (x) > 1e9 || std::abs (y) > 1e9)
                return fail ("Invalid headroom floor coordinate.");
            path.emplace_back (x - ox, y - oy);
        }
        points += path.size ();
        if (points > 200000)
            return fail ("Headroom floor contour budget exceeded.");
        paths.push_back (std::move (path));
    }
    paths = cp::Union (paths, cp::FillRule::EvenOdd, 6);
    cp::PathsD low, covered;
    using Key = std::array<int64_t, 3>;
    std::map<std::pair<Key, Key>, unsigned> edges;
    double volume = 0;
    for (size_t i = 0; i < indices.size (); i += 3) {
        std::vector<Point> triangle;
        Key keys[3];
        for (size_t k = 0; k < 3; ++k) {
            const size_t at = size_t (indices[i + k]) * 3;
            triangle.push_back ({ vertices[at] - ox, vertices[at + 1] - oy, vertices[at + 2] - floorZ });
            keys[k] = { int64_t (std::llround (triangle.back ().x * 1e6)),
                        int64_t (std::llround (triangle.back ().y * 1e6)),
                        int64_t (std::llround (triangle.back ().height * 1e6)) };
        }
        const auto& a = triangle[0];
        const auto& b = triangle[1];
        const auto& c = triangle[2];
        volume += (a.x * (b.y * c.height - b.height * c.y) + a.y * (b.height * c.x - b.x * c.height) +
                   a.height * (b.x * c.y - b.y * c.x)) /
                  6;
        for (size_t k = 0; k < 3; ++k) {
            const auto& from = keys[k];
            const auto& to = keys[(k + 1) % 3];
            if (from != to)
                ++edges[from < to ? std::make_pair (from, to) : std::make_pair (to, from)];
        }
        if (cp::Area (XY (triangle)) <= 1e-10)
            continue; // Only upward exit faces: undersides/vertical walls are not the roof above a floor.
        auto roof = Half (triangle, -1e-7, true);
        if (roof.size () < 3)
            continue;
        covered.push_back (XY (roof));
        // A micron-scale boundary tolerance retains exactly 1.6 m even at survey Z.
        if (std::all_of (roof.begin (), roof.end (), [] (const auto& p) { return p.height >= kMinimum - 1e-7; }))
            continue;
        roof = Half (roof, kMinimum, false);
        if (roof.size () >= 3)
            low.push_back (XY (roof));
    }
    for (const auto& edge : edges)
        if (edge.second != 2)
            return fail ("Headroom awaits a closed manifold roof body; no partial partition shown.");
    if (!std::isfinite (volume) || volume <= 0)
        return fail ("Headroom needs an outward-wound solid roof body; no inverted-surface substitute used.");
    covered = cp::Union (covered, cp::FillRule::NonZero, 6);
    const auto missing = cp::Difference (paths, covered, cp::FillRule::NonZero, 6);
    double perimeter = 0;
    for (const auto& path : paths)
        perimeter += cp::Length (path, true);
    if (std::abs (cp::Area (missing)) > (std::max) (1e-6, 2e-6 * perimeter))
        return fail ("Headroom roof does not cover the complete floor; no guessed height used.");
    low = cp::Union (low, cp::FillRule::NonZero, 6);
    const auto excluded = cp::Intersect (paths, low, cp::FillRule::NonZero, 6);
    const auto counted = cp::Difference (paths, low, cp::FillRule::NonZero, 6);
    size_t retained = 0;
    for (const auto* collection : { &excluded, &counted })
        for (const auto& path : *collection)
            retained += path.size ();
    if (retained > 200000)
        return fail ("Headroom partition contour budget exceeded.");
    out.countedArea = std::abs (cp::Area (counted));
    out.excludedArea = std::abs (cp::Area (excluded));
    out.counted = Chains (counted, ox, oy);
    out.excluded = Chains (excluded, ox, oy);
    result = std::move (out);
    return true;
}
} // namespace geomsrv::archviz::massingheadroom
