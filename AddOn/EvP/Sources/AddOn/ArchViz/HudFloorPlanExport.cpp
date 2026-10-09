// Plan view's Export plan: one building as a reference example for improving the generator.
// The file is a story-slices document (as Bake's Export 2D writes) so the private generator
// reads it as a fixture; its "plan" member carries what the user designed here.
#include "ArchViz/HudFloorPlanFrame.hpp"
#include "NodeGraph/Json.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace geomsrv::archviz::buildingplan {
namespace {
namespace js = evp::nodegraph::json;
namespace cp = Clipper2Lib;
using V = js::JsonValue;
V Number (double value)
{
    return V::Double (std::isfinite (value) ? std::round (value * 1e6) / 1e6 : 0.0);
}
V Pair (Point p)
{
    return V::Array ({ Number (p.x), Number (p.y) });
}
V Rings (const std::vector<SliceChain>& rings)
{
    js::JsonArray out;
    for (const auto& ring : rings) {
        js::JsonArray points;
        for (size_t i = 0; i < ring.Count (); ++i)
            points.push_back (Pair ({ ring.xy[i * 2], ring.xy[i * 2 + 1] }));
        out.push_back (V::Array (std::move (points)));
    }
    return V::Array (std::move (out));
}
V Points (const cp::PathD& path)
{
    js::JsonArray out;
    for (const auto& p : path)
        out.push_back (V::Object ({ { "x", Number (p.x) }, { "y", Number (p.y) } }));
    return V::Array (std::move (out));
}
// The floor's counted boundary in world XY: the snapshot's union, or the sources' union.
cp::PathsD Boundary (const Floor& floor)
{
    const auto paths = [] (const std::vector<SliceChain>& rings) {
        cp::PathsD out;
        for (const auto& ring : rings) {
            cp::PathD path;
            for (size_t i = 0; i < ring.Count (); ++i)
                path.emplace_back (ring.xy[i * 2], ring.xy[i * 2 + 1]);
            out.push_back (std::move (path));
        }
        return out;
    };
    if (floor.outlineKnown)
        return cp::Union (paths (floor.outline), cp::FillRule::NonZero, 6);
    cp::PathsD parts;
    for (const auto& source : floor.contours) {
        const auto own = cp::Union (paths (source), cp::FillRule::EvenOdd, 6);
        parts.insert (parts.end (), own.begin (), own.end ());
    }
    return cp::Union (parts, cp::FillRule::NonZero, 6);
}
std::string FileStem (const std::string& key, int story, const std::string& stamp)
{
    std::string name = key.substr (key.find (':') == std::string::npos ? 0 : key.find (':') + 1);
    for (auto& c : name)
        if (!std::isalnum (static_cast<unsigned char> (c)) && c != '-' && c != '_')
            c = '_';
    if (name.empty ())
        name = "building";
    return name.substr (0, 48) + "-floor" + std::to_string (story) + "-" + stamp;
}
V Design (const QuickPlan& quick, int story)
{
    js::JsonArray flats;
    for (size_t i = 0; i < quick.seeds.size () && i < quick.units.size (); ++i) {
        const auto& seed = quick.seeds[i];
        const auto traits = Traits (quick, seed);
        const auto& type = quick.programme.types[(std::min) (seed.type, quick.programme.types.size () - 1)];
        js::JsonArray keep;
        for (const auto& [trait, label] :
             { std::pair { kCorner, "corner" }, { kDualAspect, "dualAspect" }, { kStraightFacade, "straightFacade" } })
            if (seed.keep & trait)
                keep.push_back (V::String (label));
        flats.push_back (V::Object ({ { "id", V::Integer (seed.id) },
                                      { "type", V::String (TypeName (quick, seed)) },
                                      { "rooms", Number (type.rooms) },
                                      { "minM2", Number (type.minM2) },
                                      { "maxM2", Number (type.maxM2) },
                                      { "targetM2", Number (TargetArea (quick, seed)) },
                                      { "netM2", Number (traits.net) },
                                      { "grossM2", Number (traits.gross) },
                                      { "facadeM", Number (traits.facade) },
                                      { "entranceM", Number (traits.access) },
                                      { "depthM", Number (traits.depth) },
                                      { "corner", V::Bool ((traits.traits & kCorner) != 0) },
                                      { "dualAspect", V::Bool ((traits.traits & kDualAspect) != 0) },
                                      { "straightFacade", V::Bool ((traits.traits & kStraightFacade) != 0) },
                                      { "locked", V::Bool (seed.locked) },
                                      { "keep", V::Array (std::move (keep)) },
                                      { "centre", Pair (UnitCenter (quick, seed)) },
                                      { "rings", Rings (quick.units[i].rings) } }));
    }
    js::JsonArray corridors, empty;
    for (const auto& region : quick.corridors)
        corridors.push_back (Rings (region.rings));
    for (const auto& region : quick.unassigned)
        empty.push_back (Rings (region.rings));
    return V::Object (
        { { "story", V::Integer (story) },
          { "ready", V::Bool (quick.ready) },
          { "note", V::String (quick.note) },
          { "solveNote", V::String (quick.solveNote) },
          { "score", Number (quick.score) },
          { "egress", V::Object ({ { "longestM", Number (quick.egress.longest) },
                                   { "beyondLimitCells", V::Integer (int64_t (quick.egress.invalid.size ())) },
                                   { "cellM", Number (frame::kModule) } }) },
          { "corridors", V::Array (std::move (corridors)) },
          { "empty", V::Array (std::move (empty)) },
          { "flats", V::Array (std::move (flats)) } });
}
} // namespace

