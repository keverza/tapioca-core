#include "ArchViz/HudBuildingPlan.hpp"
#include "ArchViz/MassingSlices.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <clipper2/clipper.h>

namespace geomsrv::archviz::buildingplan {
namespace {
namespace cp = Clipper2Lib;
cp::PathsD Outline (const Floor& floor, Point origin)
{
    cp::PathsD paths;
    const auto convert = [&] (const std::vector<SliceChain>& contours, cp::PathsD& target) {
        for (const auto& chain : contours) {
            cp::PathD path;
            for (size_t i = 0; i < chain.Count (); ++i)
                path.emplace_back (chain.xy[i * 2] - origin.x, chain.xy[i * 2 + 1] - origin.y);
            target.push_back (std::move (path));
        }
    };
    if (floor.outlineKnown) {
        convert (floor.outline, paths);
        return paths;
    }
    for (const auto& source : floor.contours) {
        cp::PathsD part;
        convert (source, part);
        const auto normalized = cp::Union (part, cp::FillRule::EvenOdd, 6);
        paths.insert (paths.end (), normalized.begin (), normalized.end ());
    }
    return cp::Union (paths, cp::FillRule::NonZero, 6);
}
bool Valid (Point point)
{
    return std::isfinite (point.x) && std::isfinite (point.y) && std::abs (point.x) <= 1e9 && std::abs (point.y) <= 1e9;
}
bool Sized (const Core& core)
{
    return std::isfinite (core.width) && std::isfinite (core.depth) && core.width >= kMinCore - 1e-9 &&
           core.width <= kMaxCore + 1e-9 && core.depth >= kMinCore - 1e-9 && core.depth <= kMaxCore + 1e-9;
}
std::vector<double> Numbers (const metadata::EntityMetadata& entity, const char* key, bool& present, bool& valid)
{
    std::vector<double> out;
    const auto* property = metadata::FindProperty (entity, key);
    present = property != nullptr;
    valid = true;
    if (!property)
        return out;
    const auto& value = property->value;
    if (value.type != metadata::ValueType::List || value.elementType != metadata::ValueType::Length ||
        value.list.empty () || value.list.size () % 2 || value.list.size () > kMaxStairs * 2) {
        valid = false;
        return {};
    }
    for (const auto& item : value.list) {
        if (item.type != metadata::ValueType::Length || !std::isfinite (item.d) || std::abs (item.d) > 1e9) {
            valid = false;
            return {};
        }
        out.push_back (item.d);
    }
    return out;
}
} // namespace
std::vector<Point> Corners (const Core& core, double angle)
{
    const double c = std::cos (angle), s = std::sin (angle), w = core.width / 2, d = core.depth / 2;
    std::vector<Point> out;
    for (const auto& [u, v] : { std::pair { -w, -d }, { w, -d }, { w, d }, { -w, d } })
        out.push_back ({ core.center.x + u * c - v * s, core.center.y + u * s + v * c });
    return out;
}
std::string Fingerprint (const metadata::EntityMetadata& entity)
{
    metadata::EntityMetadata value;
    for (const char* key : { kLocations, kShapes, kDesigns })
        if (const auto* property = metadata::FindProperty (entity, key))
            value.properties.push_back (*property);
    return metadata::ToJson (value);
}
bool Matches (const metadata::EntityMetadata& entity, const hudmeta::Edit& edit)
{
    const auto* role = metadata::FindProperty (entity, "tapioca.role");
    const std::string key =
        massingbuildings::Id (entity).empty () ? "slab:" + edit.element : "building:" + massingbuildings::Id (entity);
    return role && role->value.type == metadata::ValueType::String && role->value.s == "MassingSlab" &&
           key == edit.expectedBuildingKey && Fingerprint (entity) == edit.expectedPropertyJson;
}
Stored Read (const metadata::EntityMetadata& entity)
{
    bool present = false, valid = true;
    std::string designs;
    if (const auto* text = metadata::FindProperty (entity, kDesigns);
        text && text->value.type == metadata::ValueType::String)
        designs = text->value.s;
    const auto points = Numbers (entity, kLocations, present, valid);
    if (!present)
        return { {}, designs };
    if (!valid)
        return { {}, designs, true };
    bool shaped = false, sizes = true;
    const auto shapes = Numbers (entity, kShapes, shaped, sizes);
    // Sizes are optional: a legacy or mismatched list reads as default 4.5 x 4.2 m cores.
    const bool useShapes = shaped && sizes && shapes.size () == points.size ();
    Stored stored;
    stored.designs = designs;
    for (size_t i = 0; i < points.size (); i += 2) {
        Core core { { points[i], points[i + 1] } };
        if (useShapes && Sized ({ {}, shapes[i], shapes[i + 1] })) {
            core.width = shapes[i];
            core.depth = shapes[i + 1];
        }
        stored.cores.push_back (core);
    }
    return stored;
}
Plan Build (const massingslices::Result& slices, const massingbuildings::Preview& preview)
{
    Plan plan;
    plan.key = preview.building.key;
    plan.guids = preview.building.guids;
    if (!slices.complete || preview.section.floors.empty ()) {
        plan.note = "Plan awaits the complete current building slices.";
        return plan;
    }
    bool first = true;
    for (const auto& guid : plan.guids) {
        const auto source = std::find_if (slices.planSources.begin (), slices.planSources.end (),
                                          [&] (const auto& item) { return item.guid == guid; });
        if (source == slices.planSources.end ()) {
            plan.note = "Plan awaits every building member's metadata.";
            return plan;
        }
        plan.mixed |= source->stored.invalid ||
                      (!first && (plan.saved != source->stored.cores || plan.savedDesigns != source->stored.designs));
        plan.sources.push_back (*source);
        if (first)
            plan.saved = source->stored.cores, plan.savedDesigns = source->stored.designs;
        first = false;
    }
    if (plan.mixed)
        plan.saved.clear (), plan.savedDesigns.clear ();
    size_t points = 0;
    for (const auto& row : preview.section.floors) {
        Floor floor;
        floor.story = row.storey;
        floor.z = row.base;
        floor.areaM2 = row.areaM2;
        for (const auto& part : row.parts) {
            const auto source = std::find_if (slices.rows.begin (), slices.rows.end (), [&] (const auto& item) {
                return item.guid == part.guid && item.story == part.sourceStory && std::abs (item.z - row.base) < 1e-6;
            });
            if (source == slices.rows.end ()) {
                plan.floors.clear ();
                plan.note = "Plan awaits every current floor contour; no partial building shown.";
                return plan;
            }
            for (const auto* contours : { &source->chains, &source->rawChains })
                for (const auto& chain : *contours) {
                    points += chain.Count ();
                    if (!chain.closed || chain.xy.size () % 2 || chain.Count () < 3 || points > 1000000 ||
                        std::any_of (chain.xy.begin (), chain.xy.end (), [] (double coordinate) {
                            return !std::isfinite (coordinate) || std::abs (coordinate) > 1e9;
                        })) {
                        plan.floors.clear ();
                        plan.note = "Plan contour is invalid or exceeds its display budget.";
                        return plan;
                    }
                }
            floor.contours.push_back (source->chains);
            floor.height = (std::max) (floor.height, source->floorHeight);
            floor.physical.push_back (source->rawChains);
        }
        const Point origin =
            floor.contours.empty () || floor.contours.front ().empty ()
                ? Point {}
                : Point { floor.contours.front ().front ().xy[0], floor.contours.front ().front ().xy[1] };
        for (const auto& path : Outline (floor, origin)) {
            SliceChain chain;
            chain.closed = true;
            for (const auto& point : path)
                chain.xy.insert (chain.xy.end (), { point.x + origin.x, point.y + origin.y });
            floor.outline.push_back (std::move (chain));
        }
        floor.outlineKnown = true;
        floor.outlineKey = OutlineKey (floor);
        plan.floors.push_back (std::move (floor));
    }
    // One frame per building (as the private generator): the largest floor's longest edge.
    const auto largest = std::max_element (plan.floors.begin (), plan.floors.end (),
                                           [] (const Floor& a, const Floor& b) { return a.areaM2 < b.areaM2; });
    if (largest != plan.floors.end ()) {
        double longest = 0;
        for (const auto& ring : largest->outline)
            for (size_t i = 0; i < ring.Count (); ++i) {
                const size_t j = (i + 1) % ring.Count ();
                const double x = ring.xy[j * 2] - ring.xy[i * 2], y = ring.xy[j * 2 + 1] - ring.xy[i * 2 + 1];
                if (std::hypot (x, y) > longest + 1e-9) {
                    longest = std::hypot (x, y);
                    plan.angle = std::atan2 (y, x);
                }
            }
    }
    return plan;
}
const Floor* Displayed (const Plan& plan, const Draft& draft)
{
    for (const auto& floor : plan.floors)
        if (floor.story == draft.story)
            return &floor;
    return plan.floors.empty () ? nullptr : &plan.floors.front ();
}
bool Contains (const Floor& floor, Point point)
{
    if (!std::isfinite (point.x) || !std::isfinite (point.y))
        return false;
    for (const auto& contours : floor.contours) {
        bool inside = false;
        bool boundary = false;
        for (const auto& chain : contours) {
            if (!chain.closed || chain.Count () < 3 || chain.xy.size () % 2)
                return false;
            for (size_t i = 0, j = chain.Count () - 1; i < chain.Count (); j = i++) {
                const double ax = chain.xy[j * 2] - point.x, ay = chain.xy[j * 2 + 1] - point.y;
                const double bx = chain.xy[i * 2] - point.x, by = chain.xy[i * 2 + 1] - point.y;
                const double cross = ax * by - ay * bx;
                // Boundaries are not placement targets (including courtyard edges).
                if (std::abs (cross) <= 1e-8 * (std::max) (1.0, std::hypot (bx - ax, by - ay)) &&
                    ax * bx + ay * by <= 0)
                    boundary = true;
                if ((ay > 0) != (by > 0) && ax + (bx - ax) * (-ay) / (by - ay) > 0)
                    inside = !inside;
            }
        }
        if (inside && !boundary)
            return true;
    }
    return false;
}
bool Dirty (const Draft& draft)
{
    return draft.known && (draft.cores != draft.original || !(draft.designs == draft.originalDesigns) ||
                           (draft.originalMixed && draft.changed));
}
bool Conflict (const Plan& plan, const Draft& draft)
{
    if (!draft.known)
        return false;
    if (plan.guids != draft.guids || plan.saved != draft.original || plan.mixed != draft.originalMixed ||
        plan.savedDesigns != floorscheme::edit::ToJson (draft.originalDesigns) ||
        plan.sources.size () != draft.fingerprints.size ())
        return true;
    for (size_t i = 0; i < plan.sources.size (); ++i)
        if (plan.sources[i].fingerprint != draft.fingerprints[i])
            return true;
    return false;
}
void Reset (const Plan& plan, Draft& draft)
{
    const int story = draft.story;
    auto programme = std::move (draft.programme);
    draft = {};
    draft.programme = floorprogramme::Valid (programme) ? std::move (programme) : floorprogramme::Default ();
    draft.known = true;
    draft.story = story;
    draft.cores = draft.original = plan.saved;
    // Saved designs that do not read (a newer add-on wrote them) open as none; Save replaces them.
    std::string unread;
    if (!floorscheme::edit::FromJson (plan.savedDesigns, draft.designs, unread))
        draft.designs = {};
    draft.originalDesigns = draft.designs;
    draft.originalMixed = plan.mixed;
    draft.guids = plan.guids;
    for (const auto& source : plan.sources)
        draft.fingerprints.push_back (source.fingerprint);
}
void Sync (const Plan& plan, Draft& draft)
{
    if (!draft.known || (!Dirty (draft) && Conflict (plan, draft)) ||
        (Dirty (draft) && !plan.mixed && plan.saved == draft.cores &&
         plan.savedDesigns == floorscheme::edit::ToJson (draft.designs) && plan.guids == draft.guids))
        Reset (plan, draft);
}
std::vector<hudmeta::Edit> Edits (const Plan& plan, const Draft& draft)
{
    if (!Dirty (draft) || Conflict (plan, draft) || plan.floors.empty ())
        return {};
    if (draft.cores.size () > kMaxStairs ||
        std::any_of (draft.cores.begin (), draft.cores.end (),
                     [] (const Core& core) { return !Valid (core.center) || !Sized (core); }))
        return {};
    // The typology places stairs by its own rules (FloorScheme.hpp); a saved stair need only stand
    // on a floor of the building.
    for (const auto& core : draft.cores)
        if (std::none_of (plan.floors.begin (), plan.floors.end (),
                          [&] (const Floor& floor) { return Contains (floor, core.center); }))
            return {};
    std::vector<hudmeta::Edit> edits;
    for (const auto& guid : plan.guids) {
        hudmeta::Edit edit;
        edit.id = kLocations;
        edit.element = guid;
        edit.expectedBuildingKey = plan.key;
        const auto source = std::find_if (plan.sources.begin (), plan.sources.end (),
                                          [&] (const auto& item) { return item.guid == guid; });
        if (source == plan.sources.end ())
            return {};
        edit.expectedPropertyJson = source->fingerprint;
        edit.kind = hudmeta::FieldKind::Fixed;
        edit.type = metadata::ValueType::List;
        edit.action = draft.cores.empty () ? hudmeta::Edit::Action::Clear : hudmeta::Edit::Action::Set;
        edit.text = floorscheme::edit::ToJson (draft.designs); // the floor designs travel with the stairs
        for (const auto& core : draft.cores) {
            edit.numbers.insert (edit.numbers.end (), { core.center.x, core.center.y });
            edit.shapes.insert (edit.shapes.end (), { core.width, core.depth });
        }
        edit.label = "Proposed stairwells and floor plans";
        edits.push_back (std::move (edit));
    }
    return edits;
}
std::string OutlineKey (const Floor& floor)
{
    // Canonical world rings at micrometre precision: equal across input order, start vertex and
    // winding, never across translated buildings.
    std::vector<std::string> keys;
    const auto append = [&] (const auto& rings) {
        for (const auto& ring : rings) {
            cp::PathD canonical;
            bool finite = true;
            for (size_t i = 0; i < ring.Count (); ++i) {
                const double x = ring.xy[i * 2], y = ring.xy[i * 2 + 1];
                finite &= std::isfinite (x) && std::isfinite (y) && std::abs (x) <= 1e9 && std::abs (y) <= 1e9;
                canonical.emplace_back (x, y);
            }
            if (finite && canonical.size () >= 3)
                canonical = cp::TrimCollinear (canonical, 6);
            std::vector<std::string> points;
            for (const auto& point : canonical) {
                std::ostringstream p;
                p << std::fixed << std::setprecision (6) << (std::abs (point.x) < 0.5e-6 ? 0 : point.x) << ','
                  << (std::abs (point.y) < 0.5e-6 ? 0 : point.y);
                points.push_back (p.str ());
            }
            if (points.empty ())
                continue;
            const size_t start = size_t (std::min_element (points.begin (), points.end ()) - points.begin ());
            std::string forward, reverse;
            for (size_t i = 0; i < points.size (); ++i) {
                forward += points[(start + i) % points.size ()] + ';';
                reverse += points[(start + points.size () - i) % points.size ()] + ';';
            }
            keys.push_back (std::to_string (ring.closed) + ':' + (std::min) (forward, reverse));
        }
    };
    if (floor.outlineKnown)
        append (floor.outline);
    else
        for (const auto& source : floor.contours)
            append (source);
    std::sort (keys.begin (), keys.end ());
    std::string key;
    for (const auto& ring : keys)
        key += '/' + ring;
    return key + '#';
}
void UseProgramme (Draft& draft, const floorprogramme::Programme& programme)
{
    if (floorprogramme::Valid (programme) && !(draft.programme == programme))
        draft.programme = programme;
}
} // namespace geomsrv::archviz::buildingplan
