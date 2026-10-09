#include "ArchViz/HudBuildingPlan.hpp"
#include "ArchViz/MassingSlices.hpp"
#include <algorithm>
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
bool FitsOutline (const cp::PathsD& outline, Point delta)
{
    const double x = delta.x, y = delta.y, w = kStairWidth / 2, d = kStairDepth / 2;
    const cp::PathD rectangle { { x - w, y - d }, { x + w, y - d }, { x + w, y + d }, { x - w, y + d } };
    return !outline.empty () &&
           std::abs (cp::Area (cp::Difference ({ rectangle }, outline, cp::FillRule::NonZero, 6))) < 1e-6;
}
bool Valid (Point point)
{
    return std::isfinite (point.x) && std::isfinite (point.y) && std::abs (point.x) <= 1e9 && std::abs (point.y) <= 1e9;
}
} // namespace
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
        floor.outlineKey = QuickSignature (floor, {});
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
bool Fits (const Floor& floor, Point center)
{
    return Valid (center) && FitsOutline (Outline (floor, center), {});
}
Point Snap (const Floor& floor, Point center)
{
    if (!Valid (center))
        return center;
    const auto outline = Outline (floor, center);
    struct Candidate {
        double distance;
        Point delta, normal;
        double shift;
    };
    std::vector<Candidate> candidates;
    for (const auto& path : outline)
        for (size_t i = 0; i < path.size (); ++i) {
            const auto& a = path[i];
            const auto& b = path[(i + 1) % path.size ()];
            const double length = std::hypot (b.x - a.x, b.y - a.y);
            if (length < 1e-8)
                continue;
            const double tx = (b.x - a.x) / length, ty = (b.y - a.y) / length;
            const double nx = -ty, ny = tx;
            const double along = -a.x * tx - a.y * ty;
            const double extent = kStairWidth / 2 * std::abs (tx) + kStairDepth / 2 * std::abs (ty);
            if (along + extent < 0 || along - extent > length)
                continue; // Snap to a segment, never its infinite extension.
            const double distance = -a.x * nx - a.y * ny;
            const double support = kStairWidth / 2 * std::abs (nx) + kStairDepth / 2 * std::abs (ny);
            for (double sign : { -1.0, 1.0 }) {
                const double shift = sign * support - distance;
                if (std::abs (shift) > kSnapDistance)
                    continue;
                candidates.push_back ({ std::abs (shift), { nx * shift, ny * shift }, { nx, ny }, shift });
                std::stable_sort (candidates.begin (), candidates.end (),
                                  [] (const auto& lhs, const auto& rhs) { return lhs.distance < rhs.distance; });
                if (candidates.size () > 8)
                    candidates.pop_back ();
            }
        }
    for (const auto& candidate : candidates)
        if (FitsOutline (outline, candidate.delta))
            return { center.x + candidate.delta.x, center.y + candidate.delta.y };
    // A corner may require both nearby sides to snap before the footprint fits.
    for (size_t i = 0; i < candidates.size (); ++i)
        for (size_t j = i + 1; j < candidates.size (); ++j) {
            const auto& a = candidates[i];
            const auto& b = candidates[j];
            const double det = a.normal.x * b.normal.y - a.normal.y * b.normal.x;
            if (std::abs (det) < 1e-6)
                continue;
            const Point delta { (a.shift * b.normal.y - b.shift * a.normal.y) / det,
                                (a.normal.x * b.shift - b.normal.x * a.shift) / det };
            if (std::hypot (delta.x, delta.y) <= kSnapDistance && FitsOutline (outline, delta))
                return { center.x + delta.x, center.y + delta.y };
        }
    return center;
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
    const bool acknowledged = draft.known && draft.points == plan.saved && draft.guids == plan.guids && !plan.mixed;
    auto quickPlans = acknowledged ? std::move (draft.quickPlans) : std::map<int, QuickPlan> {};
    auto uniqueFloors = acknowledged ? std::move (draft.uniqueFloors) : std::set<int> {};
    draft = {};
    draft.quickPlans = std::move (quickPlans);
    draft.uniqueFloors = std::move (uniqueFloors);
    draft.known = true;
    draft.story = story;
    draft.points = draft.original = plan.saved;
    draft.originalMixed = plan.mixed;
    draft.guids = plan.guids;
    for (const auto& source : plan.sources)
        draft.fingerprints.push_back (source.fingerprint);
}
void Sync (const Plan& plan, Draft& draft)
{
    if (!draft.known || (!Dirty (draft) && Conflict (plan, draft)) ||
        (Dirty (draft) && !plan.mixed && plan.saved == draft.points && plan.guids == draft.guids))
        Reset (plan, draft);
}
std::vector<hudmeta::Edit> Edits (const Plan& plan, const Draft& draft)
{
    if (!Dirty (draft) || draft.dragging || Conflict (plan, draft) || plan.floors.empty ())
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
    point = Snap (floor, point);
    if (!draft.placing || !Fits (floor, point))
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
bool BeginDrag (Draft& draft, Point mouse, uintptr_t owner)
{
    if (!draft.moving || draft.dragging || draft.selected < 0 || size_t (draft.selected) >= draft.points.size () ||
        !Valid (mouse))
        return false;
    const auto point = draft.points[size_t (draft.selected)];
    if (std::abs (mouse.x - point.x) > kStairWidth / 2 || std::abs (mouse.y - point.y) > kStairDepth / 2)
        return false;
    draft.dragOriginal = point;
    draft.dragOffset = { point.x - mouse.x, point.y - mouse.y };
    draft.dragging = true;
    draft.dragOwner = owner;
    return true;
}
bool Drag (const Floor& floor, Draft& draft, Point mouse)
{
    if (!draft.dragging || draft.selected < 0 || size_t (draft.selected) >= draft.points.size ())
        return false;
    const auto point = Snap (floor, { mouse.x + draft.dragOffset.x, mouse.y + draft.dragOffset.y });
    if (!Fits (floor, point))
        return false; // Retain the last valid position across holes/outside the canvas.
    for (size_t i = 0; i < draft.points.size (); ++i)
        if (int (i) != draft.selected && std::hypot (draft.points[i].x - point.x, draft.points[i].y - point.y) < 1e-6)
            return false;
    draft.points[size_t (draft.selected)] = point;
    return true;
}
void EndDrag (Draft& draft)
{
    if (draft.dragging && draft.selected >= 0 && size_t (draft.selected) < draft.points.size ())
        draft.changed |= draft.points[size_t (draft.selected)] != draft.dragOriginal;
    draft.moving = draft.dragging = false;
    draft.dragOwner = 0;
}
void Cancel (Draft& draft)
{
    for (auto& [story, plan] : draft.quickPlans)
        CancelUnits (plan);
    if (draft.dragging && draft.selected >= 0 && size_t (draft.selected) < draft.points.size ())
        draft.points[size_t (draft.selected)] = draft.dragOriginal;
    draft.placing = draft.moving = draft.dragging = false;
    draft.dragOwner = 0;
}
bool NeedsTwoStairs (double areaM2, const massingareas::Coefficients& coefficients)
{
    return areaM2 * coefficients.grossFactor > 500.0;
}
} // namespace geomsrv::archviz::buildingplan
