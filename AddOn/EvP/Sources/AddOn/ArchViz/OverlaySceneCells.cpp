// Visible, model-sized plan cell values. Kept in the view-dependent stream so a large
// analysis does not upload hundreds of thousands of offscreen glyphs on every edit.
#include "ArchViz/OverlaySceneBuilder.hpp"
#include "ArchViz/OverlayHud.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace geomsrv::archviz::overlayscene::build {
namespace {

using Bounds = std::array<double, 4>;
using Point = std::array<double, 2>;

bool CellBounds (const layers::Mesh& mesh, const layers::HoverCell& cell, Bounds& bounds)
{
    if (cell.boundsKnown) {
        std::copy (std::begin (cell.planBounds), std::end (cell.planBounds), bounds.begin ());
        return true;
    }
    bool found = false;
    for (const uint32_t triangle : cell.triangles) {
        if (size_t (triangle) * 3 + 2 >= mesh.indices.size ())
            return false;
        for (size_t k = 0; k < 3; ++k) {
            const size_t p = size_t (mesh.indices[size_t (triangle) * 3 + k]) * 3;
            if (p + 2 >= mesh.points.size ())
                return false;
            const double x = mesh.points[p], y = mesh.points[p + 1];
            if (!found) {
                bounds = { x, y, x, y };
                found = true;
            }
            bounds[0] = (std::min) (bounds[0], x);
            bounds[1] = (std::min) (bounds[1], y);
            bounds[2] = (std::max) (bounds[2], x);
            bounds[3] = (std::max) (bounds[3], y);
        }
    }
    return found;
}

// Sum triangle coverage of the label rectangle, not just membership of its corners:
// a thin seam or a disconnected patch through the middle must hide the whole value.
bool CoversLabel (const layers::Mesh& mesh, const layers::HoverCell& cell, const Bounds& box)
{
    double area = 0;
    for (const uint32_t triangle : cell.triangles) {
        std::vector<Point> polygon;
        for (size_t k = 0; k < 3; ++k) {
            const size_t p = size_t (mesh.indices[size_t (triangle) * 3 + k]) * 3;
            polygon.push_back ({ mesh.points[p] - box[0], mesh.points[p + 1] - box[1] });
        }
        for (int edge = 0; edge < 4 && !polygon.empty (); ++edge) {
            const int axis = edge % 2;
            const double boundary = edge < 2 ? 0 : box[axis + 2] - box[axis];
            const auto distance = [&] (const Point& p) { return edge < 2 ? p[axis] : boundary - p[axis]; };
            std::vector<Point> clipped;
            Point previous = polygon.back ();
            double previousD = distance (previous);
            for (const Point& current : polygon) {
                const double d = distance (current);
                if ((d >= 0) != (previousD >= 0)) {
                    const double t = previousD / (previousD - d);
                    clipped.push_back (
                        { previous[0] + (current[0] - previous[0]) * t, previous[1] + (current[1] - previous[1]) * t });
                }
                if (d >= 0)
                    clipped.push_back (current);
                previous = current;
                previousD = d;
            }
            polygon = std::move (clipped);
        }
        double twiceArea = 0;
        for (size_t k = 0; k < polygon.size (); ++k) {
            const Point& a = polygon[k];
            const Point& b = polygon[(k + 1) % polygon.size ()];
            twiceArea += a[0] * b[1] - a[1] * b[0];
        }
        area += std::abs (twiceArea) * 0.5;
    }
    const double required = (box[2] - box[0]) * (box[3] - box[1]);
    return required > 0 && std::abs (area - required) <= required * 1e-6;
}

} // namespace

