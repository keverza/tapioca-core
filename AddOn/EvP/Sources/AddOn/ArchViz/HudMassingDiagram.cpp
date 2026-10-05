#include "ArchViz/HudMassingDiagram.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::hudmassingrules {
void DrawDiagramOffset (ImDrawList& draw, const std::vector<ScreenPoint>& points, float fontSize,
                        ProjectedDrawList* occupied)
{
    if (points.size () < 3 || !std::isfinite (fontSize) || fontSize <= 0)
        return;
    const double dash = 5 * fontSize / 13, period = 8 * fontSize / 13;
    double total = 0;
    for (size_t i = 0; i < points.size (); ++i) {
        const auto a = points[i], b = points[(i + 1) % points.size ()];
        if (!std::isfinite (a.x) || !std::isfinite (a.y))
            return;
        total += std::hypot (double (b.x) - a.x, double (b.y) - a.y);
    }
    if (!std::isfinite (total) || total > 8192 * period)
        return;
    double arc = 0;
    for (size_t i = 0; i < points.size (); ++i) {
        const auto a = points[i], b = points[(i + 1) % points.size ()];
        const double dx = double (b.x) - a.x, dy = double (b.y) - a.y, length = std::hypot (dx, dy);
        if (length < 1e-6)
            continue;
        if (occupied)
            occupied->lines.push_back ({ a, b, IM_COL32 (166, 98, 38, 255), 2 });
        for (double start = std::floor (arc / period) * period; start < arc + length; start += period) {
            const double low = (std::max) (arc, start), high = (std::min) (arc + length, start + dash);
            if (high <= low)
                continue;
            const auto at = [&] (double position) {
                const double t = (position - arc) / length;
                return ImVec2 { float (a.x + t * dx), float (a.y + t * dy) };
            };
            draw.AddLine (at (low), at (high), IM_COL32 (166, 98, 38, 255), 2);
        }
        arc += length;
    }
}
} // namespace geomsrv::archviz::hudmassingrules
