// Plan view's Export plan: one building as a reference example for improving the generator.
// The file is a story-slices document (as Bake's Export 2D writes) so the private generator
// reads it as a fixture; its "plan" member carries what the user designed here.
#include "ArchViz/FloorPlanner.hpp"
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
V Shape (const floorscheme::Ring& ring)
{
    js::JsonArray points;
    for (const auto& p : ring)
        points.push_back (Pair ({ p.x, p.y }));
    return V::Array ({ V::Array (std::move (points)) });
}
// A floor's typology scheme: its flats, circulation, stairs, what is unassigned, and its walls
// against the next building.
V Design (const floorscheme::Scheme& s, int story, const floorprogramme::Programme& programme)
{
    js::JsonArray flats, corridors, empty, cores, party;
    for (const auto& f : s.flats)
        flats.push_back (V::Object ({ { "type", V::String (floorprogramme::Name (programme, f.type)) },
                                      { "rooms", Number (f.rooms) },
                                      { "netM2", Number (f.net) },
                                      { "grossM2", Number (f.gross) },
                                      { "frontageM", Number (f.frontage) },
                                      { "depthM", Number (f.depth) },
                                      { "corner", V::Bool (f.corner) },
                                      { "dualAspect", V::Bool (f.through) },
                                      { "corridorEnd", V::Bool (f.cap) },
                                      { "roomsLeftToUser", V::Bool (f.manual) },
                                      { "inRange", V::Bool (f.inRange) },
                                      { "rings", Shape (f.shape) } }));
    for (const auto& c : s.corridors)
        corridors.push_back (Shape (c.shape));
    for (const auto& l : s.lobbies)
        corridors.push_back (Shape (l));
    for (const auto& u : s.unassigned)
        empty.push_back (V::Object ({ { "reason", V::String (u.reason) }, { "rings", Shape (u.shape) } }));
    for (const auto& c : s.cores)
        cores.push_back (V::Object ({ { "x", Number (c.centre.x) },
                                      { "y", Number (c.centre.y) },
                                      { "width", Number (c.width) },
                                      { "depth", Number (c.depth) },
                                      { "rings", Shape (c.shape) } }));
    for (const auto& w : s.party)
        party.push_back (V::Array ({ Pair ({ w[0].x, w[0].y }), Pair ({ w[1].x, w[1].y }) }));
    size_t errors = 0;
    for (const auto& d : s.diagnostics)
        errors += d.level == floorscheme::Diagnostic::Error;
    return V::Object ({ { "story", V::Integer (story) },
                        { "typology", V::String (s.typology) },
                        { "grossM2", Number (s.gross) },
                        { "netM2", Number (s.net) },
                        { "errors", V::Integer (int64_t (errors)) },
                        { "stairs", V::Array (std::move (cores)) },
                        { "corridors", V::Array (std::move (corridors)) },
                        { "partyWalls", V::Array (std::move (party)) },
                        { "empty", V::Array (std::move (empty)) },
                        { "flats", V::Array (std::move (flats)) } });
}
} // namespace

PlanFile ExportPlan (const Plan& plan, const Draft& draft, const Floor& shown, const std::string& stamp,
                     const std::map<int, const floorscheme::Scheme*>& schemes)
{
    PlanFile file;
    const std::string stem = FileStem (plan.key, shown.story, stamp);
    file.name = stem + ".json";
    js::JsonArray slices, floors, designs, cores, programme;
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
        const int design = DesignStory (plan, draft, floor);
        floors.push_back (V::Object ({ { "story", V::Integer (floor.story) },
                                       { "z", Number (floor.z) },
                                       { "height", Number (floor.height) },
                                       { "areaM2", Number (floor.areaM2) },
                                       { "design", V::Integer (design) },
                                       { "unique", V::Bool (draft.designs.unique.contains (floor.story)) } }));
        if (const auto scheme = schemes.find (floor.story); scheme != schemes.end () && scheme->second)
            designs.push_back (Design (*scheme->second, floor.story, draft.programme));
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
                     { "plan", V::Object ({ { "format", V::String ("tapioca.floor-plan.typology") },
                                            { "version", V::Integer (2) },
                                            { "exported", V::String (stamp) },
                                            { "building", V::String (plan.key) },
                                            { "frameAngleDeg", Number (plan.angle * 180 / 3.14159265358979323846) },
                                            { "shownStory", V::Integer (shown.story) },
                                            { "programme", V::Array (std::move (programme)) },
                                            { "cores", V::Array (std::move (cores)) },
                                            { "floors", V::Array (std::move (floors)) },
                                            { "designs", V::Array (std::move (designs)) },
                                            { "edits", V::String (floorscheme::edit::ToJson (draft.designs)) } }) } });
    file.text = js::Write (document, 1) + "\n";
    return file;
}
} // namespace geomsrv::archviz::buildingplan
