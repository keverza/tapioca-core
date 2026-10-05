// ArchViz/StorySliceOverlayContent -- see the header.

#include "ArchViz/StorySliceOverlayContent.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace geomsrv {
namespace archviz {
namespace storysliceoverlay {

namespace layers = overlaylayers;

namespace {

double SignedArea (const SliceChain& chain)
{
    const size_t n = chain.Count ();
    double area = 0.0;
    for (size_t i = 0, j = n - 1; i < n; j = i++)
        area += chain.xy[j * 2] * chain.xy[i * 2 + 1] - chain.xy[i * 2] * chain.xy[j * 2 + 1];
    return area * 0.5;
}

// Even-odd over every closed contour of the slice: a hole is outside.
bool Inside (const std::vector<SliceChain>& contours, double x, double y)
{
    bool inside = false;
    for (const SliceChain& chain : contours) {
        if (!chain.closed)
            continue;
        const size_t n = chain.Count ();
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const double xi = chain.xy[i * 2], yi = chain.xy[i * 2 + 1];
            const double xj = chain.xy[j * 2], yj = chain.xy[j * 2 + 1];
            if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
                inside = !inside;
        }
    }
    return inside;
}

} // namespace

bool CornerAnchor (const std::vector<SliceChain>& contours, const SliceChain& chain, double inset, double& x, double& y)
{
    const size_t n = chain.Count ();
    if (!chain.closed || n < 3)
        return false;
    const double orientation = SignedArea (chain) >= 0.0 ? 1.0 : -1.0;
    std::vector<size_t> order (n);
    std::iota (order.begin (), order.end (), size_t (0));
    std::sort (order.begin (), order.end (), [&chain] (size_t a, size_t b) {
        return chain.xy[a * 2] + chain.xy[a * 2 + 1] < chain.xy[b * 2] + chain.xy[b * 2 + 1];
    });
    for (const size_t i : order) {
        const size_t p = (i + n - 1) % n, q = (i + 1) % n;
        const double vx = chain.xy[i * 2], vy = chain.xy[i * 2 + 1];
        double ax = chain.xy[p * 2] - vx, ay = chain.xy[p * 2 + 1] - vy;
        double bx = chain.xy[q * 2] - vx, by = chain.xy[q * 2 + 1] - vy;
        const double la = std::hypot (ax, ay), lb = std::hypot (bx, by);
        if (la < 1e-9 || lb < 1e-9)
            continue;
        // Convex: the boundary turns the polygon's own way at this vertex.
        if ((-ax * by + ay * bx) * orientation <= 1e-12)
            continue;
        ax /= la;
        ay /= la;
        bx /= lb;
        by /= lb;
        double sx = ax + bx, sy = ay + by;
        const double ls = std::hypot (sx, sy);
        if (ls < 1e-6)
            continue; // a straight run is no corner
        sx /= ls;
        sy /= ls;
        const double cx = vx + sx * inset, cy = vy + sy * inset;
        if (Inside (contours, cx, cy)) {
            x = cx;
            y = cy;
            return true;
        }
    }
    return false;
}

namespace {

// ⚠️ THE ROOM IS THE WHOLE STRAIGHT EDGE (the user, 2026-10-01: one floor's label smaller
// than the others'). From vertex `i` towards `step` (+1 the next vertex, -1 the previous)
// along (dx, dy): the edge, and those after it that go on the same way -- a vertex where the
// outline does not turn, which a cut through a body leaves, does not end it.
double StraightRun (const SliceChain& chain, size_t i, int step, double dx, double dy)
{
    const size_t n = chain.Count ();
    double length = 0.0;
    size_t at = i;
    for (size_t k = 0; k < n; ++k) {
        const size_t next = step > 0 ? (at + 1) % n : (at + n - 1) % n;
        const double ex = chain.xy[next * 2] - chain.xy[at * 2], ey = chain.xy[next * 2 + 1] - chain.xy[at * 2 + 1];
        const double l = std::hypot (ex, ey);
        if (k > 0 && l > 1e-9 && (std::fabs (ex * dy - ey * dx) > 1e-3 * l || ex * dx + ey * dy <= 0.0))
            break;
        length += l;
        at = next;
    }
    return length;
}

} // namespace

bool PlaceOnSlice (const std::vector<SliceChain>& contours, const SliceChain& chain, double inset, SlicePlacement& out)
{
    const size_t n = chain.Count ();
    if (!chain.closed || n < 3)
        return false;
    const double orientation = SignedArea (chain) >= 0.0 ? 1.0 : -1.0;
    std::vector<size_t> order (n);
    std::iota (order.begin (), order.end (), size_t (0));
    std::sort (order.begin (), order.end (), [&chain] (size_t a, size_t b) {
        return chain.xy[a * 2] + chain.xy[a * 2 + 1] < chain.xy[b * 2] + chain.xy[b * 2 + 1];
    });
    for (const size_t i : order) {
        const size_t p = (i + n - 1) % n, q = (i + 1) % n;
        const double vx = chain.xy[i * 2], vy = chain.xy[i * 2 + 1];
        double ax = chain.xy[p * 2] - vx, ay = chain.xy[p * 2 + 1] - vy;
        double bx = chain.xy[q * 2] - vx, by = chain.xy[q * 2 + 1] - vy;
        const double la = std::hypot (ax, ay), lb = std::hypot (bx, by);
        if (la < 1e-9 || lb < 1e-9 || (-ax * by + ay * bx) * orientation <= 1e-12)
            continue; // no edge, or not a convex corner
        ax /= la;
        ay /= la;
        bx /= lb;
        by /= lb;
        const double sx = ax + bx, sy = ay + by; // into the slice, for a convex corner
        // Of the corner's two edges, the one whose left side is the slice: text along
        // it rises into the slice and reads the right way up from above.
        const double candidates[2][3] = { { bx, by, StraightRun (chain, i, 1, bx, by) },
                                          { ax, ay, StraightRun (chain, i, -1, ax, ay) } };
        for (const auto& candidate : candidates) {
            const double dx = candidate[0], dy = candidate[1];
            const double ux = -dy, uy = dx;
            if (ux * sx + uy * sy <= 0.0)
                continue;
            const double x = vx + dx * inset + ux * inset, y = vy + dy * inset + uy * inset;
            if (!Inside (contours, x, y))
                continue;
            out.x = x;
            out.y = y;
            out.dx = dx;
            out.dy = dy;
            out.room = (std::max) (candidate[2] - 2.0 * inset, 0.0);
            return true;
        }
    }
    return false;
}

double LabelSizeMetres (double wanted, double areaM2, double room, const std::string& text)
{
    double size = wanted > 0.0 ? wanted : (std::min) ((std::max) (0.035 * std::sqrt (areaM2), 0.2), 1.2);
    size_t characters = 0;
    for (const unsigned char c : text)
        characters += (c & 0xC0u) != 0x80u ? 1 : 0;
    // About 0.55 em a character in the bundled font; the text keeps to 90 % of the edge.
    const double width = double (characters) * 0.55 * size;
    if (wanted <= 0.0 && room > 0.0 && width > 0.9 * room)
        size = (std::max) (size * 0.9 * room / width, 0.05);
    return size;
}

std::string AreaText (double areaM2, uint32_t decimals, const std::string& name, bool withName)
{
    char buffer[64] = {};
    std::snprintf (buffer, sizeof (buffer), "%.*f m\xC2\xB2", int ((std::min) (decimals, 6u)), areaM2);
    return withName && !name.empty () ? name + "  " + buffer : std::string (buffer);
}

std::vector<Slice> FromStoreys (const storeyslices::Snapshot& snapshot)
{
    std::vector<Slice> slices;
    for (const storeyslices::Storey& storey : snapshot.storeys) {
        Slice slice;
        slice.chains = storey.chains;
        slice.z = storey.level;
        slice.areaM2 = storey.areaM2;
        slice.name = storey.name;
        slice.storey = storey.index;
        slices.push_back (std::move (slice));
    }
    return slices;
}

uint32_t FillColour (const Controls& controls, size_t level, double z, double low, double high)
{
    if (!controls.fillColors.empty ())
        return controls.fillColors[level % controls.fillColors.size ()];
    const layers::Colormap& ramp = controls.fillColormap;
    if (ramp.stops.empty ())
        return controls.fillRgba;
    const double min = ramp.autoRange ? low : ramp.min, max = ramp.autoRange ? high : ramp.max;
    const float t = max > min ? float ((z - min) / (max - min)) : 0.0f;
    // The ramp's stops, piecewise linear, clamped at its ends -- as the heatmaps read it.
    const float at = (std::min) ((std::max) (t, 0.0f), 1.0f);
    const std::vector<layers::ColourStop>& stops = ramp.stops;
    if (at <= stops.front ().at)
        return stops.front ().rgba;
    for (size_t k = 1; k < stops.size (); ++k) {
        if (at > stops[k].at)
            continue;
        const float span = stops[k].at - stops[k - 1].at;
        const float f = span > 1e-9f ? (at - stops[k - 1].at) / span : 1.0f;
        uint32_t rgba = 0;
        for (int shift = 24; shift >= 0; shift -= 8) {
            const float a = float ((stops[k - 1].rgba >> shift) & 0xFFu), b = float ((stops[k].rgba >> shift) & 0xFFu);
            rgba |= uint32_t (std::lround (a + (b - a) * f)) << shift;
        }
        return rgba;
    }
    return stops.back ().rgba;
}

Built BuildLayer (const std::vector<Slice>& slices, const Controls& controls)
{
    Built out;
    out.layer.name = kLayerName;
    out.layer.views = controls.views;
    out.layer.occlusion = layers::Behind::Hide;
    // The heights drawn, lowest first: what a floor's colour is counted by.
    auto shown = [&controls] (const Slice& slice) {
        return controls.storeys.empty () ||
               std::find (controls.storeys.begin (), controls.storeys.end (), slice.storey) != controls.storeys.end ();
    };
    // ⚠️ ONE SIZE FOR EVERY LABEL LYING ON A SLICE (the user, 2026-10-01: "make sure font size
    // on all slabs is same size"): each is fitted to its slice and edge, then all take the
    // smallest, so every one still fits. A size asked for is already everyone's.
    std::vector<size_t> onSlice;
    double uniform = 0.0;
    std::vector<double> heights;
    for (const Slice& slice : slices)
        if (shown (slice))
            heights.push_back (slice.z);
    std::sort (heights.begin (), heights.end ());
    heights.erase (
        std::unique (heights.begin (), heights.end (), [] (double a, double b) { return std::fabs (a - b) < 1e-6; }),
        heights.end ());
    for (const Slice& slice : slices) {
        if (!shown (slice))
            continue;
        const double z = slice.z + controls.liftMetres;
        size_t level = 0;
        while (level + 1 < heights.size () && heights[level] < slice.z - 1e-6)
            ++level;
        const uint32_t fill = FillColour (controls, level, slice.z, heights.empty () ? slice.z : heights.front (),
                                          heights.empty () ? slice.z : heights.back ());

        if ((fill & 0xFFu) != 0 && controls.fillOpacity > 0.0f) {
            std::vector<StorySliceFillVertex> triangles;
            BuildSliceFill (slice.chains, float (z), triangles);
            if (triangles.size () >= 3) {
                layers::Mesh mesh;
                for (const StorySliceFillVertex& v : triangles) {
                    mesh.indices.push_back (uint32_t (mesh.points.size () / 3));
                    mesh.points.insert (mesh.points.end (), { double (v.x), double (v.y), z });
                }
                mesh.rgba = fill;
                mesh.styled = true;
                mesh.style.opacity = controls.fillOpacity;
                mesh.style.behind = controls.fillBehind;
                // What hover mode says over it (OverlayHover.hpp): the slice's figures.
                char level[48] = {};
                std::snprintf (level, sizeof (level), "%+.2f m", slice.z);
                mesh.hoverTitle = slice.name.empty () ? std::string ("Slice") : slice.name;
                mesh.hoverRows = { { "Area", AreaText (slice.areaM2, controls.decimals, std::string (), false) },
                                   { "Storey", std::to_string (slice.storey) },
                                   { "Cut at", level } };
                out.layer.meshes.push_back (std::move (mesh));
            }
        }

        for (const SliceChain& chain : slice.chains) {
            if (chain.Count () < 2)
                continue;
            layers::Polyline outline;
            for (size_t i = 0; i < chain.Count (); ++i)
                outline.points.insert (outline.points.end (), { chain.xy[i * 2], chain.xy[i * 2 + 1], z });
            outline.closed = chain.closed;
            outline.rgba = controls.outlineRgba;
            outline.widthPixels = controls.outlineWidthPixels;
            outline.dashMetres = controls.outlineDashMetres;
            outline.hidden = controls.outlineHidden;
            // Never Layer: the guest draws it, visible part solid, hidden part per `behind`.
            outline.behind =
                controls.outlineBehind == layers::Behind::Layer ? layers::Behind::Dash : controls.outlineBehind;
            out.layer.polylines.push_back (std::move (outline));
        }

        if (controls.label) {
            const SliceChain* largest = nullptr;
            double largestArea = 0.0;
            for (const SliceChain& chain : slice.chains) {
                if (!chain.closed || chain.Count () < 3)
                    continue;
                const double area = std::fabs (SignedArea (chain));
                if (area > largestArea) {
                    largestArea = area;
                    largest = &chain;
                }
            }
            layers::Text label;
            label.text = AreaText (slice.areaM2, controls.decimals, slice.name, controls.labelName);
            label.rgba = controls.labelRgba;
            label.haloRgba = controls.labelHaloRgba;
            label.haloPixels = controls.labelHaloPixels;
            label.font = controls.labelFont;
            label.behind = layers::Behind::Show;
            label.at[2] = z;
            label.minProjectedPixels = controls.labelMinProjectedPixels;
            if (controls.labelOnSlice) {
                // Lying on the slice, along an edge, from just inside a corner.
                const double guess = LabelSizeMetres (controls.labelSizeMetres, largestArea, 0.0, label.text);
                SlicePlacement place;
                if (largest != nullptr &&
                    PlaceOnSlice (slice.chains, *largest, (std::max) (0.25, 0.5 * guess), place)) {
                    label.planar = true;
                    label.at[0] = place.x;
                    label.at[1] = place.y;
                    label.direction[0] = place.dx;
                    label.direction[1] = place.dy;
                    label.direction[2] = 0.0;
                    label.sizeMetres = LabelSizeMetres (controls.labelSizeMetres, largestArea, place.room, label.text);
                    label.sizePixels = 32.0f; // the layout's resolution only
                    label.align = layers::Align::Left;
                    label.baseline = layers::Baseline::Bottom;
                    uniform = onSlice.empty () ? label.sizeMetres : (std::min) (uniform, label.sizeMetres);
                    onSlice.push_back (out.layer.texts.size ());
                    out.layer.texts.push_back (std::move (label));
                }
            }
            else {
                double x = 0.0, y = 0.0;
                const double inset = (std::min) ((std::max) (0.06 * std::sqrt (largestArea), 0.25), 1.5);
                if (largest != nullptr && CornerAnchor (slice.chains, *largest, inset, x, y)) {
                    label.at[0] = x;
                    label.at[1] = y;
                    label.sizePixels = controls.labelSizePixels;
                    label.sizeMetres = controls.labelSizeMetres > 0 ? controls.labelSizeMetres : 0.2;
                    out.layer.texts.push_back (std::move (label));
                }
            }
        }
        ++out.slices;
        out.areaM2 += slice.areaM2;
    }
    for (const size_t at : onSlice)
        out.layer.texts[at].sizeMetres = uniform;
    return out;
}

} // namespace storysliceoverlay
} // namespace archviz
} // namespace geomsrv
