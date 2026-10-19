#include "Edit/EditGizmo.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace geomsrv::edit {

std::vector<Handle> Handles (GizmoMode mode, const Document& doc, const std::vector<Ref>& selection, const Frame& frame)
{
    auto handles = Handles (mode);
    std::vector<Vec3> points;
    bool valid = !selection.empty (), path = false, extrusion = false, partialExtrusion = false;
    bool lockXY = false, lockZ = false;
    const bool face = selection.size () == 1 && selection.front ().part == Part::Face;
    for (const auto& ref : selection) {
        valid = valid && Exists (doc, ref);
        if (const Path* p = FindPath (doc, ref.entity)) {
            path = true;
            if (ref.part != Part::Entity && ref.part != Part::PathPoint) {
                valid = false; // ranges and grade points use their own controls, even in mixed selections
                continue;
            }
            for (const auto& c : p->points)
                if (ref.part == Part::Entity || c.id == ref.item) {
                    points.push_back (c.p);
                    lockXY = lockXY || c.lockXY;
                }
            if (ref.part == Part::Entity)
                lockZ = lockZ || std::all_of (p->profile.begin (), p->profile.end (),
                                              [] (const GradeBreak& b) { return b.fixed; });
            else {
                AlignmentGeometry alignment;
                std::string error;
                if (!BuildAlignment (p->points, alignment, error))
                    valid = false;
                else
                    for (size_t i = 0; i < p->points.size (); ++i)
                        if (p->points[i].id == ref.item)
                            for (const auto& b : p->profile)
                                if ((b.anchor == ref.item ||
                                     std::abs (b.station - alignment.pointStations[i]) < 1e-6) &&
                                    (b.fixed || b.curve > 0))
                                    lockZ = true;
            }
            continue;
        }
        const Entity* e = Find (doc, ref.entity);
        if (e == nullptr || !Exists (doc, ref)) {
            valid = false;
            continue;
        }
        std::set<size_t> indices;
        if (ref.part == Part::Entity || (ref.part == Part::Face && ref.item < 2))
            for (size_t i = 0; i < e->vertices.size (); ++i)
                indices.insert (i);
        else if (ref.part == Part::Vertex)
            indices.insert (size_t (VertexIndex (*e, ref.item)));
        else if (ref.part == Part::Segment || ref.part == Part::Face) {
            size_t a = 0, b = 0;
            SegmentEnds (*e, ref.part == Part::Face ? size_t (ref.item - 2) : size_t (SegmentIndex (*e, ref.item)), a,
                         b);
            indices.insert (a);
            indices.insert (b);
        }
        extrusion = extrusion || e->kind == Kind::Extrusion;
        partialExtrusion = partialExtrusion || (e->kind == Kind::Extrusion && indices.size () < e->vertices.size ());
        for (size_t i : indices) {
            points.push_back (e->vertices[i].p);
        }
    }
    double extent[3] {};
    if (!points.empty ()) {
        double lo[3], hi[3];
        for (int a = 0; a < 3; ++a)
            lo[a] = hi[a] = Dot (points.front () - frame.origin, frame.axes[a]);
        for (const auto& p : points)
            for (int a = 0; a < 3; ++a) {
                const double value = Dot (p - frame.origin, frame.axes[a]);
                lo[a] = std::min (lo[a], value), hi[a] = std::max (hi[a], value);
            }
        for (int a = 0; a < 3; ++a)
            extent[a] = hi[a] - lo[a];
    }
    const bool spread = *std::max_element (extent, extent + 3) > kMinSegment;
    const auto translates = [&] (Vec3 direction) {
        const bool xy = std::hypot (direction.x, direction.y) > 1e-9;
        const bool z = std::abs (direction.z) > 1e-9;
        return !(xy && lockXY) && !(z && (lockZ || partialExtrusion));
    };
    for (auto& h : handles) {
        bool supported = valid;
        const int a = h.axis < 0 ? 2 : h.axis;
        const Vec3 axis = frame.axes[a];
        if (face) {
            // Only the supported face-normal push/pull, not a translation of the body's base.
            supported = supported && (h.kind == HandleKind::Face ||
                                      (mode == GizmoMode::Gumball && h.kind == HandleKind::Axis && a == 2));
            if (supported) {
                h.kind = HandleKind::Face;
                supported = translates (axis);
            }
        }
        else
            switch (h.kind) {
                case HandleKind::Centre:
                    supported = supported && (mode == GizmoMode::Move || mode == GizmoMode::Gumball);
                    break;
                case HandleKind::Axis:
                    supported = supported && translates (axis);
                    break;
                case HandleKind::Plane:
                    supported =
                        supported && translates (frame.axes[(a + 1) % 3]) && translates (frame.axes[(a + 2) % 3]);
                    break;
                case HandleKind::Ring:
                    supported = supported && spread && !lockXY && (!lockZ || path) &&
                                extent[(a + 1) % 3] + extent[(a + 2) % 3] > kMinSegment &&
                                (!(path || extrusion) || std::abs (axis.z) > 0.999);
                    break;
                case HandleKind::ScaleAxis:
                    // A path scales in plan only; an extrusion's height is not a vertex extent,
                    // so a factor on its Z axis would move the base alone -- refused however the
                    // box is measured, not merely while its vertices share one height.
                    supported = supported && extent[a] > kMinSegment && !lockXY && (!lockZ || path) &&
                                (!path || std::abs (axis.z) < 1e-9) && !(extrusion && a == 2);
                    break;
                case HandleKind::ScaleUniform:
                    supported = supported && spread && !lockXY && (!lockZ || path);
                    break;
                case HandleKind::Side:
                    supported = supported && !path && extent[a] > kMinSegment && translates (axis);
                    break;
                case HandleKind::Face:
                    supported = false;
                    break;
            }
        h.movable = supported;
        h.focusable = h.focusable && supported;
    }
    return handles;
}

} // namespace geomsrv::edit
