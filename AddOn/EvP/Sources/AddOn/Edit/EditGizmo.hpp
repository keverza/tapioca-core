#ifndef EVP_EDIT_EDITGIZMO_HPP
#define EVP_EDIT_EDITGIZMO_HPP

// The manipulator: one gizmo whose handles are rows in a table (kind, axis, whether
// a value can be typed into it, whether it can be dragged, which handles update
// together), so a new mode is a new table, not new widget code. Drags map pick rays
// onto the handle's line, plane or ring in world metres; a typed value produces the
// same Transform a drag would.
#include "Edit/EditSession.hpp"
#include "Edit/EditTracker.hpp"
#include <vector>

namespace geomsrv::edit {

enum class GizmoMode : uint8_t { Move, Rotate, Scale, Stretch, Extrude };
enum class HandleKind : uint8_t { Centre, Axis, Plane, Ring, ScaleAxis, ScaleUniform, Face };

// Orthonormal axes at the pivot: world, the working plane, or path-local.
struct Frame {
    Vec3 origin;
    Vec3 axes[3] = { { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 } };
};
// Path-local frame: X along the segment in plan, Z up, Y to its left.
bool SegmentFrame (Vec3 a, Vec3 b, Frame& frame);
// Axis 2 along the face's outward normal (the face handle drags along it); origin at its centre.
bool FaceFrame (const Entity& entity, size_t face, Frame& frame);

struct Handle {
    uint32_t index = 0;
    HandleKind kind = HandleKind::Centre;
    int axis = -1;          // frame axis: the line, the plane's normal, the ring's axis
    bool focusable = false; // accepts a typed value
    bool movable = true;
    uint32_t group = 0; // handles that highlight and update together
    Quantity quantity = Quantity::Distance;
};
std::vector<Handle> Handles (GizmoMode mode);

struct Ray {
    Vec3 origin, direction;
};

struct Transform {
    Vec3 translate;
    Vec3 pivot;
    Vec3 rotationAxis { 0.0, 0.0, 1.0 };
    double angle = 0.0; // radians, right-handed about rotationAxis
    Frame scaleFrame;   // axes the per-axis scale is measured along
    Vec3 scale { 1.0, 1.0, 1.0 };
    double extrude = 0.0;  // metres along a face's normal
    bool relocate = false; // a centre drag: move the pivot, not the geometry
};
Vec3 Apply (const Transform& transform, Vec3 p);

struct DragResult {
    bool ok = false;
    Transform transform;
    double value = 0.0; // what the handle's label shows: metres, degrees or a factor
    std::string error;
};
// Refuses edge-on views (a line pointing into the camera, a plane seen side-on) rather
// than turning a tiny screen move into a huge model move.
DragResult Drag (const Frame& frame, const Handle& handle, const Ray& start, const Ray& now);
// The Transform a typed value means on that handle: metres, degrees or a factor.
DragResult FromValue (const Frame& frame, const Handle& handle, double value);

// Applies a Transform to the selected items. Entities move whole; vertices, segments
// and faces move only their own vertices (the stretch); a non-zero `extrude` pushes a
// face along its normal. Locked coordinates and geometry the result would break are
// refused, not warped.
Operation TransformOperation (const std::vector<Ref>& selection, const Transform& transform);

} // namespace geomsrv::edit
#endif
