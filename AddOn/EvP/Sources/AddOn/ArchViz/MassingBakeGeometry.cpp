#include "ArchViz/MassingBake.hpp"
#include <clipper2/clipper.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <map>

namespace geomsrv::archviz::massingbake {
namespace {
namespace js = evp::nodegraph::json;
namespace cp = Clipper2Lib;
using V = js::JsonValue;
bool Fail (std::string& error, const char* message)
{
    error = message;
    return false;
}
V Ring (const cp::PathD& path, double ox, double oy)
{
    js::JsonArray out;
    for (const auto& point : path)
        out.push_back (V::Object ({ { "x", V::Double (point.x + ox) }, { "y", V::Double (point.y + oy) } }));
    return V::Array (std::move (out));
}
bool Polygons (const std::vector<SliceChain>& chains, js::JsonArray& out, size_t& points, std::string& error)
{
    if (chains.empty ())
        return true;
    if (chains.front ().Count () < 3)
        return Fail (error, "Bake requires closed floor contours.");
    const double ox = chains.front ().xy[0], oy = chains.front ().xy[1];
    cp::PathsD paths;
    for (const auto& chain : chains) {
        if (!chain.closed || chain.xy.size () % 2 || chain.Count () < 3 || chain.Count () > 4096)
            return Fail (error, "Bake needs closed contours with 3..4096 points.");
        cp::PathD path;
        for (size_t i = 0; i < chain.Count (); ++i) {
            const double x = chain.xy[i * 2], y = chain.xy[i * 2 + 1];
            if (!std::isfinite (x) || !std::isfinite (y) || std::abs (x) > 1e9 || std::abs (y) > 1e9)
                return Fail (error, "Invalid bake coordinate.");
            path.emplace_back (x - ox, y - oy);
        }
        points += path.size ();
        if (points > 100000)
            return Fail (error, "Bake contour budget exceeded.");
        paths.push_back (std::move (path));
    }
    cp::ClipperD clip (6);
    clip.AddSubject (paths);
    cp::PolyTreeD tree;
    if (!clip.Execute (cp::ClipType::Union, cp::FillRule::EvenOdd, tree))
        return Fail (error, "Bake contour normalization failed.");
    const auto visit = [&] (const auto& self, const cp::PolyPathD& node) -> void {
        if (!node.Polygon ().empty () && !node.IsHole ()) {
            js::JsonArray holes;
            for (const auto& child : node)
                if (child->IsHole ())
                    holes.push_back (Ring (child->Polygon (), ox, oy));
            out.push_back (
                V::Object ({ { "outer", Ring (node.Polygon (), ox, oy) }, { "holes", V::Array (std::move (holes)) } }));
        }
        for (const auto& child : node)
            self (self, *child);
    };
    visit (visit, tree);
    return true;
}
bool Solid (const overlaylayers::Mesh& mesh, V& result, std::string& error)
{
    if (mesh.points.size () < 12 || mesh.points.size () % 3 || mesh.points.size () > 300000 ||
        mesh.indices.size () < 12 || mesh.indices.size () % 3 || mesh.indices.size () > 300000)
        return Fail (error, "Bake needs a bounded complete envelope solid, not an empty preview.");
    const std::array<double, 3> origin { mesh.points[0], mesh.points[1], mesh.points[2] };
    using Key = std::array<int64_t, 3>;
    std::map<Key, uint32_t> welded;
    std::vector<uint32_t> mapping;
    std::vector<std::array<double, 3>> local;
    js::JsonArray vertices, polygons;
    for (size_t i = 0; i < mesh.points.size (); i += 3) {
        Key key;
        std::array<double, 3> p;
        for (size_t k = 0; k < 3; ++k) {
            const double value = mesh.points[i + k];
            if (!std::isfinite (value) || std::abs (value) > 1e9)
                return Fail (error, "Invalid envelope bake coordinate.");
            p[k] = value - origin[k];
            key[k] = int64_t (std::llround (p[k] * 1e6));
        }
        const auto added = welded.emplace (key, uint32_t (local.size ()));
        mapping.push_back (added.first->second);
        if (added.second) {
            local.push_back (p);
            vertices.push_back (
                V::Object ({ { "x", V::Double (p[0]) }, { "y", V::Double (p[1]) }, { "z", V::Double (p[2]) } }));
        }
    }
    std::map<std::pair<uint32_t, uint32_t>, unsigned> edges;
    double volume = 0;
    for (size_t i = 0; i < mesh.indices.size (); i += 3) {
        uint32_t ids[3];
        for (size_t k = 0; k < 3; ++k) {
            if (mesh.indices[i + k] >= mapping.size ())
                return Fail (error, "Invalid envelope bake index.");
            ids[k] = mapping[mesh.indices[i + k]];
        }
        if (ids[0] == ids[1] || ids[1] == ids[2] || ids[2] == ids[0])
            return Fail (error, "Degenerate envelope bake face.");
        for (size_t k = 0; k < 3; ++k)
            ++edges[{ ids[k], ids[(k + 1) % 3] }];
        const auto& a = local[ids[0]];
        const auto& b = local[ids[1]];
        const auto& c = local[ids[2]];
        volume += (a[0] * (b[1] * c[2] - b[2] * c[1]) + a[1] * (b[2] * c[0] - b[0] * c[2]) +
                   a[2] * (b[0] * c[1] - b[1] * c[0])) /
                  6;
        polygons.push_back (V::Object (
            { { "vertexIds", V::Array ({ V::Integer (ids[0]), V::Integer (ids[1]), V::Integer (ids[2]) }) } }));
    }
    for (const auto& [edge, count] : edges)
        if (count != 1 || edges[{ edge.second, edge.first }] != 1)
            return Fail (error, "Envelope bake requires a closed oriented shell; no box substitution.");
    if (!std::isfinite (volume) || volume <= 1e-9)
        return Fail (error, "Envelope bake requires a positive outward volume.");
    result = V::Object ({ { "basePoint", V::Object ({ { "x", V::Double (origin[0]) },
                                                      { "y", V::Double (origin[1]) },
                                                      { "z", V::Double (origin[2]) } }) },
                          { "body", V::Object ({ { "bodyType", V::String ("Solid") },
                                                 { "vertices", V::Array (std::move (vertices)) },
                                                 { "polygons", V::Array (std::move (polygons)) } }) } });
    return true;
}
} // namespace
bool Geometry (Kind kind, const massingslices::Result* slices, const massingcalculation::Result* envelope,
               const std::vector<SliceChain>& collapse, V& result, std::string& error)
{
    error.clear ();
    js::JsonArray items;
    size_t points = 0;
    if (kind == Kind::Slices) {
        if (slices && !slices->complete)
            return Fail (error, "Story-slice sources are incomplete; wait for all current operated bodies.");
        if (!slices || slices->rows.empty ())
            return Fail (error, "No current story slices to bake.");
        for (const auto& row : slices->rows) {
            if (!std::isfinite (row.z) || !std::isfinite (row.floorHeight) || row.floorHeight <= 0)
                return Fail (error, "Invalid slice bake elevation/height.");
            js::JsonArray polygons;
            if (!Polygons (row.chains, polygons, points, error))
                return false;
            for (auto& polygon : polygons) {
                auto object = *polygon.AsObject ();
                object["z"] = V::Double (row.z);
                object["height"] = V::Double (row.floorHeight);
                object["sourceGuid"] = V::String (row.guid);
                object["story"] = V::Integer (row.story);
                std::string group = "slab:" + row.guid;
                for (const auto& surface : slices->buildingSurfaces)
                    if (surface.record.guid == row.guid && !surface.record.id.empty ()) {
                        group = "building:" + surface.record.id;
                        break;
                    }
                object["group"] = V::String (group);
                items.push_back (V::Object (std::move (object)));
            }
        }
    }
    else if (kind == Kind::Envelope) {
        if (!envelope || !envelope->hasEnvelope)
            return Fail (error, "No current allowed envelope to bake.");
        for (const auto& mesh : envelope->layer.meshes) {
            V solid;
            if (!Solid (mesh, solid, error))
                return false;
            items.push_back (std::move (solid));
        }
    }
    else if (!Polygons (collapse, items, points, error))
        return false;
    if (kind == Kind::Collapse) {
        const auto rounded = [] (V& vertices) {
            slabslices::Ring ring;
            for (const auto& point : *vertices.AsArray ()) {
                double x = 0, y = 0;
                point.Find ("x")->AsDouble (x);
                point.Find ("y")->AsDouble (y);
                ring.xy.insert (ring.xy.end (), { x, y });
            }
            ring = CircularRing (ring);
            js::JsonArray outline, arcs;
            for (size_t i = 0; i < ring.xy.size () / 2; ++i) {
                outline.push_back (
                    V::Object ({ { "x", V::Double (ring.xy[i * 2]) }, { "y", V::Double (ring.xy[i * 2 + 1]) } }));
                arcs.push_back (V::Double (i < ring.arcs.size () ? ring.arcs[i] : 0));
            }
            vertices = V::Array (std::move (outline));
            return V::Array (std::move (arcs));
        };
        for (auto& item : items) {
            auto object = *item.AsObject ();
            object["arcs"] = rounded (object["outer"]);
            js::JsonArray holes = *object["holes"].AsArray (), holeArcs;
            for (auto& hole : holes)
                holeArcs.push_back (rounded (hole));
            object["holes"] = V::Array (std::move (holes));
            object["holeArcs"] = V::Array (std::move (holeArcs));
            item = V::Object (std::move (object));
        }
    }
    if (items.empty () || items.size () > 2048)
        return Fail (error, "No counted geometry to bake, or element budget exceeded.");
    auto out = V::Object ({ { "kind", V::String (kind == Kind::Slices     ? "slices"
                                                 : kind == Kind::Envelope ? "envelope"
                                                                          : "collapse") },
                            { "items", V::Array (std::move (items)) } });
    if (js::Write (out, 0).size () > 16 * 1024 * 1024)
        return Fail (error, "Bake snapshot transport budget exceeded.");
    result = std::move (out);
    return true;
}
bool Export2DJson (const massingslices::Result& slices, std::string& text, std::string& error)
{
    V geometry;
    if (!Geometry (Kind::Slices, &slices, nullptr, {}, geometry, error))
        return false;
    const auto document =
        V::Object ({ { "format", V::String ("tapioca.story-slices.2d") },
                     { "version", V::Integer (1) },
                     { "units", V::String ("m") },
                     { "coordinateSystem", V::String ("Archicad project XY and world Z") },
                     { "ringClosure", V::String ("implicit") },
                     { "edgeType", V::String ("straight; curved sources tessellated") },
                     { "contourBasis", V::String ("counted; outside-envelope and low-headroom regions excluded") },
                     { "slices", *geometry.Find ("items") } });
    text = js::Write (document, 2) + "\n";
    return true;
}
std::vector<size_t> HomeStoreys (const ProjectStoreys& storeys, const std::vector<double>& elevations,
                                 const std::vector<std::string>& groups)
{
    if (storeys.Empty () || elevations.size () != groups.size ())
        return {};
    std::map<std::string, std::map<int64_t, size_t>> floors;
    for (size_t i = 0; i < elevations.size (); ++i)
        floors[groups[i]][int64_t (std::llround (elevations[i] * 1e6))] = 0;
    for (auto& [group, levels] : floors) {
        const double first = double (levels.begin ()->first) / 1e6;
        size_t home = 0;
        for (size_t i = 1; i < storeys.levels.size (); ++i)
            if (std::abs (storeys.levels[i] - first) < std::abs (storeys.levels[home] - first))
                home = i;
        for (auto& [z, assigned] : levels) {
            assigned = home;
            home = (std::min) (home + 1, storeys.levels.size () - 1);
        }
    }
    std::vector<size_t> result;
    for (size_t i = 0; i < elevations.size (); ++i)
        result.push_back (floors[groups[i]][int64_t (std::llround (elevations[i] * 1e6))]);
    return result;
}
slabslices::Ring CircularRing (const slabslices::Ring& sampled)
{
    const size_t count = sampled.xy.size () / 2;
    slabslices::Ring result;
    const auto point = [&] (size_t i) {
        return std::pair<double, double> { sampled.xy[(i % count) * 2], sampled.xy[(i % count) * 2 + 1] };
    };
    size_t at = 0;
    while (at < count) {
        const auto [ax, ay] = point (at);
        result.xy.insert (result.xy.end (), { ax, ay });
        size_t end = at + 1;
        double angle = 0;
        if (at + 4 <= count) {
            const auto [bx, by] = point (at + 1);
            const auto [cx, cy] = point (at + 2);
            const double ux = bx - ax, uy = by - ay, vx = cx - ax, vy = cy - ay;
            const double cross = ux * vy - uy * vx;
            const double divisor = 2 * cross;
            if (std::abs (divisor) > 1e-10) {
                const double u2 = ux * ux + uy * uy, v2 = vx * vx + vy * vy;
                const double ox = (u2 * vy - v2 * uy) / divisor;
                const double oy = (ux * v2 - vx * u2) / divisor;
                const double radius = std::hypot (ox, oy);
                double previous = std::atan2 (-oy, -ox), sweep = 0;
                if (radius > 0.01 && radius <= 10000) {
                    for (size_t i = at + 1; i <= count; ++i) {
                        const auto [x, y] = point (i);
                        const double px = x - ax - ox, py = y - ay - oy;
                        if (std::abs (std::hypot (px, py) - radius) > 0.00005)
                            break;
                        const double current = std::atan2 (py, px);
                        const double delta = std::remainder (current - previous, 2 * 3.141592653589793);
                        if (delta * cross <= 0 || std::abs (delta) > 0.5 || std::abs (sweep + delta) > 1.58)
                            break;
                        sweep += delta;
                        previous = current;
                        if (i >= at + 4) {
                            end = i;
                            angle = sweep;
                        }
                    }
                }
            }
        }
        result.arcs.push_back (angle);
        at = end;
    }
    return result.xy.size () >= 6 ? result : sampled;
}
std::string Inputs (const V& geometry, const V& settings, uint64_t token)
{
    auto request = *geometry.AsObject ();
    request["settings"] = settings;
    request["token"] = V::Integer (int64_t (token));
    // Graph-script inputs are named bindings, not the argument object itself.
    return js::Write (V::Object ({ { "request", V::Object (std::move (request)) } }), 0);
}
} // namespace geomsrv::archviz::massingbake
