#ifndef EVP_EDIT_EDITGIZMO_HPP
#define EVP_EDIT_EDITGIZMO_HPP

// The manipulator: one gizmo whose handles are rows in a table (kind, axis, whether
// a value can be typed into it, whether it can be dragged, which handles update
// together), so a new mode is a new table, not new widget code. Drags map pick rays
// onto the handle's line, plane or ring in world metres; a typed value produces the
// same Transform a drag would.
#include "Edit/EditSession.hpp"
#include "Edit/EditSelection.hpp"
#include "Edit/EditTracker.hpp"
#include <vector>

namespace geomsrv::edit {

// Gumball: every transform at once, one handle set -- arrows to move, arcs to turn, boxes on
// the axes to scale, squares to move in a plane, grips on the selection's box to stretch. The
// single modes remain for a gizmo that shows one transform alone.
enum class GizmoMode : uint8_t { Move, Rotate, Scale, Stretch, Extrude, Gumball };
enum class HandleKind : uint8_t { Centre, Axis, Plane, Ring, ScaleAxis, ScaleUniform, Face, Side };

// Orthonormal axes at the pivot: world, the working plane, or path-local.
struct Frame {
    Vec3 origin;
    Vec3 axes[3] = { { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 } };
    double half[3] = { 0.0, 0.0, 0.0 }; // the selection's half-size along each axis (FitBox)
};
// Path-local frame: X along the segment in plan, Z up, Y to its left.
bool SegmentFrame (Vec3 a, Vec3 b, Frame& frame);
// Axis 2 along the face's outward normal (the face handle drags along it); origin at its centre.
bool FaceFrame (const Entity& entity, size_t face, Frame& frame);
// The axes of what is selected, at `origin`, so the gumball turns with the shape: a
// segment's own direction; for an entity or parts of one, its longest edge in plan (the
// first of equal ones, so a turned square keeps its axes). World axes for paths and for a
// selection spanning several entities.
Frame SelectionFrame (const Document& doc, const std::vector<Ref>& items, Vec3 origin);
// The selection's box along `frame`'s axes: the origin moves to its centre and `half` holds
// its half-sizes. False when nothing selected has a vertex.
bool FitBox (const Document& doc, const std::vector<Ref>& items, Frame& frame);

struct Handle {
    uint32_t index = 0;
    HandleKind kind = HandleKind::Centre;
    int axis = -1;          // frame axis: the line, the plane's normal, the ring's axis
    bool focusable = false; // accepts a typed value
    bool movable = true;
    uint32_t group = 0; // handles that highlight and update together
    Quantity quantity = Quantity::Distance;
    int side = 0; // a Side grip: +1 or -1 along its axis
    // Laid out to share the gumball with every other kind: an arrow's head end alone, a scale
    // box partway out with no stem, an arc between the axes, a side grip clear of the rest.
    bool compact = false;
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
    // A stretch: the selected vertices on the `side` of the pivot along scaleFrame's axis
    // `stretchAxis` move `stretchBy` metres along it; the rest stay. A selection with no
    // extent on that axis moves whole (a segment pushed sideways). Not part of Apply.
    int stretchAxis = -1, stretchSide = 0;
    double stretchBy = 0.0;
};
Vec3 Apply (const Transform& transform, Vec3 p);
// The frame a transform carries along: its origin moved, its axes turned.
Frame Moved (const Frame& frame, const Transform& transform);

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

// Each handle drawn as world-space polylines, `size` metres across (the host picks
// the size from its metres-per-pixel so the gizmo keeps one screen size). A centre is
// one point; a ring is a closed circle.
struct HandleShape {
    std::vector<Vec3> points;
    bool closed = false;
};
std::vector<HandleShape> HandleGeometry (const Frame& frame, const Handle& handle, double size);
// A handle shape as it projects, in pixels: false when it is seen edge-on -- a line into
// the view, a plane or a ring side-on. Such a shape is neither drawn nor picked.
bool FacesTheView (const std::vector<std::pair<double, double>>& screen, bool closed);
// The handle nearest the cursor within `radius` pixels, or -1.
int PickHandle (const Frame& frame, const std::vector<Handle>& handles, double size, const PickView& view, double x,
                double y, double radius);

// Applies a Transform to the selected items. Entities move whole; vertices, segments
// and faces move only their own vertices (the stretch); a non-zero `extrude` pushes a
// face along its normal. Locked coordinates and geometry the result would break are
// refused, not warped.
Operation TransformOperation (const std::vector<Ref>& selection, const Transform& transform);

} // namespace geomsrv::edit
#endif
