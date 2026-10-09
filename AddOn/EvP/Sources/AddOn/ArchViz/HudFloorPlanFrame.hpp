#ifndef EVP_ARCHVIZ_HUDFLOORPLANFRAME_HPP
#define EVP_ARCHVIZ_HUDFLOORPLANFRAME_HPP

// The quick plan's building frame, shared by its translation units only: world XY to the
// frame (origin at the longest edge, x along it) and back, and Clipper paths in the frame.
#include "ArchViz/HudBuildingPlan.hpp"
#include <clipper2/clipper.h>
#include <cmath>

namespace geomsrv::archviz::buildingplan::frame {
namespace cp = Clipper2Lib;
constexpr double kCorridor = 1.8, kModule = 0.3, kMinWidth = 3.4;
constexpr size_t kMaxUnits = 128;
struct Rect {
    double x0, y0, x1, y1;
};
inline cp::PathD Polygon (Rect r)
{
    return { { r.x0, r.y0 }, { r.x1, r.y0 }, { r.x1, r.y1 }, { r.x0, r.y1 } };
}
inline Point Local (const QuickPlan& plan, Point p)
{
    const double x = p.x - plan.origin.x, y = p.y - plan.origin.y;
    return { x * std::cos (plan.angle) + y * std::sin (plan.angle),
             -x * std::sin (plan.angle) + y * std::cos (plan.angle) };
}
inline Point World (const QuickPlan& plan, Point p)
{
    return { plan.origin.x + p.x * std::cos (plan.angle) - p.y * std::sin (plan.angle),
             plan.origin.y + p.x * std::sin (plan.angle) + p.y * std::cos (plan.angle) };
}
inline cp::PathsD Paths (const QuickPlan& plan, const std::vector<SliceChain>& rings)
{
    cp::PathsD out;
    for (const auto& ring : rings) {
        cp::PathD path;
        for (size_t i = 0; i < ring.Count (); ++i) {
            const auto p = Local (plan, { ring.xy[i * 2], ring.xy[i * 2 + 1] });
            path.emplace_back (p.x, p.y);
        }
        out.push_back (std::move (path));
    }
    return out;
}
// Points on a boundary count as inside. Not Clipper's PointInPolygon: with MSVC its double
// cross-product sign goes through uint64 products, so differences below one unit (a metre
// here) truncate to zero and a point centimetres outside an edge reads as on it.
inline bool Inside (const cp::PathsD& paths, Point p)
{
    int winding = 0;
    for (const auto& path : paths) {
        bool inside = false;
        for (size_t i = 0, j = path.size () - 1; i < path.size (); j = i++) {
            const auto& a = path[j];
            const auto& b = path[i];
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double cross = dx * (p.y - a.y) - dy * (p.x - a.x);
            const double dot = (p.x - a.x) * dx + (p.y - a.y) * dy;
            if (std::abs (cross) <= 1e-9 * (std::max) (1.0, std::hypot (dx, dy)) && dot >= -1e-12 &&
                dot <= dx * dx + dy * dy + 1e-12)
                return true;
            if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + dx * (p.y - a.y) / dy)
                inside = !inside;
        }
        if (inside)
            winding += cp::Area (path) > 0 ? 1 : -1;
    }
    return winding != 0;
}
// A core's rectangle in the frame. Cores turn with the building frame, so it is axis-aligned.
inline Rect CoreRect (const QuickPlan& plan, const Core& core)
{
    const auto q = Local (plan, core.center);
    return { q.x - core.width / 2, q.y - core.depth / 2, q.x + core.width / 2, q.y + core.depth / 2 };
}
PlanRegion Region (const QuickPlan& plan, const cp::PathsD& paths);           // world rings and fill triangles
double FacadeLength (const PlanRegion& segment, double lo, double hi);        // outline length of a span
int TemplateStory (const Plan& plan, const Draft& draft, const Floor& floor); // shared design's floor
} // namespace geomsrv::archviz::buildingplan::frame
#endif
