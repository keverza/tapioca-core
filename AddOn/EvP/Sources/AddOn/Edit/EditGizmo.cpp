#include "Edit/EditGizmo.hpp"
#include "Edit/EditPathEdit.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace geomsrv::edit {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kEdgeOnLine = 0.999; // |ray . axis| above this: the line points at the camera
constexpr double kEdgeOnPlane = 1.0e-3;
} // namespace

bool SegmentFrame (Vec3 a, Vec3 b, Frame& frame)
{
    Vec3 x;
    if (!Normalize (Vec3 { b.x - a.x, b.y - a.y, 0.0 }, x))
        return false;
    frame.origin = a;
    frame.axes[0] = x;
    frame.axes[2] = { 0.0, 0.0, 1.0 };
    frame.axes[1] = Cross (frame.axes[2], x);
    return true;
}

bool FaceFrame (const Entity& entity, size_t face, Frame& frame)
{
    Vec3 n;
    if (!FaceNormal (entity, face, n))
        return false;
    const auto corners = FacePolygon (entity, face);
    Vec3 centre;
    for (const auto& c : corners)
        centre = centre + c;
    frame.origin = centre * (1.0 / static_cast<double> (corners.size ()));
    frame.axes[2] = n; // the face handle drags along axis 2
    const Vec3 seed = std::abs (n.z) < 0.9 ? Vec3 { 0.0, 0.0, 1.0 } : Vec3 { 1.0, 0.0, 0.0 };
    Normalize (Cross (seed, n), frame.axes[0]);
    frame.axes[1] = Cross (n, frame.axes[0]);
    return true;
}

Frame SelectionFrame (const Document& doc, const std::vector<Ref>& items, Vec3 origin)
{
    Frame world;
    world.origin = origin;
    if (items.empty () || FindPath (doc, items.front ().entity) != nullptr)
        return world;
    for (const auto& r : items)
        if (r.entity != items.front ().entity)
            return world;
    const Entity* e = Find (doc, items.front ().entity);
    if (e == nullptr || e->vertices.size () < 2)
        return world;
    size_t from = 0, to = 0;
    if (items.size () == 1 && items.front ().part == Part::Segment) {
        const int i = SegmentIndex (*e, items.front ().item);
        if (i < 0)
            return world;
        SegmentEnds (*e, static_cast<size_t> (i), from, to);
    }
    else {
        double best = -1.0;
        for (size_t i = 0; i < SegmentCount (*e); ++i) {
            size_t a = 0, b = 0;
            SegmentEnds (*e, i, a, b);
            const double length = DistanceXY (e->vertices[a].p, e->vertices[b].p);
            if (length > best + 1.0e-9 * std::max (1.0, best))
                best = length, from = a, to = b;
        }
    }
    Frame f;
    if (!SegmentFrame (e->vertices[from].p, e->vertices[to].p, f))
        return world;
    f.origin = origin;
    return f;
}

std::vector<Handle> Handles (GizmoMode mode)
{
    std::vector<Handle> h;
    const auto add = [&] (HandleKind kind, int axis, bool focusable, uint32_t group, Quantity q) {
        h.push_back ({ static_cast<uint32_t> (h.size ()), kind, axis, focusable, true, group, q });
    };
    switch (mode) {
        case GizmoMode::Move:
            add (HandleKind::Centre, -1, false, 2, Quantity::Distance);
            for (int a = 0; a < 3; ++a)
                add (HandleKind::Axis, a, true, 0, Quantity::Distance);
            for (int a = 0; a < 3; ++a)
                add (HandleKind::Plane, a, false, 1, Quantity::Distance);
            break;
        case GizmoMode::Rotate:
            add (HandleKind::Centre, -1, false, 2, Quantity::Angle);
            for (int a = 0; a < 3; ++a)
                add (HandleKind::Ring, a, true, 0, Quantity::Angle);
            break;
        case GizmoMode::Scale:
            add (HandleKind::Centre, -1, false, 2, Quantity::Distance);
            add (HandleKind::ScaleUniform, 2, true, 1, Quantity::Distance);
            for (int a = 0; a < 3; ++a)
                add (HandleKind::ScaleAxis, a, true, 0, Quantity::Distance);
            break;
        case GizmoMode::Stretch:
            for (int a = 0; a < 3; ++a)
                add (HandleKind::Axis, a, true, 0, Quantity::Distance);
            break;
        case GizmoMode::Extrude:
            add (HandleKind::Face, 2, true, 0, Quantity::Distance);
            break;
    }
    return h;
}

static Vec3 Rotate (Vec3 v, Vec3 axis, double angle)
{
    // Rodrigues: v cos + (k x v) sin + k (k . v)(1 - cos)
    const double c = std::cos (angle), s = std::sin (angle);
    return v * c + Cross (axis, v) * s + axis * (Dot (axis, v) * (1.0 - c));
}

