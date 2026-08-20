#include "ArchViz/PlanAnchorRibbon.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Below this the two endpoints are the same point and there is no direction to
// take a perpendicular to. Archicad's coordinates are metres, so a tenth of a
// millimetre is far below anything a plan can express.
constexpr float kMinSegmentMetres = 1e-4f;

}   // namespace

void TessellateEdge (float x0, float y0, float x1, float y1,
                     float arcAngle, float arcSign, float arcChordMetres,
                     std::vector<float>& outXY)
{
    outXY.push_back (x0);
    outXY.push_back (y0);

    const float sweep = arcAngle * arcSign;
    if (std::fabs (sweep) < 1e-6f)
        return;                                  // straight: the chord is the edge

    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float chord = std::sqrt (dx * dx + dy * dy);
    if (chord < kMinSegmentMetres)
        return;

    // ⚠️ A FULL CIRCLE IS TWO 180-DEGREE ARCS BETWEEN TWO POINTS, which is how
    // Archicad stores one (see CommandUtils' walk: a 2-point ring is valid when
    // arced). At exactly pi the half-angle tangent below is infinite, so the
    // centre is the chord midpoint and there is no offset to compute.
    const float halfSweep = sweep * 0.5f;
    const float sinHalf = std::sin (halfSweep);
    if (std::fabs (sinHalf) < 1e-6f)
        return;                                  // a 2*pi "arc": degenerate, draw the chord

    const float radius = chord / (2.0f * sinHalf);      // signed with the sweep

    // Centre of the arc. For a COUNTERCLOCKWISE-positive sweep the centre lies
    // along the chord's LEFT normal at (chord/2)/tan(sweep/2) — verified in the
    // unit tests against a quarter circle about the origin.
    const float ux = dx / chord;
    const float uy = dy / chord;
    const float leftX = -uy;
    const float leftY = ux;
    const float tanHalf = std::tan (halfSweep);
    const float offset = (std::fabs (tanHalf) < 1e-6f) ? 0.0f : (chord * 0.5f) / tanHalf;
    const float cx = (x0 + x1) * 0.5f + leftX * offset;
    const float cy = (y0 + y1) * 0.5f + leftY * offset;

    // Enough chords that no chord exceeds arcChordMetres. Bounded so a tiny
    // arcChordMetres on a large arc cannot allocate without limit.
    const float absRadius = std::fabs (radius);
    const float arcLength = std::fabs (sweep) * absRadius;
    const float wanted = (arcChordMetres > 1e-4f) ? (arcLength / arcChordMetres) : 1.0f;
    const int steps = std::max (2, std::min (256, int (std::ceil (wanted))));

    const float startAngle = std::atan2 (y0 - cy, x0 - cx);
    for (int i = 1; i < steps; ++i) {
        const float t = float (i) / float (steps);
        const float angle = startAngle + sweep * t;
        outXY.push_back (cx + absRadius * std::cos (angle));
        outXY.push_back (cy + absRadius * std::sin (angle));
    }
}

size_t BuildAnchorRibbonSet (const std::vector<std::vector<float>>& outlines,
                             const std::vector<std::vector<float>>& arcs,
                             float z, float arcSign, float arcChordMetres,
                             std::vector<PlanAnchorVertex>& out)
{
    const size_t before = out.size ();

    for (size_t ring = 0; ring < outlines.size (); ++ring) {
        const std::vector<float>& xy = outlines[ring];
        if (xy.size () < 4)
            continue;                       // fewer than 2 points: no direction to take

        const size_t pointCount = xy.size () / 2;
        const std::vector<float>* ringArcs =
            (ring < arcs.size () && arcs[ring].size () >= pointCount) ? &arcs[ring] : nullptr;

        // Closed, always: these are element OUTLINES, and a wall outline that
        // did not close would leave one edge of the wall undrawn.
        BuildAnchorRibbon (xy.data (), pointCount,
                           ringArcs != nullptr ? ringArcs->data () : nullptr,
                           /*closed*/ true, z, arcSign, arcChordMetres, out);
    }

    return out.size () - before;
}

size_t BuildAnchorRibbon (const float* xy, size_t pointCount,
                          const float* arcs, bool closed, float z,
                          float arcSign, float arcChordMetres,
                          std::vector<PlanAnchorVertex>& out)
{
    if (xy == nullptr || pointCount < 2)
        return 0;

    // Arcs first, so the ribbon below only ever sees straight segments. Doing it
    // in one pass would mean the corner caps had to know whether their neighbour
    // was a chord or a real edge.
    std::vector<float> points;
    points.reserve (pointCount * 2);

    const size_t edgeCount = closed ? pointCount : (pointCount - 1);
    for (size_t i = 0; i < edgeCount; ++i) {
        const size_t next = (i + 1) % pointCount;
        TessellateEdge (xy[i * 2], xy[i * 2 + 1], xy[next * 2], xy[next * 2 + 1],
                        arcs != nullptr ? arcs[i] : 0.0f, arcSign, arcChordMetres,
                        points);
    }
    if (!closed) {                               // TessellateEdge omits every end point
        points.push_back (xy[(pointCount - 1) * 2]);
        points.push_back (xy[(pointCount - 1) * 2 + 1]);
    }

    const size_t total = points.size () / 2;
    if (total < 2)
        return 0;

    const size_t before = out.size ();
    const size_t segments = closed ? total : (total - 1);

    for (size_t i = 0; i < segments; ++i) {
        const size_t next = (i + 1) % total;
        const float x0 = points[i * 2],    y0 = points[i * 2 + 1];
        const float x1 = points[next * 2], y1 = points[next * 2 + 1];

        const float dx = x1 - x0;
        const float dy = y1 - y0;
        const float length = std::sqrt (dx * dx + dy * dy);
        if (length < kMinSegmentMetres)
            continue;                            // a repeated point, not a segment

        const float ux = dx / length;
        const float uy = dy / length;
        const float nx = -uy;                    // left normal
        const float ny = ux;

        // Four corners: (start|end) x (left|right), each pushed across by the
        // normal and along by the tangent (the square cap).
        const PlanAnchorVertex startLeft  {x0, y0, z,  nx,  ny, -ux, -uy};
        const PlanAnchorVertex startRight {x0, y0, z, -nx, -ny, -ux, -uy};
        const PlanAnchorVertex endLeft    {x1, y1, z,  nx,  ny,  ux,  uy};
        const PlanAnchorVertex endRight   {x1, y1, z, -nx, -ny,  ux,  uy};

        // ⚠️ NO WINDING ORDER IS RELIED ON. The PSO draws this with culling OFF,
        // because the ribbon is flat and a top-down camera that ends up on the
        // other side of it would otherwise erase the whole anchor layer.
        out.push_back (startLeft);
        out.push_back (endLeft);
        out.push_back (startRight);

        out.push_back (startRight);
        out.push_back (endLeft);
        out.push_back (endRight);
    }

    return out.size () - before;
}

}   // namespace archviz
}   // namespace geomsrv
