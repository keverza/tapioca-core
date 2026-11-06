#include "Edit/EditCompound.hpp"
#include "Edit/EditShape.hpp"
#include "Geometry/PlanarTriangulation.hpp"

namespace geomsrv::edit {

namespace {

bool InsideRing (const std::vector<Vec3>& ring, Vec3 p)
{
    bool in = false;
    for (size_t i = 0, j = ring.size () - 1; i < ring.size (); j = i++) {
        const Vec3 a = ring[i], b = ring[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

} // namespace

bool CapTriangles (const Entity& entity, std::vector<Vec3>& points, std::vector<uint32_t>& triangles,
                   std::string& error)
{
    points.clear ();
    triangles.clear ();
    const auto rings = ShapeRings (entity);
    if (rings.empty ()) {
        error = "the profile has no contour";
        return false;
    }
    std::vector<uint32_t> first;
    for (const auto& ring : rings) {
        if (ring.size () < 3) {
            error = "a contour of the profile is degenerate";
            points.clear ();
            return false;
        }
        first.push_back (static_cast<uint32_t> (points.size ()));
        points.insert (points.end (), ring.begin (), ring.end ());
    }
    // Contours never cross (ValidateContours), so containment of one point is containment of the
    // ring. A solid region is an even-depth contour with its odd-depth children -- the holes
    // directly inside it; an island inside a hole is a region of its own. The triangulator takes
    // exactly one outer and its holes, so each region is covered on its own.
    const size_t count = rings.size ();
    std::vector<size_t> depth (count, 0), parent (count, count);
    for (size_t c = 0; c < count; ++c)
        for (size_t o = 0; o < count; ++o)
            if (o != c && InsideRing (rings[o], rings[c].front ()))
                ++depth[c];
    for (size_t c = 0; c < count; ++c)
        for (size_t o = 0; o < count; ++o)
            if (o != c && depth[o] + 1 == depth[c] && InsideRing (rings[o], rings[c].front ()))
                parent[c] = o;
    for (size_t outer = 0; outer < count; ++outer) {
        if (depth[outer] % 2 != 0)
            continue;
        std::vector<size_t> group { outer };
        for (size_t c = 0; c < count; ++c)
            if (parent[c] == outer)
                group.push_back (c);
        std::vector<std::vector<double>> xy (group.size ());
        std::vector<engine::PlanarRing> planar;
        for (size_t k = 0; k < group.size (); ++k) {
            for (const auto& p : rings[group[k]])
                xy[k].insert (xy[k].end (), { p.x, p.y });
            planar.push_back ({ xy[k].data (), rings[group[k]].size () });
        }
        engine::PlanarTriangulation cover;
        if (!engine::TriangulatePlanar (planar, false, cover, error) || !cover.generatedXY.empty () ||
            cover.cover == engine::PlanarCover::HoleDropped || cover.cover == engine::PlanarCover::Approximate ||
            cover.cover == engine::PlanarCover::Refused) {
            if (error.empty ())
                error = "the profile's cap has no exact cover that keeps its holes";
            points.clear ();
            triangles.clear ();
            return false;
        }
        // The cover indexes the group's rings concatenated; map back to `points`.
        std::vector<uint32_t> map;
        for (size_t k = 0; k < group.size (); ++k)
            for (uint32_t i = 0; i < rings[group[k]].size (); ++i)
                map.push_back (first[group[k]] + i);
        for (uint32_t index : cover.triangles)
            triangles.push_back (map[index]);
    }
    return true;
}

bool SolidTriangles (const Entity& entity, std::vector<Vec3>& points, std::vector<uint32_t>& triangles,
                     std::string& error)
{
    if (entity.kind != Kind::Extrusion) {
        error = "only an extrusion has a solid";
        return false;
    }
    std::vector<Vec3> cap;
    std::vector<uint32_t> capTriangles;
    if (!CapTriangles (entity, cap, capTriangles, error))
        return false;
    const uint32_t total = static_cast<uint32_t> (cap.size ());
    points = cap;
    for (const auto& p : cap)
        points.push_back (p + Vec3 { 0.0, 0.0, entity.height });
    triangles.clear ();
    for (size_t t = 0; t + 2 < capTriangles.size (); t += 3) {
        const uint32_t a = capTriangles[t], b = capTriangles[t + 1], c = capTriangles[t + 2];
        triangles.insert (triangles.end (), { a, c, b });                         // bottom faces down
        triangles.insert (triangles.end (), { total + a, total + b, total + c }); // top faces up
    }
    // Every contour already winds with the solid on its left, so a quad up each of its pieces
    // turned this way faces out of the solid -- outward on the outline, into a hole on a hole.
    uint32_t base = 0;
    for (const auto& ring : ShapeRings (entity)) {
        const uint32_t n = static_cast<uint32_t> (ring.size ());
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t a = base + i, b = base + (i + 1) % n;
            triangles.insert (triangles.end (), { a, b, total + b, a, total + b, total + a });
        }
        base += n;
    }
    return true;
}

} // namespace geomsrv::edit