Vec3 Apply (const Transform& t, Vec3 p)
{
    Vec3 d = p - t.pivot;
    const Frame& f = t.scaleFrame;
    const double k[3] = { Dot (d, f.axes[0]) * t.scale.x, Dot (d, f.axes[1]) * t.scale.y,
                          Dot (d, f.axes[2]) * t.scale.z };
    d = f.axes[0] * k[0] + f.axes[1] * k[1] + f.axes[2] * k[2];
    Vec3 axis { 0.0, 0.0, 1.0 };
    if (t.angle != 0.0 && Normalize (t.rotationAxis, axis))
        d = Rotate (d, axis, t.angle);
    return t.pivot + d + t.translate;
}

Frame Moved (const Frame& frame, const Transform& t)
{
    Frame out = frame;
    out.origin = Apply (t, frame.origin);
    Vec3 axis;
    if (t.angle != 0.0 && Normalize (t.rotationAxis, axis))
        for (auto& a : out.axes)
            a = Rotate (a, axis, t.angle);
    return out;
}

namespace {

bool Unit (const Ray& ray, Vec3& dir)
{
    return Finite (ray.origin) && Normalize (ray.direction, dir);
}

// Parameter along origin + t axis of the point nearest the ray.
bool LineParameter (Vec3 origin, Vec3 axis, const Ray& ray, double& t)
{
    Vec3 d;
    if (!Unit (ray, d))
        return false;
    const double b = Dot (axis, d);
    if (std::abs (b) > kEdgeOnLine)
        return false;
    const Vec3 w = origin - ray.origin;
    t = (b * Dot (w, d) - Dot (w, axis)) / (1.0 - b * b);
    return std::isfinite (t);
}

bool PlaneHit (Vec3 origin, Vec3 normal, const Ray& ray, Vec3& hit)
{
    Vec3 d;
    if (!Unit (ray, d))
        return false;
    const double den = Dot (normal, d);
    if (std::abs (den) < kEdgeOnPlane)
        return false;
    const double t = Dot (origin - ray.origin, normal) / den;
    if (!(t >= 0.0))
        return false; // behind the eye
    hit = ray.origin + d * t;
    return true;
}

DragResult Refuse (const char* why)
{
    DragResult r;
    r.error = why;
    return r;
}

} // namespace

DragResult Drag (const Frame& frame, const Handle& handle, const Ray& start, const Ray& now)
{
    DragResult r;
    r.transform.pivot = frame.origin;
    r.transform.scaleFrame = frame;
    const Vec3 axis = handle.axis >= 0 && handle.axis < 3 ? frame.axes[handle.axis] : frame.axes[2];
    switch (handle.kind) {
        case HandleKind::Axis:
        case HandleKind::Face: {
            double t0 = 0.0, t1 = 0.0;
            if (!LineParameter (frame.origin, axis, start, t0) || !LineParameter (frame.origin, axis, now, t1))
                return Refuse ("the handle points into the view");
            r.value = t1 - t0;
            if (handle.kind == HandleKind::Face)
                r.transform.extrude = r.value;
            else
                r.transform.translate = axis * r.value;
            break;
        }
        case HandleKind::Plane:
        case HandleKind::Centre: {
            const Vec3 normal = handle.kind == HandleKind::Centre ? frame.axes[2] : axis;
            Vec3 h0, h1;
            if (!PlaneHit (frame.origin, normal, start, h0) || !PlaneHit (frame.origin, normal, now, h1))
                return Refuse ("the plane is seen edge-on");
            r.transform.translate = h1 - h0;
            r.transform.relocate = handle.kind == HandleKind::Centre;
            r.value = Length (r.transform.translate);
            break;
        }
        case HandleKind::Ring: {
            Vec3 h0, h1;
            if (!PlaneHit (frame.origin, axis, start, h0) || !PlaneHit (frame.origin, axis, now, h1))
                return Refuse ("the ring is seen edge-on");
            const Vec3 a = h0 - frame.origin, b = h1 - frame.origin;
            if (Length (a) < 1.0e-9 || Length (b) < 1.0e-9)
                return Refuse ("the drag started on the pivot");
            const double angle = std::atan2 (Dot (Cross (a, b), axis), Dot (a, b));
            r.transform.rotationAxis = axis;
            r.transform.angle = angle;
            r.value = angle * 180.0 / kPi;
            break;
        }
        case HandleKind::ScaleAxis: {
            double t0 = 0.0, t1 = 0.0;
            if (!LineParameter (frame.origin, axis, start, t0) || !LineParameter (frame.origin, axis, now, t1))
                return Refuse ("the handle points into the view");
            if (std::abs (t0) < 1.0e-9)
                return Refuse ("the drag started on the pivot");
            r.value = t1 / t0;
            (handle.axis == 0   ? r.transform.scale.x
             : handle.axis == 1 ? r.transform.scale.y
                                : r.transform.scale.z) = r.value;
            break;
        }
        case HandleKind::ScaleUniform: {
            Vec3 h0, h1;
            if (!PlaneHit (frame.origin, axis, start, h0) || !PlaneHit (frame.origin, axis, now, h1))
                return Refuse ("the handle's plane is seen edge-on");
            const double d0 = Distance (h0, frame.origin);
            if (d0 < 1.0e-9)
                return Refuse ("the drag started on the pivot");
            r.value = Distance (h1, frame.origin) / d0;
            r.transform.scale = { r.value, r.value, r.value };
            break;
        }
    }
    r.ok = true;
    return r;
}