PlanFile ExportPlan (const Plan& plan, Draft& draft, const Floor& shown, const std::string& stamp)
{
    PlanFile file;
    const std::string stem = FileStem (plan.key, shown.story, stamp);
    file.name = stem + ".json";
    js::JsonArray slices, floors, designs, cores, programme;
    std::set<int> written;
    for (const auto& floor : plan.floors) {
        // Outer rings with the holes inside them, as the story-slices format keeps them.
        const auto boundary = Boundary (floor);
        for (const auto& outer : boundary) {
            if (cp::Area (outer) <= 0)
                continue;
            js::JsonArray holes;
            for (const auto& hole : boundary)
                if (cp::Area (hole) < 0 && !hole.empty () &&
                    frame::Inside ({ outer }, { hole.front ().x, hole.front ().y }))
                    holes.push_back (Points (hole));
            slices.push_back (V::Object ({ { "group", V::String (plan.key) },
                                           { "story", V::Integer (floor.story) },
                                           { "z", Number (floor.z) },
                                           { "height", Number (floor.height) },
                                           { "outer", Points (outer) },
                                           { "holes", V::Array (std::move (holes)) } }));
        }
        const int design = frame::TemplateStory (plan, draft, floor);
        floors.push_back (V::Object ({ { "story", V::Integer (floor.story) },
                                       { "z", Number (floor.z) },
                                       { "height", Number (floor.height) },
                                       { "areaM2", Number (floor.areaM2) },
                                       { "design", V::Integer (design) },
                                       { "unique", V::Bool (draft.uniqueFloors.contains (floor.story)) } }));
        if (written.insert (design).second)
            designs.push_back (Design (QuickFor (plan, draft, floor), design));
    }
    for (const auto& core : draft.cores) {
        js::JsonArray corners;
        for (const auto& p : Corners (core, plan.angle))
            corners.push_back (Pair (p));
        cores.push_back (V::Object ({ { "x", Number (core.center.x) },
                                      { "y", Number (core.center.y) },
                                      { "width", Number (core.width) },
                                      { "depth", Number (core.depth) },
                                      { "corners", V::Array (std::move (corners)) } }));
    }
    for (size_t t = 0; t < draft.programme.types.size (); ++t) {
        const auto& type = draft.programme.types[t];
        programme.push_back (V::Object ({ { "name", V::String (floorprogramme::Name (draft.programme, t)) },
                                          { "rooms", Number (type.rooms) },
                                          { "minM2", Number (type.minM2) },
                                          { "maxM2", Number (type.maxM2) },
                                          { "share", Number (type.share) } }));
    }
    const V document =
        V::Object ({ { "format", V::String ("tapioca.story-slices.2d") },
                     { "version", V::Integer (1) },
                     { "name", V::String (stem) },
                     { "units", V::String ("m") },
                     { "coordinateSystem", V::String ("Archicad project XY and world Z") },
                     { "ringClosure", V::String ("implicit") },
                     { "edgeType", V::String ("straight; curved sources tessellated") },
                     { "contourBasis", V::String ("counted; outside-envelope and low-headroom regions excluded") },
                     { "module", Number (frame::kModule) },
                     { "program", V::String (floorprogramme::Brief (draft.programme, "\n")) },
                     { "slices", V::Array (std::move (slices)) },
                     { "plan", V::Object ({ { "format", V::String ("tapioca.floor-plan.native") },
                                            { "version", V::Integer (1) },
                                            { "exported", V::String (stamp) },
                                            { "building", V::String (plan.key) },
                                            { "frameAngleDeg", Number (plan.angle * 180 / 3.14159265358979323846) },
                                            { "shownStory", V::Integer (shown.story) },
                                            { "programme", V::Array (std::move (programme)) },
                                            { "cores", V::Array (std::move (cores)) },
                                            { "floors", V::Array (std::move (floors)) },
                                            { "designs", V::Array (std::move (designs)) } }) } });
    file.text = js::Write (document, 1) + "\n";
    return file;
}
} // namespace geomsrv::archviz::buildingplan
