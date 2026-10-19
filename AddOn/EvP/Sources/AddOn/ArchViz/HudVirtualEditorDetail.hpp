#ifndef EVP_ARCHVIZ_HUDVIRTUALEDITORDETAIL_HPP
#define EVP_ARCHVIZ_HUDVIRTUALEDITORDETAIL_HPP

// What the virtual editor's view host, its drawing and its panel share: colours, the
// view's mapping between model metres and pixels, model-anchored drawing, and the edits
// they can all start (undo, redo, delete). Not an interface: only HudVirtualEditor*.cpp
// include it.
#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/OverlayHud.hpp"
#include <imgui.h>
#include <string>

namespace geomsrv::archviz::edithud::detail {

// The layer's palette (EditLayers.hpp Style): nothing white on Archicad's white plan.
inline const ImU32 kAccent = IM_COL32 (245, 158, 11, 255);
inline const ImU32 kSnap = IM_COL32 (255, 122, 0, 255);
inline const ImU32 kGuide = IM_COL32 (155, 93, 229, 255);
inline const ImU32 kHover = IM_COL32 (14, 98, 201, 255);
inline const ImU32 kInk = IM_COL32 (30, 34, 40, 255);
inline const ImU32 kPaper = IM_COL32 (247, 248, 250, 235);
inline const ImU32 kRule = IM_COL32 (91, 101, 115, 255); // dimension lines
inline const ImU32 kShadow = IM_COL32 (20, 24, 30, 70);  // under strokes, so they read on any ground
inline const ImU32 kAxis[3] = { IM_COL32 (224, 64, 79, 255), IM_COL32 (94, 165, 36, 255),
                                IM_COL32 (47, 111, 228, 255) };

// Model metres to and from this view's pixels: the plan's transform or the 3D projection
// the input layer handed over. Nothing else is known about the camera, and nothing is guessed.
struct Map {
    const overlayhud::Input& v;

