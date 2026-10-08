#include "ArchViz/HudBuildingPlan.hpp"
#include "ArchViz/MassingSlices.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::buildingplan {
std::string Fingerprint (const metadata::EntityMetadata& entity)
{
    metadata::EntityMetadata value;
    if (const auto* property = metadata::FindProperty (entity, kLocations))
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
    const auto* property = metadata::FindProperty (entity, kLocations);
    if (!property)
        return {};
    const auto& value = property->value;
    if (value.type != metadata::ValueType::List || value.elementType != metadata::ValueType::Length ||
        value.list.empty () || value.list.size () % 2 || value.list.size () > kMaxStairs * 2)
        return { {}, true };
    Stored stored;
    for (size_t i = 0; i < value.list.size (); i += 2) {
        const auto& x = value.list[i];
        const auto& y = value.list[i + 1];
        const Point point { x.d, y.d };
        if (x.type != metadata::ValueType::Length || y.type != metadata::ValueType::Length ||
            !std::isfinite (point.x) || !std::isfinite (point.y) || std::abs (point.x) > 1e9 ||
            std::abs (point.y) > 1e9)
            return { {}, true };
        stored.points.push_back (point);
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
        plan.mixed |= source->stored.invalid || (!first && plan.saved != source->stored.points);
        plan.sources.push_back (*source);
        if (first)
            plan.saved = source->stored.points;
        first = false;
    }
    if (plan.mixed)
        plan.saved.clear ();
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
            floor.physical.push_back (source->rawChains);
        }
        plan.floors.push_back (std::move (floor));
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
    return draft.known && (draft.points != draft.original || (draft.originalMixed && draft.changed));
}
bool Conflict (const Plan& plan, const Draft& draft)
{
    if (!draft.known)
        return false;
    if (plan.guids != draft.guids || plan.saved != draft.original || plan.mixed != draft.originalMixed ||
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
    draft = {};
    draft.known = true;
    draft.story = story;
    draft.points = draft.original = plan.saved;
    draft.originalMixed = plan.mixed;
    draft.guids = plan.guids;
    for (const auto& source : plan.sources)
        draft.fingerprints.push_back (source.fingerprint);
}
std::vector<hudmeta::Edit> Edits (const Plan& plan, const Draft& draft)
{
    if (!Dirty (draft) || Conflict (plan, draft) || plan.floors.empty ())
        return {};
    if (draft.points.size () > kMaxStairs || std::any_of (draft.points.begin (), draft.points.end (), [] (Point point) {
            return !std::isfinite (point.x) || !std::isfinite (point.y) || std::abs (point.x) > 1e9 ||
                   std::abs (point.y) > 1e9;
        }))
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
        edit.action = draft.points.empty () ? hudmeta::Edit::Action::Clear : hudmeta::Edit::Action::Set;
        for (const auto& point : draft.points) {
            edit.numbers.push_back (point.x);
            edit.numbers.push_back (point.y);
        }
        edit.label = "Proposed stairwell locations";
        edits.push_back (std::move (edit));
    }
    return edits;
}
bool Place (const Floor& floor, Draft& draft, Point point)
{
    if (!draft.placing || !Contains (floor, point))
        return false;
    for (size_t i = 0; i < draft.points.size (); ++i)
        if (int (i) != draft.selected && std::hypot (draft.points[i].x - point.x, draft.points[i].y - point.y) < 1e-6)
            return false;
    if (draft.selected >= 0 && size_t (draft.selected) < draft.points.size ())
        draft.points[size_t (draft.selected)] = point;
    else {
        if (draft.points.size () >= kMaxStairs)
            return false;
        draft.selected = int (draft.points.size ());
        draft.points.push_back (point);
    }
    draft.placing = false;
    draft.changed = true;
    return true;
}
bool NeedsTwoStairs (double areaM2, const massingareas::Coefficients& coefficients)
{
    return areaM2 * coefficients.grossFactor > 500.0;
}
} // namespace geomsrv::archviz::buildingplan