void Builder::AddCellLabels (const layers::Layer& layer, const overlayhud::Input& input)
{
    if (!input.planar || input.width <= 0 || input.height <= 0)
        return;
    const double pixelsPerMetre =
        (std::min) (std::hypot (input.plan[0], input.plan[3]), std::hypot (input.plan[1], input.plan[4]));
    if (!(pixelsPerMetre > 0))
        return;
    const auto onscreen = [&] (double x, double y) {
        const double px = input.plan[0] * x + input.plan[1] * y + input.plan[2];
        const double py = input.plan[3] * x + input.plan[4] * y + input.plan[5];
        return std::isfinite (px) && std::isfinite (py) && px >= 0 && py >= 0 && px <= input.width &&
               py <= input.height;
    };
    for (auto it = layer.meshes.rbegin (); it != layer.meshes.rend (); ++it) {
        const auto& mesh = *it;
        if (!mesh.cellLabels || !layers::DrawnByGuest (mesh, layer))
            continue;
        double minimum = mesh.colormap.min, maximum = mesh.colormap.max;
        if (mesh.colormap.autoRange && !mesh.values.empty ()) {
            const auto range = std::minmax_element (mesh.values.begin (), mesh.values.end ());
            minimum = *range.first;
            maximum = *range.second;
        }
        for (auto cellIt = mesh.hoverCells.rbegin (); cellIt != mesh.hoverCells.rend (); ++cellIt) {
            const auto& cell = *cellIt;
            Bounds bounds;
            if (!CellBounds (mesh, cell, bounds) || !onscreen (cell.center[0], cell.center[1]))
                continue;
            const double em = (std::min) (bounds[2] - bounds[0], bounds[3] - bounds[1]) * 0.22;
            if (em * pixelsPerMetre < 14 * scale_)
                continue;
            const double value = std::abs (cell.to[2] - cell.from[2]);
            layers::Text text;
            text.text = FormatLength (value, 2, layers::LengthUnit::Metres, false);
            text.sizePixels = 24;
            text.sizeMetres = em;
            text.planar = true;
            text.minProjectedPixels = 14;
            text.direction[0] = 1;
            text.normal[2] = 1;
            std::copy (std::begin (cell.center), std::end (cell.center), text.at);
            text.rgba = RampTextColor (mesh.colormap, value, minimum, maximum);
            text.haloPixels = 0;
            text.backgroundRgba = 0;
            text.behind = layers::Behind::Show;
            overlaytext::Label label;
            if (!LayOut (text.text, text.sizePixels, text.align, text.baseline, label, text.font))
                continue;
            const double m = em / text.sizePixels;
            const Bounds box { cell.center[0] + label.left * m, cell.center[1] - label.bottom * m,
                               cell.center[0] + label.right * m, cell.center[1] - label.top * m };
            if (box[0] < bounds[0] || box[1] < bounds[1] || box[2] > bounds[2] || box[3] > bounds[3])
                continue;
            bool visible = true;
            Bounds screen { input.width, input.height, 0, 0 };
            for (const double x : { box[0], box[2] })
                for (const double y : { box[1], box[3] }) {
                    visible = visible && onscreen (x, y);
                    const double px = input.plan[0] * x + input.plan[1] * y + input.plan[2];
                    const double py = input.plan[3] * x + input.plan[4] * y + input.plan[5];
                    screen[0] = (std::min) (screen[0], px - 2 * scale_);
                    screen[1] = (std::min) (screen[1], py - 2 * scale_);
                    screen[2] = (std::max) (screen[2], px + 2 * scale_);
                    screen[3] = (std::max) (screen[3], py + 2 * scale_);
                }
            if (!visible || !CoversLabel (mesh, cell, box))
                continue;
            const int left = int (std::floor (screen[0] / 64)), right = int (std::floor (screen[2] / 64));
            const int top = int (std::floor (screen[1] / 64)), bottom = int (std::floor (screen[3] / 64));
            bool overlaps = false;
            for (int x = left; x <= right && !overlaps; ++x)
                for (int y = top; y <= bottom && !overlaps; ++y) {
                    const auto bin = planLabelBins_.find ({ x, y });
                    if (bin != planLabelBins_.end ())
                        overlaps = std::any_of (bin->second.begin (), bin->second.end (), [&] (const Bounds& other) {
                            return screen[0] < other[2] && screen[2] > other[0] && screen[1] < other[3] &&
                                   screen[3] > other[1];
                        });
                }
            if (overlaps)
                continue;
            if (draft_.glyphs.size () + label.quads.size () * 6 > kMaxGlyphVertices) {
                Truncated ();
                return;
            }
            // The hovered cell's value is drawn once, above the pointer (AddHud), not here under
            // it where the pointer covers it. Its place is still taken: a coincident cell beneath
            // must not print its own value where the hovered one is drawn.
            if (input.hover.hasMeasuredCell && !input.overHud && input.hover.layer == layer.name &&
                std::equal (std::begin (cell.center), std::end (cell.center), std::begin (input.hover.cellCenter)))
                planHoverLabelHidden_ = true;
            else
                AddPlanarText (layer, text, label);
            for (int x = left; x <= right; ++x)
                for (int y = top; y <= bottom; ++y)
                    planLabelBins_[{ x, y }].push_back (screen);
        }
    }
}
} // namespace geomsrv::archviz::overlayscene::build
