#include "ArchViz/HudBuildingPlan.hpp"
#include "ArchViz/MassingSlices.hpp"
#include "ArchViz/HudFloorPlanFrame.hpp"
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
bool Valid (Point point)
{
    return std::isfinite (point.x) && std::isfinite (point.y) && std::abs (point.x) <= 1e9 && std::abs (point.y) <= 1e9;
}
bool Sized (const Core& core)
{
    return std::isfinite (core.width) && std::isfinite (core.depth) && core.width >= kMinCore - 1e-9 &&
           core.width <= kMaxCore + 1e-9 && core.depth >= kMinCore - 1e-9 && core.depth <= kMaxCore + 1e-9;
}
// The core at `delta` from the outline's origin, which is the core's own centre.
bool FitsOutline (const cp::PathsD& outline, const Core& core, double angle, Point delta)
{
    if (outline.empty ())
        return false;
    for (const auto& corner : Corners ({ delta, core.width - 4e-6, core.depth - 4e-6 }, angle))
        if (!frame::Inside (outline, corner))
            return false;
    const double c = std::cos (angle), s = std::sin (angle);
    const auto local = [&] (cp::PointD p) {
        return Point { (p.x - delta.x) * c + (p.y - delta.y) * s, -(p.x - delta.x) * s + (p.y - delta.y) * c };
    };
    // Any counted boundary passing through the rectangle's strict interior means a hole
    // or concavity is covered. Segment clipping is linear in vertices; no Boolean op on hover.
    const double w = core.width / 2 - 2e-6, d = core.depth / 2 - 2e-6;
    for (const auto& path : outline)
        for (size_t i = 0; i < path.size (); ++i) {
            const auto a = local (path[i]), b = local (path[(i + 1) % path.size ()]);
            double lo = 0, hi = 1;
            const auto clip = [&] (double at, double slope, double extent) {
                if (std::abs (slope) < 1e-12)
                    return std::abs (at) < extent;
                double t0 = (-extent - at) / slope, t1 = (extent - at) / slope;
                if (t0 > t1)
                    std::swap (t0, t1);
                lo = (std::max) (lo, t0);
                hi = (std::min) (hi, t1);
                return hi > lo;
            };
            if (clip (a.x, b.x - a.x, w) && clip (a.y, b.y - a.y, d))
                return false;
        }
    return true;
}
double Clearance (const cp::PathsD& outline, const Core& core, double angle)
{
    const auto corners = Corners ({ {}, core.width, core.depth }, angle);
    const auto distance = [] (Point p, Point a, Point b) {
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double t =
            std::clamp (((p.x - a.x) * dx + (p.y - a.y) * dy) / (std::max) (dx * dx + dy * dy, 1e-20), 0.0, 1.0);
        return std::hypot (p.x - a.x - t * dx, p.y - a.y - t * dy);
    };
    double gap = 1e300;
    for (const auto& path : outline)
        for (size_t i = 0; i < path.size (); ++i) {
            const Point a { path[i].x, path[i].y },
                b { path[(i + 1) % path.size ()].x, path[(i + 1) % path.size ()].y };
            for (size_t j = 0; j < corners.size (); ++j) {
                const auto c = corners[j], d = corners[(j + 1) % corners.size ()];
                gap = (std::min) ({ gap, distance (c, a, b), distance (d, a, b), distance (a, c, d),
                                    distance (b, c, d) });
            }
        }
    return gap;
}
// Half extents of the turned core along unit direction (x, y).
double Support (const Core& core, double angle, double x, double y)
{
    const double u = x * std::cos (angle) + y * std::sin (angle), v = -x * std::sin (angle) + y * std::cos (angle);
    return core.width / 2 * std::abs (u) + core.depth / 2 * std::abs (v);
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
        floor.outlineKey = QuickSignature (floor, {});
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
bool Fits (const Floor& floor, const Core& core, double angle)
{
    return Valid (core.center) && Sized (core) && std::isfinite (angle) &&
           FitsOutline (Outline (floor, core.center), core, angle, {});
}
bool CoreAllowed (const Floor& floor, const Core& core, double angle)
{
    if (!Valid (core.center) || !Sized (core) || !std::isfinite (angle))
        return false;
    const auto outline = Outline (floor, core.center);
    if (!FitsOutline (outline, core, angle, {}))
        return false;
    const double gap = Clearance (outline, core, angle);
    return gap <= 1e-4 || gap >= kCoreFacadeGap - 1e-6;
}
Point Snap (const Floor& floor, const Core& core, double angle)
{
    const Point center = core.center;
    if (!Valid (center) || !Sized (core) || !std::isfinite (angle))
        return center;
    const auto outline = Outline (floor, center);
    const double snapDistance = FitsOutline (outline, core, angle, {}) ? kCoreFacadeGap : kSnapDistance;
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
            const double extent = Support (core, angle, tx, ty);
            if (along + extent < 0 || along - extent > length)
                continue; // Snap to a segment, never its infinite extension.
            const double distance = -a.x * nx - a.y * ny;
            const double support = Support (core, angle, nx, ny);
            for (double sign : { -1.0, 1.0 }) {
                const double shift = sign * support - distance;
                if (std::abs (shift) > snapDistance)
                    continue;
                candidates.push_back ({ std::abs (shift), { nx * shift, ny * shift }, { nx, ny }, shift });
                std::stable_sort (candidates.begin (), candidates.end (),
                                  [] (const auto& lhs, const auto& rhs) { return lhs.distance < rhs.distance; });
                if (candidates.size () > 8)
                    candidates.pop_back ();
            }
        }
    for (const auto& candidate : candidates)
        if (FitsOutline (outline, core, angle, candidate.delta))
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
            if (std::hypot (delta.x, delta.y) <= snapDistance && FitsOutline (outline, core, angle, delta))
                return { center.x + delta.x, center.y + delta.y };
        }
    return center;
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
    const bool acknowledged = draft.known && draft.cores == plan.saved &&
                              floorscheme::edit::ToJson (draft.designs) == plan.savedDesigns &&
                              draft.guids == plan.guids && !plan.mixed;
    auto quickPlans = acknowledged ? std::move (draft.quickPlans) : std::map<int, QuickPlan> {};
    auto uniqueFloors = acknowledged ? std::move (draft.uniqueFloors) : std::set<int> {};
    auto programme = std::move (draft.programme);
    const auto newCore = draft.newCore;
    const bool moveCores = draft.moveCores;
    draft = {};
    draft.quickPlans = std::move (quickPlans);
    draft.uniqueFloors = std::move (uniqueFloors);
    draft.programme = floorprogramme::Valid (programme) ? std::move (programme) : floorprogramme::Default ();
    draft.newCore = newCore;
    draft.moveCores = moveCores;
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
    if (!Dirty (draft) || draft.dragging || Conflict (plan, draft) || plan.floors.empty ())
        return {};
    if (draft.cores.size () > kMaxStairs ||
        std::any_of (draft.cores.begin (), draft.cores.end (),
                     [] (const Core& core) { return !Valid (core.center) || !Sized (core); }))
        return {};
    for (const auto& floor : plan.floors)
        for (const auto& core : draft.cores)
            if (!CoreAllowed (floor, core, plan.angle))
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
bool Place (const Floor& floor, Draft& draft, Point point, double angle)
{
    const bool moving = draft.selected >= 0 && size_t (draft.selected) < draft.cores.size ();
    Core core = moving ? draft.cores[size_t (draft.selected)] : draft.newCore;
    core.center = point;
    core.center = Snap (floor, core, angle);
    if (!draft.placing || !CoreAllowed (floor, core, angle))
        return false;
    for (size_t i = 0; i < draft.cores.size (); ++i)
        if (int (i) != draft.selected &&
            std::hypot (draft.cores[i].center.x - core.center.x, draft.cores[i].center.y - core.center.y) < 1e-6)
            return false;
    if (moving)
        draft.cores[size_t (draft.selected)] = core;
    else {
        if (draft.cores.size () >= kMaxStairs)
            return false;
        draft.selected = int (draft.cores.size ());
        draft.cores.push_back (core);
    }
    draft.placing = false;
    draft.changed = true;
    return true;
}
bool BeginDrag (Draft& draft, Point mouse, uintptr_t owner, double angle)
{
    if (!draft.moving || draft.dragging || draft.selected < 0 || size_t (draft.selected) >= draft.cores.size () ||
        !Valid (mouse))
        return false;
    const auto& core = draft.cores[size_t (draft.selected)];
    const double x = mouse.x - core.center.x, y = mouse.y - core.center.y;
    const double u = x * std::cos (angle) + y * std::sin (angle), v = -x * std::sin (angle) + y * std::cos (angle);
    if (std::abs (u) > core.width / 2 || std::abs (v) > core.depth / 2)
        return false;
    draft.dragOriginal = core.center;
    draft.dragOffset = { core.center.x - mouse.x, core.center.y - mouse.y };
    draft.dragging = true;
    draft.dragOwner = owner;
    return true;
}
bool Drag (const Floor& floor, Draft& draft, Point mouse, double angle)
{
    if (!draft.dragging || draft.selected < 0 || size_t (draft.selected) >= draft.cores.size ())
        return false;
    auto core = draft.cores[size_t (draft.selected)];
    core.center = { mouse.x + draft.dragOffset.x, mouse.y + draft.dragOffset.y };
    core.center = Snap (floor, core, angle);
    if (!CoreAllowed (floor, core, angle))
        return false; // Retain the last valid position across holes/outside the canvas.
    for (size_t i = 0; i < draft.cores.size (); ++i)
        if (int (i) != draft.selected &&
            std::hypot (draft.cores[i].center.x - core.center.x, draft.cores[i].center.y - core.center.y) < 1e-6)
            return false;
    draft.cores[size_t (draft.selected)] = core;
    return true;
}
void EndDrag (Draft& draft)
{
    if (draft.dragging && draft.selected >= 0 && size_t (draft.selected) < draft.cores.size ())
        draft.changed |= draft.cores[size_t (draft.selected)].center != draft.dragOriginal;
    draft.moving = draft.dragging = false;
    draft.dragOwner = 0;
}
void Cancel (Draft& draft)
{
    for (auto& [story, plan] : draft.quickPlans)
        CancelUnits (plan);
    if (draft.dragging && draft.selected >= 0 && size_t (draft.selected) < draft.cores.size ())
        draft.cores[size_t (draft.selected)].center = draft.dragOriginal;
    draft.placing = draft.moving = draft.dragging = false;
    draft.dragOwner = 0;
}
bool NeedsTwoStairs (double areaM2, const massingareas::Coefficients& coefficients)
{
    return areaM2 * coefficients.grossFactor > 500.0;
}
} // namespace geomsrv::archviz::buildingplan