DragResult FromValue (const Frame& frame, const Handle& handle, double value)
{
    if (!handle.focusable || !std::isfinite (value))
        return Refuse ("that handle takes no typed value");
    DragResult r;
    r.ok = true;
    r.value = value;
    r.transform.pivot = frame.origin;
    r.transform.scaleFrame = frame;
    const Vec3 axis = handle.axis >= 0 && handle.axis < 3 ? frame.axes[handle.axis] : frame.axes[2];
    switch (handle.kind) {
        case HandleKind::Axis:
            r.transform.translate = axis * value;
            break;
        case HandleKind::Face:
            r.transform.extrude = value;
            break;
        case HandleKind::Ring:
            r.transform.rotationAxis = axis;
            r.transform.angle = value * kPi / 180.0;
            break;
        case HandleKind::ScaleAxis:
        case HandleKind::ScaleUniform:
            if (!(value > 0.0))
                return Refuse ("a scale factor must be positive");
            if (handle.kind == HandleKind::ScaleUniform)
                r.transform.scale = { value, value, value };
            else
                (handle.axis == 0   ? r.transform.scale.x
                 : handle.axis == 1 ? r.transform.scale.y
                                    : r.transform.scale.z) = value;
            break;
        default:
            return Refuse ("that handle takes no typed value");
    }
    return r;
}

std::vector<HandleShape> HandleGeometry (const Frame& frame, const Handle& handle, double size)
{
    const Vec3 o = frame.origin;
    const int a = handle.axis >= 0 && handle.axis < 3 ? handle.axis : 2;
    const Vec3 axis = frame.axes[a], u = frame.axes[(a + 1) % 3], v = frame.axes[(a + 2) % 3];
    switch (handle.kind) {
        case HandleKind::Centre:
            return { { { o }, false } };
        case HandleKind::Axis:
        case HandleKind::Face:
            return { { { o, o + axis * size }, false } };
        case HandleKind::ScaleAxis: {
            const Vec3 tip = o + axis * (size * 0.7);
            const double b = size * 0.06;
            return { { { o, tip }, false },
                     { { tip + u * b + v * b, tip - u * b + v * b, tip - u * b - v * b, tip + u * b - v * b }, true } };
        }
        case HandleKind::Plane: {
            const double lo = size * 0.25, hi = size * 0.45;
            return { { { o + u * lo + v * lo, o + u * hi + v * lo, o + u * hi + v * hi, o + u * lo + v * hi }, true } };
        }
        case HandleKind::ScaleUniform: {
            const double b = size * 0.15;
            return { { { o + u * b + v * b, o - u * b + v * b, o - u * b - v * b, o + u * b - v * b }, true } };
        }
        case HandleKind::Ring: {
            HandleShape ring;
            ring.closed = true;
            for (int k = 0; k < 48; ++k) {
                const double t = 2.0 * kPi * k / 48.0;
                ring.points.push_back (o + u * (0.8 * size * std::cos (t)) + v * (0.8 * size * std::sin (t)));
            }
            return { ring };
        }
    }
    return {};
}

