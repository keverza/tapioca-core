#ifndef EVP_ARCHVIZ_BUILDINGTOPOLOGYDETAIL_HPP
#define EVP_ARCHVIZ_BUILDINGTOPOLOGYDETAIL_HPP

// Shared by the BuildingTopology translation units only.
#include "ArchViz/BuildingTopology.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::buildingtopology::detail {
namespace cp = Clipper2Lib;
constexpr int kPrecision = 6;

inline cp::PathD ToPath (const Ring& ring)
{
    cp::PathD path;
    path.reserve (ring.size ());
    for (const auto& p : ring)
        path.emplace_back (p.x, p.y);
    return path;
}
inline cp::PathsD ToPaths (const std::vector<Ring>& rings)
{
    cp::PathsD out;
    for (const auto& ring : rings)
        out.push_back (ToPath (ring));
    return out;
}
inline Ring FromPath (const cp::PathD& path)
{
    Ring ring;
    ring.reserve (path.size ());
    for (const auto& p : path)
        ring.push_back ({ p.x, p.y });
    return ring;
}
inline std::vector<Ring> FromPaths (const cp::PathsD& paths)
{
    std::vector<Ring> out;
    for (const auto& path : paths)
        out.push_back (FromPath (path));
    return out;
}
inline Ring Counter (Ring ring)
{
    if (floorscheme::Area (ring) < 0)
        std::reverse (ring.begin (), ring.end ());
    return ring;
}
inline double Area (const cp::PathsD& paths)
{
    double sum = 0;
    for (const auto& p : paths)
        sum += cp::Area (p);
    return sum;
}
inline double Dot (Vec a, Vec b)
{
    return a.x * b.x + a.y * b.y;
}
inline double Cross (Vec a, Vec b)
{
    return a.x * b.y - a.y * b.x;
}
inline Vec Direction (Vec a)
{
    const double l = std::hypot (a.x, a.y);
    return l > 1e-12 ? Vec { a.x / l, a.y / l } : Vec { 1, 0 };
}
// Winding number of `path` round `p` (counter-clockwise positive). Clipper2's PointInPolygon
// reads a point a few centimetres outside a double path as on it, so it is not used here.
inline int Winding (const cp::PathD& path, Vec p)
{
    int w = 0;
    for (size_t i = 0; i < path.size (); ++i) {
        const auto& a = path[i];
        const auto& b = path[(i + 1) % path.size ()];
        const double side = (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);
        if (a.y <= p.y) {
            if (b.y > p.y && side > 0)
                ++w;
        }
        else if (b.y <= p.y && side < 0)
            --w;
    }
    return w;
}
// Inside, with the paths' non-zero winding (holes clockwise).
inline bool Inside (const cp::PathsD& paths, Vec p)
{
    int w = 0;
    for (const auto& path : paths)
        w += Winding (path, p);
    return w != 0;
}

// BuildingTopology.cpp: one floor's cells, appended to the complex.
void AddCells (Complex& complex, int floor, const FloorInput& input, const std::vector<floorscheme::Pins::Core>& stack);
// BuildingTopologyBoundaries.cpp: one floor's faces and apertures, from its cells.
void AddFaces (Complex& complex, int floor, const FloorInput& input);
} // namespace geomsrv::archviz::buildingtopology::detail
#endif
