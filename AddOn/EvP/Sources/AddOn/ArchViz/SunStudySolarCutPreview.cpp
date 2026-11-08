// The solar cut's preview: the removed part drawn as a transparent cut-out
// (faint surfaces, its outline edges, the cut faces seen through the building
// in a darker pink) and the sun the cut wins as one region per surface with its
// perimeter, not one square per cell.

#include "ArchViz/SunStudyObstructionGeometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace geomsrv::archviz {

namespace {

using V3 = std::array<double, 3>;

V3 Sub (const V3& a, const V3& b)
{
    return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
}
V3 Add (const V3& a, const V3& b)
{
    return { a[0] + b[0], a[1] + b[1], a[2] + b[2] };
}
V3 Scale (const V3& a, double s)
{
    return { a[0] * s, a[1] * s, a[2] * s };
}
double Dot (const V3& a, const V3& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
V3 Cross (const V3& a, const V3& b)
{
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}
bool Normalize (V3& a)
{
    const double length = std::sqrt (Dot (a, a));
    if (!(length > 1.0e-12) || !std::isfinite (length))
        return false;
    a = Scale (a, 1.0 / length);
    return true;
}
V3 Corner (const evp::sunstudy::SolarCutTriangle& triangle, int c)
{
    return { triangle.v[c][0], triangle.v[c][1], triangle.v[c][2] };
}

void Triangle (const V3& a, const V3& b, const V3& c, const V3& normal, uint32_t rgba, GhPreviewBucket& bucket)
{
    const uint32_t base = uint32_t (bucket.meshVertices.size ());
    for (const V3* p : { &a, &b, &c }) {
        GhPreviewMeshVertex vertex;
        vertex.x = float ((*p)[0]);
        vertex.y = float ((*p)[1]);
        vertex.z = float ((*p)[2]);
        vertex.nx = float (normal[0]);
        vertex.ny = float (normal[1]);
        vertex.nz = float (normal[2]);
        vertex.rgba = rgba;
        bucket.meshVertices.push_back (vertex);
    }
    bucket.meshIndices.insert (bucket.meshIndices.end (), { base, base + 1, base + 2 });
}

void Segment (const V3& a, const V3& b, uint32_t rgba, GhPreviewBucket& bucket)
{
    const float from[3] = { float (a[0]), float (a[1]), float (a[2]) };
    const float to[3] = { float (b[0]), float (b[1]), float (b[2]) };
    AppendPreviewSegment (from, to, rgba, bucket);
}

// Two unit vectors spanning the plane of `n`: the first horizontal on a wall,
// world X-ish on a floor.
void PlaneBasis (const V3& n, V3& u, V3& v)
{
    const V3 axis = std::abs (n[2]) < 0.9 ? V3 { 0.0, 0.0, 1.0 } : V3 { 1.0, 0.0, 0.0 };
    u = Cross (axis, n);
    Normalize (u);
    v = Cross (n, u);
}

// The removed part's outline: per group of near-coplanar triangles (sector
// planes a few degrees apart count as one face), every triangle edge is laid
// on its line with its direction; where the two directions cover a stretch
// equally it is inside the face, otherwise it is the face's outline. This is
// immune to the T-junctions clipping and the trapezoid caps leave.
void OutlineEdges (const std::vector<const evp::sunstudy::SolarCutTriangle*>& triangles, uint32_t rgba,
                   GhPreviewBucket& bucket)
{
    constexpr double kSameFace = 0.985; // ~10 degrees
    std::vector<V3> faces;
    using Key = std::tuple<size_t, long long, long long, long long, long long, long long, long long>;
    std::map<Key, std::vector<std::pair<double, int>>> lines;
    std::map<Key, std::pair<V3, V3>> frames; // foot, direction
    const auto q = [] (double x) { return (long long) std::llround (x * 1000.0); };
    for (const auto* triangle : triangles) {
        const V3 a = Corner (*triangle, 0), b = Corner (*triangle, 1), c = Corner (*triangle, 2);
        V3 n = Cross (Sub (b, a), Sub (c, a));
        if (!Normalize (n))
            continue;
        size_t face = faces.size ();
        for (size_t f = 0; f < faces.size (); ++f)
            if (Dot (faces[f], n) > kSameFace) {
                face = f;
                break;
            }
        if (face == faces.size ())
            faces.push_back (n);
        const V3 corners[3] = { a, b, c };
        for (int e = 0; e < 3; ++e) {
            const V3& from = corners[e];
            const V3& to = corners[(e + 1) % 3];
            V3 dir = Sub (to, from);
            if (!Normalize (dir))
                continue;
            int sign = 1;
            const int major = std::abs (dir[0]) >= std::abs (dir[1]) ? (std::abs (dir[0]) >= std::abs (dir[2]) ? 0 : 2)
                                                                     : (std::abs (dir[1]) >= std::abs (dir[2]) ? 1 : 2);
            if (dir[major] < 0.0) {
                dir = Scale (dir, -1.0);
                sign = -1;
            }
            const V3 foot = Sub (from, Scale (dir, Dot (from, dir)));
            const Key key { face, q (dir[0]), q (dir[1]), q (dir[2]), q (foot[0]), q (foot[1]), q (foot[2]) };
            const double t0 = Dot (from, dir), t1 = Dot (to, dir);
            auto& events = lines[key];
            events.emplace_back ((std::min) (t0, t1), sign);
            events.emplace_back ((std::max) (t0, t1), -sign);
            frames.emplace (key, std::make_pair (foot, dir));
        }
    }
    for (auto& [key, events] : lines) {
        std::sort (events.begin (), events.end ());
        const auto& [foot, dir] = frames[key];
        int cover = 0;
        double runFrom = 0.0;
        bool open = false;
        for (size_t i = 0; i < events.size (); ++i) {
            cover += events[i].second;
            const bool lastAtT = i + 1 == events.size () || events[i + 1].first > events[i].first + 1.0e-6;
            if (!lastAtT)
                continue;
            if (cover != 0 && !open) {
                runFrom = events[i].first;
                open = true;
            }
            else if (cover == 0 && open) {
                if (events[i].first - runFrom > 1.0e-4)
                    Segment (Add (foot, Scale (dir, runFrom)), Add (foot, Scale (dir, events[i].first)), rgba, bucket);
                open = false;
            }
        }
    }
}

} // namespace

void BuildSolarCutGeometry (const evp::sunstudy::SolarCutResult& cut, const double point[3],
                            GhPreviewDrawables& drawables)
{
    const auto fill = [] (const std::vector<evp::sunstudy::SolarCutTriangle>& list, uint32_t rgba,
                          GhPreviewBucket& bucket) {
        for (const auto& triangle : list) {
            const V3 a = Corner (triangle, 0), b = Corner (triangle, 1), c = Corner (triangle, 2);
            V3 n = Cross (Sub (b, a), Sub (c, a));
            if (Normalize (n))
                Triangle (a, b, c, n, rgba, bucket);
        }
    };
    // The cutting surface: a faint sheet with its outline -- the edge rays, the
    // rim, and with a radius the line AB it starts from.
    (void) point;
    fill (cut.envelope, kSolarCutEnvelopeRgba, drawables.depthTested);
    std::vector<const evp::sunstudy::SolarCutTriangle*> sheet;
    for (const auto& triangle : cut.envelope)
        sheet.push_back (&triangle);
    OutlineEdges (sheet, kSunObstructionPink, drawables.depthTested);
    // What goes, as a transparent cut-out: its outer surfaces faint where the
    // building shows them, the cut faces through the building (x-ray) in a darker
    // pink, and the outline of the removed solid; its hidden
    // edges are drawn dashed by the depth-tested line pass.
    fill (cut.removed, kSolarCutRemovedRgba, drawables.depthTested);
    fill (cut.caps, kSolarCutCapRgba, drawables.xray);
    std::vector<const evp::sunstudy::SolarCutTriangle*> solid;
    solid.reserve (cut.removed.size () + cut.caps.size ());
    for (const auto& triangle : cut.removed)
        solid.push_back (&triangle);
    for (const auto& triangle : cut.caps)
        solid.push_back (&triangle);
    OutlineEdges (solid, kSolarCutEdgeRgba, drawables.depthTested);
}

void AppendSolarCutWonCells (const std::vector<SolarCutWonCell>& cells, double size, GhPreviewDrawables& drawables,
                             const std::vector<SolarCutWonCell>* surface)
{
    // One region per surface: cells are grouped by face direction and plane,
    // laid on a raster of the study's grid, small gaps closed and holes filled,
    // and the raster's boundary traced by marching squares into one filled
    // shape with its perimeter.
    const double step = size > 0.0 ? size : 0.5;
    using GroupKey = std::tuple<long long, long long, long long, long long>;
    struct Group {
        V3 n;
        double offset = 0.0;
        std::map<std::pair<int, int>, int> cells;
        std::set<std::pair<int, int>> surface; // every study cell of this surface, won or not
    };
    std::map<GroupKey, Group> groups;
    // A cell's surface group and its raster place on it.
    const auto place = [step] (const SolarCutWonCell& cell, GroupKey& key, V3& rounded, double& offset,
                               std::pair<int, int>& ij) {
        V3 n { cell.normal[0], cell.normal[1], cell.normal[2] };
        if (!Normalize (n))
            return false;
        rounded = { std::round (n[0] * 4.0), std::round (n[1] * 4.0), std::round (n[2] * 4.0) };
        if (!Normalize (rounded))
            return false;
        const V3 p { cell.position[0], cell.position[1], cell.position[2] };
        offset = Dot (rounded, p);
        key = { std::llround (rounded[0] * 4.0), std::llround (rounded[1] * 4.0), std::llround (rounded[2] * 4.0),
                std::llround (offset) };
        V3 u, v;
        PlaneBasis (rounded, u, v);
        ij = { int (std::lround (Dot (p, u) / step)), int (std::lround (Dot (p, v) / step)) };
        return true;
    };
    for (const auto& cell : cells) {
        GroupKey key;
        V3 rounded;
        double offset = 0.0;
        std::pair<int, int> ij;
        if (!place (cell, key, rounded, offset, ij))
            continue;
        Group& group = groups[key];
        group.n = rounded;
        if (group.cells.emplace (ij, 1).second)
            group.offset += offset;
    }
    if (surface != nullptr)
        for (const auto& cell : *surface) {
            GroupKey key;
            V3 rounded;
            double offset = 0.0;
            std::pair<int, int> ij;
            if (!place (cell, key, rounded, offset, ij))
                continue;
            const auto group = groups.find (key);
            if (group != groups.end ())
                group->second.surface.insert (ij);
        }
    for (auto& [key, group] : groups) {
        if (group.cells.empty ())
            continue;
        const double height = group.offset / double (group.cells.size ()) + 0.03;
        V3 u, v;
        PlaneBasis (group.n, u, v);
        int minI = INT32_MAX, minJ = INT32_MAX, maxI = INT32_MIN, maxJ = INT32_MIN;
        for (const auto& [ij, unused] : group.cells) {
            minI = (std::min) (minI, ij.first), maxI = (std::max) (maxI, ij.first);
            minJ = (std::min) (minJ, ij.second), maxJ = (std::max) (maxJ, ij.second);
        }
        constexpr int kPad = 2;
        const int w = maxI - minI + 1 + 2 * kPad, h = maxJ - minJ + 1 + 2 * kPad;
        if (size_t (w) * size_t (h) > 4000000u)
            continue; // a raster that large is not a preview
        std::vector<uint8_t> in (size_t (w) * h, 0), grown (in.size (), 0);
        const auto at = [w] (int x, int y) { return size_t (y) * w + x; };
        for (const auto& [ij, unused] : group.cells)
            in[at (ij.first - minI + kPad, ij.second - minJ + kPad)] = 1;
        // Closing: grow by one cell, then shrink by one -- joins cells one gap apart.
        for (int y = 1; y + 1 < h; ++y)
            for (int x = 1; x + 1 < w; ++x)
                for (int dy = -1; dy <= 1 && !grown[at (x, y)]; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (in[at (x + dx, y + dy)]) {
                            grown[at (x, y)] = 1;
                            break;
                        }
        std::fill (in.begin (), in.end (), uint8_t (0));
        for (int y = 1; y + 1 < h; ++y)
            for (int x = 1; x + 1 < w; ++x) {
                bool all = true;
                for (int dy = -1; dy <= 1 && all; ++dy)
                    for (int dx = -1; dx <= 1 && all; ++dx)
                        all = grown[at (x + dx, y + dy)] != 0;
                in[at (x, y)] = all ? 1 : 0;
            }
        // Holes: whatever empty cell the outside cannot reach is inside.
        std::vector<uint8_t> outside (in.size (), 0);
        std::vector<std::pair<int, int>> stack { { 0, 0 } };
        outside[0] = 1;
        while (!stack.empty ()) {
            const auto [x, y] = stack.back ();
            stack.pop_back ();
            const int nx[4] = { x + 1, x - 1, x, x }, ny[4] = { y, y, y + 1, y - 1 };
            for (int k = 0; k < 4; ++k)
                if (nx[k] >= 0 && ny[k] >= 0 && nx[k] < w && ny[k] < h && !in[at (nx[k], ny[k])] &&
                    !outside[at (nx[k], ny[k])]) {
                    outside[at (nx[k], ny[k])] = 1;
                    stack.emplace_back (nx[k], ny[k]);
                }
        }
        for (size_t c = 0; c < in.size (); ++c)
            in[c] = outside[c] ? 0 : 1;
        // Cleanup: closing and hole filling never reach past the surface --
        // only places where the study has a cell of this surface stay.
        if (surface != nullptr)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (in[at (x, y)] && group.surface.count ({ x + minI - kPad, y + minJ - kPad }) == 0)
                        in[at (x, y)] = 0;
        const auto world = [&] (double x, double y) {
            return Add (Scale (group.n, height),
                        Add (Scale (u, (x + minI - kPad) * step), Scale (v, (y + minJ - kPad) * step)));
        };
        // Marching squares between cell centres: inside corners and edge
        // midpoints in order make the fill; the midpoints pair into the outline.
        for (int y = 0; y + 1 < h; ++y)
            for (int x = 0; x + 1 < w; ++x) {
                const int cx[4] = { x, x + 1, x + 1, x }, cy[4] = { y, y, y + 1, y + 1 };
                bool inside[4];
                int count = 0;
                for (int k = 0; k < 4; ++k)
                    count += (inside[k] = in[at (cx[k], cy[k])] != 0) ? 1 : 0;
                if (count == 0)
                    continue;
                std::vector<V3> polygon, mids;
                for (int k = 0; k < 4; ++k) {
                    const int l = (k + 1) % 4;
                    if (inside[k])
                        polygon.push_back (world (cx[k], cy[k]));
                    if (inside[k] != inside[l]) {
                        const V3 mid = world (0.5 * (cx[k] + cx[l]), 0.5 * (cy[k] + cy[l]));
                        polygon.push_back (mid);
                        mids.push_back (mid);
                    }
                }
                for (size_t k = 1; k + 1 < polygon.size (); ++k)
                    Triangle (polygon[0], polygon[k], polygon[k + 1], group.n, kSolarCutWonRgba, drawables.depthTested);
                for (size_t k = 0; k + 1 < mids.size (); k += 2)
                    Segment (mids[k], mids[k + 1], kSolarCutWonEdgeRgba, drawables.depthTested);
            }
    }
}

} // namespace geomsrv::archviz