    bool Screen (edit::Vec3 p, ImVec2& out) const;
    // The point at height `z` that this pixel shows. The plan inverts its affine
    // transform; 3D refines a seed by Newton steps through the projection and refuses a
    // plane seen edge-on.
    bool Onto (float px, float py, double z, edit::Vec3 seed, edit::Vec3& out) const;
    // Checked arbitrary-plane inverse and local pixel scale. Failure leaves outputs intact;
    // a representable mapping is not permission to author unsupported geometry on that plane.
    bool Onto (float px, float py, const edit::WorkingPlane& plane, edit::Vec3 seed, edit::Vec3& out) const;
    bool MetresPerPixel (float px, float py, const edit::WorkingPlane& plane, edit::Vec3 seed, double& out) const;
    // A pick ray through the pixel, towards the scene.
    bool RayAt (float px, float py, double z, edit::Vec3 seed, edit::Ray& ray) const;
    double MetresPerPixel (float px, float py, double z, edit::Vec3 seed) const;
    edit::PickView Pick () const;
};

// Drawing anchored to model points, so the Present reprojects it with the view.
roofcanvas::Anchor At (edit::Vec3 p, ImVec2 s);
void Line (Session& s, const Map& m, edit::Vec3 a, edit::Vec3 b, ImU32 colour, float width);
void Dot (Session& s, const Map& m, edit::Vec3 p, float radius, ImU32 colour, bool filled = true);
void DashedLine (Session& s, const Map& m, edit::Vec3 a, edit::Vec3 b, ImU32 colour, float width);
void Note (Session& s, const Map& m, edit::Vec3 p, ImVec2 offset, const std::string& text, ImU32 colour);

// Drafting aids (HudVirtualEditorSnap.cpp): the session's snap settings and references
// into a query; an edge a snap landed on kept as a reference; the exact point a drag was
// pressed (its captured base point); and a moved item's edges snapping flush to parallel
// ones, correcting `t` -- false when none did.
void PrepareSnap (const Session& s, edit::SnapQuery& q);
edit::Vec3 SnappedTranslation (const Session& s, edit::Vec3 at);
// Fill the kernel's optional view screening from this view's own mapping
// (HudVirtualEditorSnap.cpp): a candidate must be near the pointer in this view's pixels, on
// screen, in front of the camera and within the session's 3D proximity cap. The flush's reach is
// wider than the pointer's snap radius, or a target the pointer would already have snapped to
// could never be reached. The kernel acquires no camera; the mapping and viewport are the ones the
// input layer handed over. Absent, the query keeps the plane metric alone.
void ScreenPointer (const Session& s, const Map& m, ImVec2 mouse, float scale, edit::SnapQuery& q);
void ScreenFlush (const Session& s, const Map& m, ImVec2 mouse, float scale, edit::SnapQuery& q);
void SnapControls (Session& s);
void Acquire (Session& s, const edit::SnapResult& snap);
edit::Vec3 GrabPoint (const Session& s, const Map& m, ImVec2 mouse, const edit::Ref& pressed, edit::Vec3 onPlane,
                      float scale);
bool FlushEdges (Session& s, const Map& m, ImVec2 mouse, const std::vector<edit::Ref>& moving, double z,
                 const std::vector<edit::Ref>& exclude, float scale, edit::Transform& t);

// Curves (HudVirtualEditorCurves.cpp): the handles of the selected anchors, dragged, and
// curved segments bent -- true when they took the pointer; the pen's handle-pulling drag,
// its preview through `hover`, and its handles put on the polyline it made.
bool CurveHandles (Session& s, const Map& m, ImVec2 mouse, bool hovered, const edit::PickView& pick, float scale);
void PenDrag (Session& s, const Map& m, ImVec2 mouse);
void PenPreview (Session& s, const Map& m, edit::Vec3 hover, float scale);
void ApplyPenHandles (edit::Document& d, edit::Id created, const std::vector<edit::Vec3>& handles);

// Live-corner grips (HudVirtualEditorCorners.cpp): hover, drag, click and double click,
// and their drawing. True when they took the pointer this frame.
bool CornerGrips (Session& s, const Map& m, ImVec2 mouse, bool hovered, float scale);
// Kerb grips, the same on every corner of a paved area (paths joined by junctions) with
// anything of it selected: dragging one sets that corner's radius alone, never another's.
bool KerbGrips (Session& s, const Map& m, ImVec2 mouse, bool hovered, float scale);
// Labels opened for typing (HudVirtualEditorLabels.cpp). A click on a placed label opens it,
// its value selected; a click elsewhere sets what was typed: true when the click was theirs.
bool LabelClick (Session& s, ImVec2 mouse, bool hovered);
// The keys while a label is open -- the number's characters, Backspace, Enter, Esc: true
// when one is open, and they were all its.
bool LabelKeys (Session& s, const overlayhud::Input& v);
// Whether two labels set the same value.
bool SameLabel (const editlayers::MarkLabel& a, const editlayers::MarkLabel& b);
bool LabelContains (const editlayers::Box& box, ImVec2 mouse);

// The kerb corner a click picked, as it is now; null when none is.
const edit::PavedCorner* PickedKerb (Session& s);
// The picked kerb corner's radius, for the tracker to offer for typing; false when none is.
bool KerbField (const Session& s, double& radius);
// A typed requested radius for the picked kerb corner; paving fits it without losing intent.
// False when no corner is picked (the value is someone else's).
bool TypeKerb (Session& s, double radius);
// A captured bend/corner or kerb grip's exact radius, with its original target scope.
// Empty outside an active radius drag; unsupported values are refused by the operation.
edit::Operation RadiusDragOperation (const Session& s, double radius);
inline bool DragPointerMoves (const Session& s, ImVec2 mouse)
{
    return !s.navigation.blocked && s.tracker.State () != edit::TrackerState::Editing && !s.dragReleased &&
           (!s.dragPointerPause || mouse.x != s.dragPointerPause->first || mouse.y != s.dragPointerPause->second);
}
// Same-view navigation pauses operation input, never the immutable session baseline.
void Navigate (Session& s, const Map& m, ImVec2 mouse);
void CancelViewEdit (Session& s, const char* reason);
void CheckPlane (Session& s);
void CapturePlane (Session& s);
void PlaneControls (Session& s);
bool DragOnto (const Session& s, const Map& m, ImVec2 mouse, double z, edit::Vec3 seed, edit::Vec3& out);

// The live dimensions of the selection, placed clear of `taken` (the gizmo, the tracker).
void DrawDimensions (Session& s, const Map& m, std::vector<editlayers::Box>& taken, float scale);
// The paths' labels (heights, grades, crossfalls) after them: each beside its mark, clear of
// everything placed so far -- a label with no free place is left out, never drawn over another.
// What the pointer is over, lit as it is: a path's centreline or a range's edges along
// their length, an object's outline, a segment along its curve; a point -- a path's control
// point or grade break, a vertex -- only when the pointer is at it, with its path's line.
void DrawHover (Session& s, const Map& m, float scale);
void DrawPathLabels (Session& s, const Map& m, std::vector<editlayers::Box>& taken, float scale);
// The paths' contour lines as they are now (traced again when the document or the interval
// asked changes; none when hidden), and their heights written along them, small and grey,
// where a label fits beside its line clear of the others.
const std::vector<edit::Contour>& Contours (Session& s);
const edit::PavedArea& PavedSurface (Session& s);
double SurfaceStep (const Session& s);
void DrawContourLabels (Session& s, const Map& m, std::vector<editlayers::Box>& taken, float scale);
// The gumball for `mode` at `frame`, `size` metres across; `active` is the handle hovered
// or, while `dragging`, held -- then it alone is drawn, with a guide along its axis.
void DrawGizmo (Session& s, const Map& m, const edit::Frame& frame, edit::GizmoMode mode, double size, int active,
                bool dragging, float scale);
// The tracker's active numeric field, beside the pointer (or a clicked handle).
void DrawTracker (Session& s, ImVec2 mouse, bool hovered, float scale);

const edit::Document& Doc (const Session& s);
// Ends the line a drawing tool is placing: a polyline or a path, selected.
void FinishDrawing (Session& s);
void History (Session& s, bool undo);
void DeleteSelection (Session& s);
// After an action: the selection keeps what still exists; when nothing does (a polyline
// that became a path), what the action made -- ids from `since` on -- is selected whole.
void SelectMade (Session& s, edit::Id since);

} // namespace geomsrv::archviz::edithud::detail
#endif
