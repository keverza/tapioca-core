#include "Edit/EditTracker.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditShape.hpp"
#include <cmath>
#include <cstdlib>

namespace geomsrv::edit {

const char* QuantityName (Quantity quantity)
{
    switch (quantity) {
        case Quantity::X:
            return "X";
        case Quantity::Y:
            return "Y";
        case Quantity::Z:
            return "Z";
        case Quantity::Length:
            return "Length";
        case Quantity::Angle:
            return "Angle";
        case Quantity::Elevation:
            return "Elevation";
        case Quantity::Slope:
            return "Slope";
        case Quantity::Width:
            return "Width";
        case Quantity::Distance:
            return "Distance";
        case Quantity::Height:
            return "Height";
        case Quantity::Radius:
            return "Radius";
        case Quantity::Factor:
            return "Scale";
    }
    return "";
}

const char* QuantityUnit (Quantity quantity)
{
    switch (quantity) {
        case Quantity::Angle:
            return "deg";
        case Quantity::Slope:
            return "%";
        case Quantity::Factor:
            return "x";
        default:
            return "m";
    }
}

bool ParseNumber (const std::string& text, double& value)
{
    size_t i = 0;
    while (i < text.size () && text[i] == ' ')
        ++i;
    size_t end = text.size ();
    while (end > i && text[end - 1] == ' ')
        --end;
    std::string clean;
    bool digit = false, point = false;
    for (size_t k = i; k < end; ++k) {
        const char c = text[k];
        if ((c == '-' || c == '+') && clean.empty ())
            clean += c;
        else if (c >= '0' && c <= '9') {
            clean += c;
            digit = true;
        }
        else if ((c == '.' || c == ',') && !point) {
            clean += '.';
            point = true;
        }
        else
            return false;
    }
    if (!digit)
        return false;
    value = std::strtod (clean.c_str (), nullptr);
    return std::isfinite (value);
}

void Tracker::Show (const std::vector<TrackerItem>& next, size_t defaultItem)
{
    bool same = next.size () == items.size () && state != TrackerState::Hidden;
    for (size_t i = 0; same && i < next.size (); ++i)
        same = next[i].quantity == items[i].quantity;
    if (same) {
        for (size_t i = 0; i < next.size (); ++i) {
            items[i].value = next[i].value;
            items[i].editable = next[i].editable;
        }
        return;
    }
    items = next;
    text.clear ();
    state = items.empty () ? TrackerState::Hidden : TrackerState::Showing;
    focus = defaultItem < items.size () ? defaultItem : 0;
    if (!items.empty () && !items[focus].editable)
        Advance (1);
}

void Tracker::Hide ()
{
    items.clear ();
    text.clear ();
    state = TrackerState::Hidden;
    focus = 0;
}

void Tracker::Advance (int step)
{
    const size_t n = items.size ();
    for (size_t k = 1; k <= n; ++k) {
        const size_t i = (focus + n + static_cast<size_t> (step > 0 ? k : n - k)) % n;
        if (items[i].editable) {
            focus = i;
            return;
        }
    }
}

Tracker::Event Tracker::Key (const TrackerKey& key)
{
    using K = TrackerKey::Kind;
    Event event;
    event.item = focus;
    if (state == TrackerState::Hidden)
        return event;
    if (state == TrackerState::Showing) {
        const bool starts = key.kind == K::Char && ((key.c >= '0' && key.c <= '9') || key.c == '-' || key.c == '+' ||
                                                    key.c == '.' || key.c == ',');
        if (starts && items[focus].editable) {
            state = TrackerState::Editing;
            text = std::string (1, key.c);
        }
        else if (key.kind == K::Tab || key.kind == K::BackTab)
            Advance (key.kind == K::Tab ? 1 : -1);
        else if (key.kind == K::Escape)
            event.outcome = Outcome::CancelGesture;
        return event;
    }
    switch (key.kind) {
        case K::Char:
            text += key.c;
            break;
        case K::Backspace:
            if (!text.empty ())
                text.pop_back ();
            if (text.empty ())
                state = TrackerState::Showing;
            break;
        case K::Escape:
            text.clear ();
            state = TrackerState::Showing;
            break;
        case K::Enter:
        case K::Tab:
        case K::BackTab: {
            double value = 0.0;
            if (!ParseNumber (text, value)) {
                event.outcome = Outcome::Invalid;
                return event;
            }
            event.value = value;
            text.clear ();
            state = TrackerState::Showing;
            if (key.kind == K::Enter) {
                event.outcome = Outcome::Commit;
                return event;
            }
            items[focus].locked = true;
            items[focus].lockedValue = value;
            event.outcome = Outcome::Locked;
            Advance (key.kind == K::Tab ? 1 : -1);
            break;
        }
    }
    return event;
}

TrackerState Tracker::State () const
{
    return state;
}

size_t Tracker::Focus () const
{
    return focus;
}

const std::string& Tracker::Text () const
{
    return text;
}

const std::vector<TrackerItem>& Tracker::Items () const
{
    return items;
}

std::optional<double> Tracker::Locked (Quantity quantity) const
{
    for (const auto& item : items)
        if (item.quantity == quantity && item.locked)
            return item.lockedValue;
    return std::nullopt;
}

namespace {

constexpr double kPi = 3.14159265358979323846;

// Resolves a segment ref to its entity and end indices; `pivot` is the end that
// stays put (the start, unless only the start is locked).
// Which end of segment `index` stays: the asked one, unless the other is locked.
void FixedEnd (const Entity& e, size_t index, bool fromEnd, bool z, size_t& pivot, size_t& moving, bool& forward)
{
    SegmentEnds (e, index, pivot, moving);
    const auto locked = [&] (size_t i) { return z ? e.vertices[i].lockZ : e.vertices[i].lockXY; };
    forward = !fromEnd;
    if (locked (forward ? moving : pivot))
        forward = !forward;
    if (!forward)
        std::swap (pivot, moving);
}

bool SegmentOf (Document& doc, const Ref& ref, Entity*& entity, size_t& pivot, size_t& moving, bool& forward, bool z,
                std::string& error, bool fromEnd = false)
{
    entity = Find (doc, ref.entity);
    const int index = entity != nullptr && ref.part == Part::Segment ? SegmentIndex (*entity, ref.item) : -1;
    if (index < 0) {
        error = "the dimension's segment no longer exists";
        return false;
    }
    FixedEnd (*entity, static_cast<size_t> (index), fromEnd, z, pivot, moving, forward);
    const auto locked = [&] (size_t i) { return z ? entity->vertices[i].lockZ : entity->vertices[i].lockXY; };
    if (locked (moving)) {
        error = "both ends of the segment are locked";
        return false;
    }
    return true;
}

bool SegmentOf (const Document& doc, const Ref& ref, Vec3& a, Vec3& b)
{
    const Entity* e = Find (doc, ref.entity);
    const int index = e != nullptr && ref.part == Part::Segment ? SegmentIndex (*e, ref.item) : -1;
    if (index < 0)
        return false;
    size_t from = 0, to = 0;
    SegmentEnds (*e, static_cast<size_t> (index), from, to);
    a = e->vertices[from].p;
    b = e->vertices[to].p;
    return true;
}

Vertex* VertexOf (Document& doc, const Ref& ref)
{
    Entity* e = Find (doc, ref.entity);
    const int index = e != nullptr && ref.part == Part::Vertex ? VertexIndex (*e, ref.item) : -1;
    return index < 0 ? nullptr : &e->vertices[static_cast<size_t> (index)];
}

} // namespace

namespace {

// On a path: a control point shows its corner radius, a grade break its elevation,
// and a range its total width and crossfall.
std::vector<Dimension> PathDimensions (const Document& doc, const Ref& ref)
{
    std::vector<Dimension> out;
    if (!Exists (doc, ref))
        return out;
    switch (ref.part) {
        case Part::PathPoint:
            out.push_back ({ ref, Quantity::Radius });
            break;
        case Part::Break:
            out.push_back ({ ref, Quantity::Elevation });
            break;
        case Part::Range:
            out.push_back ({ ref, Quantity::Width });
            out.push_back ({ ref, Quantity::Slope });
            break;
        default:
            break;
    }
    return out;
}

bool ReadPath (const Path& p, const Dimension& d, double& value)
{
    for (const auto& c : p.points)
        if (d.ref.part == Part::PathPoint && c.id == d.ref.item && d.quantity == Quantity::Radius) {
            value = c.radius;
            return true;
        }
    for (const auto& b : p.profile)
        if (d.ref.part == Part::Break && b.id == d.ref.item && d.quantity == Quantity::Elevation) {
            value = b.z;
            return true;
        }
    for (const auto& r : p.sections)
        if (d.ref.part == Part::Range && r.id == d.ref.item) {
            if (d.quantity == Quantity::Width) {
                value = r.widthLeft + r.widthRight;
                return true;
            }
            if (d.quantity == Quantity::Slope) {
                value = r.crossfall;
                return true;
            }
        }
    return false;
}

bool WritePath (Document& doc, const Dimension& d, double value, std::string& error)
{
    const Path* p = FindPath (doc, d.ref.entity);
    if (p == nullptr) {
        error = "the path no longer exists";
        return false;
    }
    if (d.ref.part == Part::PathPoint && d.quantity == Quantity::Radius)
        return SetPathRadius (doc, d.ref.entity, d.ref.item, value, error);
    if (d.ref.part == Part::Break && d.quantity == Quantity::Elevation)
        return SetPathElevation (doc, d.ref.entity, d.ref.item, value, error);
    if (d.ref.part == Part::Range)
        for (const auto& r : p->sections) {
            if (r.id != d.ref.item)
                continue;
            if (d.quantity == Quantity::Slope)
                return SetPathCrossfall (doc, d.ref.entity, r.id, value, r.fall, error);
            if (d.quantity == Quantity::Width) {
                if (!(value > 0.0)) {
                    error = "a width must be positive";
                    return false;
                }
                // Both sides keep their share of the total.
                const double total = r.widthLeft + r.widthRight;
                return SetPathWidth (doc, d.ref.entity, r.id, value * r.widthLeft / total, value * r.widthRight / total,
                                     error);
            }
        }
    error = "that dimension cannot be typed on a path";
    return false;
}

} // namespace

std::vector<Dimension> DimensionsFor (const Document& doc, const Ref& ref)
{
    if (FindPath (doc, ref.entity) != nullptr)
        return PathDimensions (doc, ref);
    std::vector<Dimension> out;
    const Entity* e = Find (doc, ref.entity);
    if (e == nullptr || !Exists (doc, ref))
        return out;
    switch (ref.part) {
        case Part::Segment:
            // A curve has a length along it; its direction and fall are its handles'.
            if (SegmentCurved (*e, static_cast<size_t> (SegmentIndex (*e, ref.item))))
                out.push_back ({ ref, Quantity::Length });
            else
                for (Quantity q : { Quantity::Length, Quantity::Slope, Quantity::Angle })
                    out.push_back ({ ref, q });
            if (e->segments[static_cast<size_t> (SegmentIndex (*e, ref.item))].width > 0.0)
                out.push_back ({ ref, Quantity::Width });
            break;
        case Part::Vertex: {
            for (Quantity q : { Quantity::Elevation, Quantity::X, Quantity::Y })
                out.push_back ({ ref, q });
            CornerGeometry c;
            const int i = VertexIndex (*e, ref.item);
            if (i >= 0 && CornerOf (*e, size_t (i), c))
                out.push_back ({ ref, Quantity::Radius }); // a corner can be rounded
            break;
        }
        case Part::Face:
        case Part::Entity:
            if (e->kind == Kind::Extrusion)
                out.push_back ({ { ref.entity, Part::Entity, 0 }, Quantity::Height });
            break;
        case Part::PathPoint:
        case Part::Break:
        case Part::Range:
            break;
    }
    return out;
}

bool SegmentEndsFor (const Document& doc, const Dimension& d, Vec3& fixed, Vec3& moving, bool& fixedIsStart)
{
    const Entity* e = Find (doc, d.ref.entity);
    const int index = e != nullptr && d.ref.part == Part::Segment ? SegmentIndex (*e, d.ref.item) : -1;
    if (index < 0)
        return false;
    size_t pivot = 0, other = 0;
    FixedEnd (*e, static_cast<size_t> (index), d.fromEnd, d.quantity == Quantity::Slope, pivot, other, fixedIsStart);
    fixed = e->vertices[pivot].p;
    moving = e->vertices[other].p;
    return true;
}

bool ReadDimension (const Document& doc, const Dimension& d, double& value)
{
    if (const Path* p = FindPath (doc, d.ref.entity))
        return ReadPath (*p, d, value);
    if (d.ref.part == Part::Segment) {
        Vec3 a, b;
        if (!SegmentOf (doc, d.ref, a, b))
            return false;
        const double plan = DistanceXY (a, b);
        switch (d.quantity) {
            case Quantity::Length: {
                const Entity* e = Find (doc, d.ref.entity);
                const size_t i = static_cast<size_t> (SegmentIndex (*e, d.ref.item));
                if (SegmentCurved (*e, i)) {
                    // Along the curve, in plan.
                    const auto points = SegmentPoints (*e, i, 64);
                    value = 0.0;
                    for (size_t k = 0; k + 1 < points.size (); ++k)
                        value += DistanceXY (points[k], points[k + 1]);
                    return true;
                }
                value = plan;
                return true;
            }
            case Quantity::Distance:
                value = Distance (a, b);
                return true;
            case Quantity::Slope:
                if (plan < kMinSegment)
                    return false; // vertical: slope is undefined, not infinite
                value = 100.0 * (b.z - a.z) / plan;
                return true;
            case Quantity::Angle: {
                Vec3 fixed, moving;
                bool forward = true;
                if (!SegmentEndsFor (doc, d, fixed, moving, forward))
                    return false;
                value = std::atan2 (moving.y - fixed.y, moving.x - fixed.x) * 180.0 / kPi;
                return true;
            }
            case Quantity::Width: {
                const Entity* e = Find (doc, d.ref.entity);
                value = e->segments[static_cast<size_t> (SegmentIndex (*e, d.ref.item))].width;
                return true;
            }
            default:
                return false;
        }
    }
    if (d.ref.part == Part::Vertex) {
        Vec3 p;
        if (!PointOf (doc, d.ref, p))
            return false;
        if (d.quantity == Quantity::Radius) {
            const Entity* e = Find (doc, d.ref.entity);
            value = e->vertices[size_t (VertexIndex (*e, d.ref.item))].radius;
            return true;
        }
        switch (d.quantity) {
            case Quantity::X:
                value = p.x;
                return true;
            case Quantity::Y:
                value = p.y;
                return true;
            case Quantity::Z:
            case Quantity::Elevation:
                value = p.z;
                return true;
            default:
                return false;
        }
    }
    const Entity* e = Find (doc, d.ref.entity);
    if (e != nullptr && d.quantity == Quantity::Height && e->kind == Kind::Extrusion) {
        value = e->height;
        return true;
    }
    return false;
}

Operation WriteDimension (const Dimension& d, double value)
{
    return [d, value] (Document& doc, std::string& error) {
        if (!std::isfinite (value)) {
            error = "not a number";
            return false;
        }
        if (FindPath (doc, d.ref.entity) != nullptr)
            return WritePath (doc, d, value, error);
        if (d.ref.part == Part::Segment) {
            const bool z = d.quantity == Quantity::Slope;
            Entity* e = nullptr;
            size_t pivot = 0, moving = 0;
            bool forward = true; // the fixed end is the segment's start
            if (d.quantity == Quantity::Width) {
                e = Find (doc, d.ref.entity);
                const int i = e != nullptr ? SegmentIndex (*e, d.ref.item) : -1;
                if (i < 0 || value < 0.0) {
                    error = i < 0 ? "the dimension's segment no longer exists" : "a width cannot be negative";
                    return false;
                }
                e->segments[static_cast<size_t> (i)].width = value;
                return true;
            }
            if (!SegmentOf (doc, d.ref, e, pivot, moving, forward, z, error, d.fromEnd))
                return false;
            if (SegmentCurved (*e, static_cast<size_t> (SegmentIndex (*e, d.ref.item)))) {
                error = "a curve is shaped by its handles: drag them, or make the segment straight";
                return false;
            }
            Vertex& m = e->vertices[moving];
            const Vec3 p = e->vertices[pivot].p;
            const double plan = DistanceXY (p, m.p);
            switch (d.quantity) {
                case Quantity::Length:
                    if (!(value > kMinSegment) || plan < kMinSegment) {
                        error = "a length must be positive";
                        return false;
                    }
                    m.p.x = p.x + (m.p.x - p.x) * value / plan;
                    m.p.y = p.y + (m.p.y - p.y) * value / plan;
                    return true;
                case Quantity::Distance: {
                    const double full = Distance (p, m.p);
                    if (!(value > kMinSegment) || full < kMinSegment) {
                        error = "a distance must be positive";
                        return false;
                    }
                    if (m.lockZ && std::abs (m.p.z - p.z) > 0.0) {
                        error = "the moving end's elevation is locked";
                        return false;
                    }
                    m.p = p + (m.p - p) * (value / full);
                    return true;
                }
                case Quantity::Slope: {
                    if (plan < kMinSegment) {
                        error = "a vertical segment has no slope";
                        return false;
                    }
                    // A slope reads start to end, whichever end stays put.
                    m.p.z = p.z + (forward ? 1.0 : -1.0) * value / 100.0 * plan;
                    return true;
                }
                case Quantity::Angle: {
                    // The direction from the end that stays to the end that moves, as read.
                    const double radians = value * kPi / 180.0;
                    m.p.x = p.x + plan * std::cos (radians);
                    m.p.y = p.y + plan * std::sin (radians);
                    return true;
                }
                default:
                    error = "that dimension cannot be typed on a segment";
                    return false;
            }
        }
        if (d.ref.part == Part::Vertex) {
            if (d.quantity == Quantity::Radius)
                return SetCornerRadius (doc, d.ref.entity, { d.ref.item }, value, error);
            Vertex* v = VertexOf (doc, d.ref);
            if (v == nullptr) {
                error = "the dimension's vertex no longer exists";
                return false;
            }
            const bool z = d.quantity == Quantity::Z || d.quantity == Quantity::Elevation;
            if ((z && v->lockZ) || (!z && v->lockXY)) {
                error = "that vertex is locked";
                return false;
            }
            (d.quantity == Quantity::X ? v->p.x : d.quantity == Quantity::Y ? v->p.y : v->p.z) = value;
            return true;
        }
        Entity* e = Find (doc, d.ref.entity);
        if (e != nullptr && d.quantity == Quantity::Height && e->kind == Kind::Extrusion) {
            if (!(value > kMinSegment)) {
                error = "a height must be positive";
                return false;
            }
            e->height = value;
            return true;
        }
        error = "that dimension cannot be typed here";
        return false;
    };
}

} // namespace geomsrv::edit
