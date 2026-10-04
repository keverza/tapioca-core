// ArchViz/HudClip -- see the header.

#include "ArchViz/HudClip.hpp"

#include <cmath>

namespace geomsrv {
namespace archviz {
namespace hudclip {

namespace {

// `a` towards `b` by `t`: position, uv, and each colour byte rounded.
Corner Between (const Corner& a, const Corner& b, float t)
{
    Corner out;
    out.x = a.x + (b.x - a.x) * t;
    out.y = a.y + (b.y - a.y) * t;
    out.u = a.u + (b.u - a.u) * t;
    out.v = a.v + (b.v - a.v) * t;
    for (int shift = 0; shift < 32; shift += 8) {
        const float from = float ((a.col >> shift) & 0xFFu), to = float ((b.col >> shift) & 0xFFu);
        const long byte = std::lround (from + (to - from) * t);
        out.col |= uint32_t (byte < 0 ? 0 : byte > 255 ? 255 : byte) << shift;
    }
    return out;
}

// One edge of the rectangle: the signed distance inside it is non-negative.
enum class Edge { Left, Top, Right, Bottom };

float Inside (const Corner& p, Edge edge, const Rect& clip)
{
    switch (edge) {
        case Edge::Left:
            return p.x - clip.left;
        case Edge::Top:
            return p.y - clip.top;
        case Edge::Right:
            return clip.right - p.x;
        case Edge::Bottom:
            return clip.bottom - p.y;
    }
    return 0.0f;
}

// The polygon `in` (`count` corners) cut by one edge into `out`; the corners it has left.
// A triangle cut by four edges has at most seven.
int CutBy (const Corner* in, int count, Edge edge, const Rect& clip, Corner* out)
{
    int kept = 0;
    for (int i = 0; i < count; ++i) {
        const Corner& p = in[i];
        const Corner& q = in[(i + 1) % count];
        const float dp = Inside (p, edge, clip), dq = Inside (q, edge, clip);
        if (dp >= 0.0f)
            out[kept++] = p;
        // The edge crossed between them: where.
        if ((dp >= 0.0f) != (dq >= 0.0f))
            out[kept++] = Between (p, q, dp / (dp - dq));
    }
    return kept;
}

} // namespace

uint32_t Clip (const Corner& a, const Corner& b, const Corner& c, const Rect& clip, std::vector<Corner>& out)
{
    if (!(clip.right > clip.left && clip.bottom > clip.top))
        return 0;
    const auto within = [&] (const Corner& p) {
        return p.x >= clip.left && p.x <= clip.right && p.y >= clip.top && p.y <= clip.bottom;
    };
    // Most of a HUD is inside its window: kept whole, bit for bit.
    if (within (a) && within (b) && within (c)) {
        out.push_back (a);
        out.push_back (b);
        out.push_back (c);
        return 1;
    }
    // Wholly past one edge: nothing.
    if ((a.x < clip.left && b.x < clip.left && c.x < clip.left) ||
        (a.x > clip.right && b.x > clip.right && c.x > clip.right) ||
        (a.y < clip.top && b.y < clip.top && c.y < clip.top) ||
        (a.y > clip.bottom && b.y > clip.bottom && c.y > clip.bottom))
        return 0;
    Corner first[8] = { a, b, c };
    Corner second[8];
    int count = 3;
    count = CutBy (first, count, Edge::Left, clip, second);
    count = CutBy (second, count, Edge::Top, clip, first);
    count = CutBy (first, count, Edge::Right, clip, second);
    count = CutBy (second, count, Edge::Bottom, clip, first);
    if (count < 3)
        return 0;
    for (int k = 1; k + 1 < count; ++k) {
        out.push_back (first[0]);
        out.push_back (first[k]);
        out.push_back (first[k + 1]);
    }
    return uint32_t (count - 2);
}

} // namespace hudclip
} // namespace archviz
} // namespace geomsrv