int PickHandle (const Frame& frame, const std::vector<Handle>& handles, double size, const PickView& view, double x,
                double y, double radius)
{
    int best = -1;
    double bestDistance = radius;
    for (size_t h = 0; h < handles.size (); ++h) {
        if (!handles[h].movable)
            continue;
        for (const auto& shape : HandleGeometry (frame, handles[h], size)) {
            std::vector<std::pair<double, double>> s;
            for (const auto& p : shape.points) {
                double sx = 0.0, sy = 0.0;
                if (!view.toScreen || !view.toScreen (p, sx, sy))
                    break;
                s.push_back ({ sx, sy });
            }
            if (s.size () != shape.points.size ())
                continue;
            // A handle seen end-on collapses to a point on screen and cannot be dragged.
            double extent = 0.0;
            for (const auto& q : s)
                extent = std::max (extent, std::hypot (q.first - s[0].first, q.second - s[0].second));
            if (s.size () > 1 && extent < 2.0)
                continue;
            const auto consider = [&] (double d) {
                if (d < bestDistance || (best < 0 && d <= bestDistance)) {
                    bestDistance = d;
                    best = static_cast<int> (h);
                }
            };
            if (s.size () == 1)
                consider (std::hypot (s[0].first - x, s[0].second - y));
            const size_t edges = shape.closed ? s.size () : s.size () - 1;
            for (size_t i = 0; i < edges && s.size () > 1; ++i) {
                const auto [ax, ay] = s[i];
                const auto [bx, by] = s[(i + 1) % s.size ()];
                const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
                const double t = len2 > 0.0 ? std::clamp (((x - ax) * dx + (y - ay) * dy) / len2, 0.0, 1.0) : 0.0;
                consider (std::hypot (ax + dx * t - x, ay + dy * t - y));
            }
        }
    }
    return best;
}

Operation TransformOperation (const std::vector<Ref>& selection, const Transform& t)
{
    return [selection, t] (Document& doc, std::string& error) {
        if (t.relocate)
            return true; // the pivot moved; the geometry did not
        std::map<Id, std::set<size_t>> moved;
        std::map<Id, std::vector<size_t>> faces;
        std::vector<Ref> pathParts;
        for (const auto& ref : selection) {
            if (FindPath (doc, ref.entity) != nullptr) {
                pathParts.push_back (ref);
                continue;
            }
            Entity* e = Find (doc, ref.entity);
            if (e == nullptr || !Exists (doc, ref)) {
                error = "a selected item no longer exists";
                return false;
            }
            auto& set = moved[e->id];
            const size_t n = e->vertices.size ();
            switch (ref.part) {
                case Part::Entity:
                    for (size_t i = 0; i < n; ++i)
                        set.insert (i);
                    break;
                case Part::Vertex:
                    set.insert (static_cast<size_t> (VertexIndex (*e, ref.item)));
                    break;
                case Part::Segment: {
                    size_t from = 0, to = 0;
                    SegmentEnds (*e, static_cast<size_t> (SegmentIndex (*e, ref.item)), from, to);
                    set.insert (from);
                    set.insert (to);
                    break;
                }
                case Part::Face:
                    if (t.extrude != 0.0)
                        faces[e->id].push_back (ref.item);
                    else if (ref.item < 2)
                        for (size_t i = 0; i < n; ++i)
                            set.insert (i);
                    else {
                        set.insert (ref.item - 2);
                        set.insert ((ref.item - 1) % n);
                    }
                    break;
                case Part::PathPoint:
                case Part::Break:
                case Part::Range:
                    break; // only on paths, handled above
            }
        }
        for (auto& [id, indices] : moved) {
            Entity& e = *Find (doc, id);
            for (size_t i : indices) {
                Vertex& v = e.vertices[i];
                const Vec3 p = Apply (t, v.p);
                const bool xy = std::abs (p.x - v.p.x) > 1.0e-12 || std::abs (p.y - v.p.y) > 1.0e-12;
                const bool z = std::abs (p.z - v.p.z) > 1.0e-12;
                if ((xy && v.lockXY) || (z && v.lockZ)) {
                    error = "a selected vertex is locked";
                    return false;
                }
                v.p = p;
            }
        }
        if (!pathParts.empty ()) {
            if (t.angle != 0.0 || !(t.scale == Vec3 { 1.0, 1.0, 1.0 }) || t.extrude != 0.0) {
                error = "paths move with the gizmo; shape them with their own handles";
                return false;
            }
            if (!TransformPathParts (doc, pathParts, t.translate, error))
                return false;
        }
        for (auto& [id, list] : faces) {
            Entity& e = *Find (doc, id);
            const size_t n = e.vertices.size ();
            for (size_t face : list) {
                if (face == 1)
                    e.height += t.extrude;
                else if (face == 0) {
                    for (auto& v : e.vertices) {
                        if (v.lockZ) {
                            error = "a locked base cannot be pushed";
                            return false;
                        }
                        v.p.z -= t.extrude;
                    }
                    e.height += t.extrude;
                }
                else {
                    Vec3 normal;
                    FaceNormal (e, face, normal);
                    for (size_t i : { face - 2, (face - 1) % n }) {
                        if (e.vertices[i].lockXY) {
                            error = "a locked edge cannot be pushed";
                            return false;
                        }
                        e.vertices[i].p = e.vertices[i].p + normal * t.extrude;
                    }
                }
            }
        }
        return true;
    };
}

} // namespace geomsrv::edit
