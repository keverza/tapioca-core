// ArchViz/PlanOverlayContent -- see the header.

#include "ArchViz/PlanOverlayContent.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {
namespace plancontent {

namespace {

// TessellateEdge's thresholds, unchanged: below a tenth of a millimetre two points are
// one point, and a sweep or half-sweep this small is a straight edge.
constexpr double kMinSegmentMetres = 1e-4;
constexpr double kStraight = 1e-6;
// Consecutive points closer than this draw nothing but a square the stroke's width.
constexpr double kSamePointMetres = 1e-9;

} // namespace

void Split (double value, float& hi, float& lo)
{
    hi = float (value);
    lo = float (value - double (hi));
}

void TessellateEdgeDouble (double x0, double y0, double x1, double y1, double arcAngle, double arcSign,
                           double arcChordMetres, std::vector<double>& outXY)
{
    outXY.push_back (x0);
    outXY.push_back (y0);

    const double sweep = arcAngle * arcSign;
    if (std::fabs (sweep) < kStraight)
        return;
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const double chord = std::sqrt (dx * dx + dy * dy);
    if (chord < kMinSegmentMetres)
        return;
    const double halfSweep = sweep * 0.5;
    const double sinHalf = std::sin (halfSweep);
    if (std::fabs (sinHalf) < kStraight)
        return;
    const double radius = chord / (2.0 * sinHalf);

    // A counter-clockwise (positive) sweep has its centre on the chord's LEFT normal.
    const double leftX = -dy / chord;
    const double leftY = dx / chord;
    const double tanHalf = std::tan (halfSweep);
    const double offset = (std::fabs (tanHalf) < kStraight) ? 0.0 : (chord * 0.5) / tanHalf;
    const double cx = (x0 + x1) * 0.5 + leftX * offset;
    const double cy = (y0 + y1) * 0.5 + leftY * offset;

    const double absRadius = std::fabs (radius);
    const double arcLength = std::fabs (sweep) * absRadius;
    const double wanted = (arcChordMetres > 1e-4) ? (arcLength / arcChordMetres) : 1.0;
    const int steps = std::max (2, std::min (256, int (std::ceil (wanted))));

    const double startAngle = std::atan2 (y0 - cy, x0 - cx);
    for (int i = 1; i < steps; ++i) {
        const double angle = startAngle + sweep * (double (i) / double (steps));
        outXY.push_back (cx + absRadius * std::cos (angle));
        outXY.push_back (cy + absRadius * std::sin (angle));
    }
}

Content BuildContent (const std::vector<std::vector<double>>& rings, const std::vector<std::vector<double>>& arcs,
                      double arcSign, double arcChordMetres)
{
    Content content;
    double sumX = 0.0, sumY = 0.0;
    size_t points = 0;
    for (const std::vector<double>& ring : rings) {
        for (size_t i = 0; i + 1 < ring.size (); i += 2) {
            sumX += ring[i];
            sumY += ring[i + 1];
            ++points;
        }
    }
    if (points == 0)
        return content;
    content.originX = sumX / double (points);
    content.originY = sumY / double (points);

    std::vector<double> polyline;
    for (size_t r = 0; r < rings.size (); ++r) {
        const std::vector<double>& ring = rings[r];
        const size_t count = ring.size () / 2;
        if (count < 2)
            continue;
        const std::vector<double>* ringArcs = (r < arcs.size () && arcs[r].size () >= count) ? &arcs[r] : nullptr;
        polyline.clear ();
        for (size_t i = 0; i < count; ++i) {
            const size_t next = (i + 1) % count;
            TessellateEdgeDouble (ring[i * 2] - content.originX, ring[i * 2 + 1] - content.originY,
                                  ring[next * 2] - content.originX, ring[next * 2 + 1] - content.originY,
                                  ringArcs != nullptr ? (*ringArcs)[i] : 0.0, arcSign, arcChordMetres, polyline);
        }
        // Closed: each edge gave its start and its arc's interior, so the ring is the
        // points in order and back to the first.
        const size_t total = polyline.size () / 2;
        if (total < 2)
            continue;
        const size_t before = content.segments.size ();
        for (size_t i = 0; i < total; ++i) {
            const size_t next = (i + 1) % total;
            const double ax = polyline[i * 2], ay = polyline[i * 2 + 1];
            const double bx = polyline[next * 2], by = polyline[next * 2 + 1];
            if (std::fabs (bx - ax) < kSamePointMetres && std::fabs (by - ay) < kSamePointMetres)
                continue;
            Segment segment;
            Split (ax, segment.x0, segment.x0Lo);
            Split (ay, segment.y0, segment.y0Lo);
            Split (bx, segment.x1, segment.x1Lo);
            Split (by, segment.y1, segment.y1Lo);
            content.segments.push_back (segment);
        }
        if (content.segments.size () > before)
            ++content.rings;
    }
    return content;
}

bool MakeViewConstants (const PixelTransform& t, double originX, double originY, uint32_t width, uint32_t height,
                        ViewConstants& out)
{
    const double determinant = t.xx * t.yy - t.xy * t.yx;
    if (width == 0 || height == 0 || !std::isfinite (determinant) || std::fabs (determinant) < 1e-12)
        return false;

    // The model point under the buffer's centre, relative to the content origin.
    const double cx = double (width) * 0.5 - t.ox;
    const double cy = double (height) * 0.5 - t.oy;
    const double anchorX = (t.yy * cx - t.xy * cy) / determinant - originX;
    const double anchorY = (t.xx * cy - t.yx * cx) / determinant - originY;
    float hiX, loX, hiY, loY;
    Split (anchorX, hiX, loX);
    Split (anchorY, hiY, loY);
    // ⚠️ THE ANCHOR'S PIXEL IS PROJECTED FROM THE SPLIT VALUE, not taken to be the
    // centre. The split rounds the anchor, and projecting what the shader will subtract
    // is what keeps that rounding out of every point on screen.
    const double ax = originX + double (hiX) + double (loX);
    const double ay = originY + double (hiY) + double (loY);

    out.linear[0] = float (t.xx);
    out.linear[1] = float (t.xy);
    out.linear[2] = float (t.yx);
    out.linear[3] = float (t.yy);
    out.view[0] = hiX;
    out.view[1] = hiY;
    out.view[2] = loX;
    out.view[3] = loY;
    out.screen[0] = float (t.xx * ax + t.xy * ay + t.ox);
    out.screen[1] = float (t.yx * ax + t.yy * ay + t.oy);
    out.screen[2] = float (2.0 / double (width));
    out.screen[3] = float (2.0 / double (height));
    return true;
}

void ShaderPixel (const ViewConstants& c, float hiX, float hiY, float loX, float loY, float& px, float& py)
{
    // `precise` in the shader: each half is differenced on its own, then summed.
    const float dx = (hiX - c.view[0]) + (loX - c.view[2]);
    const float dy = (hiY - c.view[1]) + (loY - c.view[3]);
    px = c.linear[0] * dx + c.linear[1] * dy + c.screen[0];
    py = c.linear[2] * dx + c.linear[3] * dy + c.screen[1];
}

} // namespace plancontent
} // namespace archviz
} // namespace geomsrv
