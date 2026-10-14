#include "ArchViz/RoofSlopeBake.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>

namespace geomsrv::archviz::roofslopebake {
namespace {
namespace cp = Clipper2Lib;
namespace rs = roofslope;
namespace js = evp::nodegraph::json;
using V = js::JsonValue;
V Ring (const cp::PathD& ring, const rs::Data& d)
{
    js::JsonArray values;
    for (const auto& p : ring) {
        const auto xy = rs::World (d, { p.x, p.y });
        values.push_back (V::Double (xy.x)), values.push_back (V::Double (xy.y));
    }
    return V::Array (std::move (values));
}
V Names ()
{
    return V::Array ({ V::String ("Slope Symbol"), V::String ("Slope Symbol 29"), V::String ("Slope Symbol 27") });
}
std::string Percent (double slope)
{
    std::ostringstream text;
    text << "i=" << std::fixed << std::setprecision (2) << slope * 100 << "%";
    return text.str ();
}
} // namespace
bool Transaction (const rs::Data& d, int floor, double symbolLength, const std::string& inheritSymbol,
                  js::JsonValue& request, size_t& roofs, size_t& levels, std::string& error)
{
    roofs = levels = 0;
    request = V {};
    if (d.triangles.empty () || !std::isfinite (symbolLength) || symbolLength <= 0) {
        error = "No valid slope geometry or slope-symbol length. Nothing was created.";
        return false;
    }
    for (const auto& cell : d.cells)
        if (!cell.drainInside) {
            error = "Move every proposed drain inside the clipped roof before Bake.";
            return false;
        }
    std::map<std::array<int, 3>, std::vector<const rs::Triangle*>> faces;
    for (const auto& t : d.triangles)
        faces[t.nodes].push_back (&t);
    js::JsonArray steps;
    auto step = [&] (const char* command, js::JsonObject params, js::JsonArray bindings = {}) {
        const size_t index = steps.size ();
        steps.push_back (V::String (js::Write (V::Object ({ { "command", V::String (command) },
                                                            { "params", V::Object (std::move (params)) },
                                                            { "bindingCount", V::Integer (int64_t (bindings.size ())) },
                                                            { "bindings", V::Array (std::move (bindings)) } }),
                                               0)));
        return index;
    };
    std::set<std::array<int64_t, 3>> dimensionPoints;
    double mergedArea = 0, inputArea = 0;
    for (const auto& triangle : d.triangles)
        inputArea += triangle.area;
    for (const auto& [key, triangles] : faces) {
        cp::PathsD paths;
        for (const auto* triangle : triangles) {
            cp::PathD path;
            for (const auto& n : triangle->vertices)
                path.emplace_back (n.xy.x, n.xy.y);
            if (cp::Area (path) < 0)
                std::reverse (path.begin (), path.end ());
            paths.push_back (std::move (path));
        }
        cp::ClipperD clip (6);
        clip.AddSubject (paths);
        cp::PolyTreeD tree;
        if (!clip.Execute (cp::ClipType::Union, cp::FillRule::NonZero, tree)) {
            error = "Unable to merge a roof plane; no partial bake is allowed.";
            return false;
        }
        std::function<bool (const cp::PolyPathD&)> visit = [&] (const cp::PolyPathD& node) {
            if (!node.IsHole () && !node.Polygon ().empty ()) {
                mergedArea += std::abs (cp::Area (node.Polygon ()));
                for (const auto& child : node)
                    if (child->IsHole ())
                        mergedArea -= std::abs (cp::Area (child->Polygon ()));
                // Pick an actual retained triangle inside this connected component.
                const rs::Triangle* chosen = nullptr;
                for (const auto* triangle : triangles) {
                    rs::Vec p {};
                    for (const auto& v : triangle->vertices)
                        p.x += v.xy.x / 3, p.y += v.xy.y / 3;
                    bool inside = cp::PointInPolygon (cp::PointD (p.x, p.y), node.Polygon ()) !=
                                  cp::PointInPolygonResult::IsOutside;
                    for (const auto& child : node)
                        if (child->IsHole () && cp::PointInPolygon (cp::PointD (p.x, p.y), child->Polygon ()) !=
                                                    cp::PointInPolygonResult::IsOutside)
                            inside = false;
                    if (inside && (!chosen || chosen->area < triangle->area))
                        chosen = triangle;
                }
                if (!chosen) {
                    error = "A merged roof polygon has no interior plane sample.";
                    return false;
                }
                const auto& t = *chosen;
                const double slope = std::hypot (t.gx, t.gy), cs = std::cos (d.bearing), sn = std::sin (d.bearing);
                if (!std::isfinite (slope) || slope <= 1e-8) {
                    error = "A roof face has no finite slope; nothing was baked.";
                    return false;
                }
                const double gx = t.gx * cs - t.gy * sn, gy = t.gx * sn + t.gy * cs;
                rs::Vec center {};
                double z = 0;
                for (const auto& p : t.vertices)
                    center.x += p.xy.x / 3, center.y += p.xy.y / 3, z += p.z / 3;
                const auto world = rs::World (d, center);
                const double dx = gy / slope, dy = -gx / slope; // left of pivot is uphill
                js::JsonArray holes;
                for (const auto& child : node)
                    if (child->IsHole ())
                        holes.push_back (Ring (child->Polygon (), d));
                const size_t parent = step (
                    "Tapioca.CreateRoof",
                    { { "outline", Ring (node.Polygon (), d) },
                      { "holes", V::Array (std::move (holes)) },
                      { "mode", V::String ("plane") },
                      { "winding", V::String ("ccw") },
                      { "floorInd", V::Integer (floor) },
                      { "base", V::Double (z) },
                      { "pitch", V::Double (std::atan (slope) * 180 / 3.141592653589793) },
                      { "riseLeft", V::Bool (true) },
                      { "baseLine", V::Array ({ V::Double (world.x - dx / 2), V::Double (world.y - dy / 2),
                                                V::Double (world.x + dx / 2), V::Double (world.y + dy / 2) }) } });
                ++roofs;
                // Same part names, absolute downhill angle, centre-to-origin correction,
                // txt/dt parameters and placed-object style inheritance as PlaceSlopeSymbols.
                js::JsonObject symbol {
                    { "libraryPartNames", Names () },
                    { "x", V::Double (world.x + gx / slope * symbolLength / 2) },
                    { "y", V::Double (world.y + gy / slope * symbolLength / 2) },
                    { "angle", V::Double (std::atan2 (-gy, -gx)) },
                    { "floorInd", V::Integer (floor) },
                    { "parameters",
                      V::Array (
                          { V::Object ({ { "name", V::String ("txt") }, { "value", V::String (Percent (slope)) } }),
                            V::Object ({ { "name", V::String ("dt") }, { "value", V::String ("true") } }) }) }
                };
                if (!inheritSymbol.empty ())
                    symbol["inheritFrom"] =
                        V::Object ({ { "elementId", V::Object ({ { "guid", V::String (inheritSymbol) } }) } });
                step ("Tapioca.PlaceLibraryObject", std::move (symbol));
                // Extremes of the entire retained plane, not merely its largest
                // render triangle. Nudge into an owning triangle to avoid holes.
                std::vector<rs::Node> boundary;
                for (const auto& p : node.Polygon ())
                    boundary.push_back ({ { p.x, p.y }, z + t.gx * (p.x - center.x) + t.gy * (p.y - center.y) });
                auto extremes = std::minmax_element (boundary.begin (), boundary.end (),
                                                     [] (const auto& a, const auto& b) { return a.z < b.z; });
                for (const auto* p : { &*extremes.first, &*extremes.second }) {
                    const auto xy = rs::World (d, p->xy);
                    if (!dimensionPoints
                             .insert (
                                 { std::llround (xy.x * 1000), std::llround (xy.y * 1000), std::llround (p->z * 1000) })
                             .second)
                        continue;
                    rs::Vec inside = center;
                    bool owner = false;
                    for (const auto* triangle : triangles) {
                        cp::PathD outline;
                        for (const auto& v : triangle->vertices)
                            outline.emplace_back (v.xy.x, v.xy.y);
                        if (cp::PointInPolygon (cp::PointD (p->xy.x, p->xy.y), outline) !=
                            cp::PointInPolygonResult::IsOutside) {
                            inside = {};
                            for (const auto& v : triangle->vertices)
                                inside.x += v.xy.x / 3, inside.y += v.xy.y / 3;
                            owner = true;
                            break;
                        }
                    }
                    if (!owner) {
                        error = "A level marker has no interior roof support.";
                        return false;
                    }
                    const double distance = std::hypot (inside.x - p->xy.x, inside.y - p->xy.y);
                    const double fraction = distance > 0 ? (std::min) (0.1, 0.05 / distance) : 0;
                    const auto marker = rs::World (
                        d, { p->xy.x + fraction * (inside.x - p->xy.x), p->xy.y + fraction * (inside.y - p->xy.y) });
                    const auto binding = V::Object ({ { "path", V::String ("parent.elementId.guid") },
                                                      { "step", V::Integer (int64_t (parent)) },
                                                      { "key", V::String ("elementId.guid") } });
                    step ("Tapioca.PlaceLevelDimension",
                          { { "x", V::Double (marker.x) },
                            { "y", V::Double (marker.y) },
                            { "floorInd", V::Integer (floor) },
                            { "mode", V::String ("associative") } },
                          { V::String (js::Write (binding, 0)) });
                    ++levels;
                }
            }
            for (const auto& child : node)
                if (!visit (*child))
                    return false;
            return true;
        };
        if (!visit (tree))
            return false;
    }
    if (std::abs (mergedArea - inputArea) > (std::max) (1e-4, inputArea * 1e-7)) {
        error = "Merged roof planes do not preserve the clipped design area; nothing was created.";
        return false;
    }
    if (roofs == 0 || steps.size () > 10000) {
        error = "Empty or excessive bake; reduce the design scope.";
        return false;
    }
    request = V::Object (
        { { "name", V::String ("Bake roof slopes, symbols and levels") }, { "steps", V::Array (std::move (steps)) } });
    return true;
}
} // namespace geomsrv::archviz::roofslopebake
