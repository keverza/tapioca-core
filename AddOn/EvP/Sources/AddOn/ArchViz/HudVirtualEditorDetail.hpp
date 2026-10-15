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
    // A pick ray through the pixel, towards the scene.
    bool RayAt (float px, float py, double z, edit::Vec3 seed, edit::Ray& ray) const;
    double MetresPerPixel (float px, float py, double z, edit::Vec3 seed) const;
    edit::PickView Pick () const;
};

// Drawing anchored to model points, so the Present reprojects it with the view.
roofcanvas::Anchor At (edit::Vec3 p, ImVec2 s);
void Line (Session& s, const Map& m, edit::Vec3 a, edit::Vec3 b, ImU32 colour, float width);
void Dot (Session& s, const Map& m, edit::Vec3 p, float radius, ImU32 colour, bool filled = true);
void Note (Session& s, const Map& m, edit::Vec3 p, ImVec2 offset, const std::string& text, ImU32 colour);

// Drafting aids (HudVirtualEditorSnap.cpp): the session's snap settings and references
// into a query; an edge a snap landed on kept as a reference; the point a drag grabs an
// item by (its own vertex under the pointer, else where it was pressed); and a moved
// item's edges snapping flush to parallel ones, correcting `t` -- false when none did.
void PrepareSnap (const Session& s, edit::SnapQuery& q);
void Acquire (Session& s, const edit::SnapResult& snap);
edit::Vec3 GrabPoint (const Session& s, const Map& m, ImVec2 mouse, const edit::Ref& pressed, edit::Vec3 onPlane,
                      float scale);
bool FlushEdges (Session& s, const Map& m, ImVec2 mouse, const std::vector<edit::Ref>& moving, double z,
                 const std::vector<edit::Ref>& exclude, float scale, edit::Transform& t);

// Live-corner grips (HudVirtualEditorCorners.cpp): hover, drag, click and double click,
// and their drawing. True when they took the pointer this frame.
bool CornerGrips (Session& s, const Map& m, ImVec2 mouse, bool hovered, float scale);

// The live dimensions of the selection, placed clear of `taken` (the gizmo, the tracker).
void DrawDimensions (Session& s, const Map& m, std::vector<editlayers::Box>& taken, float scale);
// The gumball for `mode` at `frame`, `size` metres across; `active` is the handle hovered
// or, while `dragging`, held -- then it alone is drawn, with a guide along its axis.
void DrawGizmo (Session& s, const Map& m, const edit::Frame& frame, edit::GizmoMode mode, double size, int active,
                bool dragging, float scale);

const edit::Document& Doc (const Session& s);
// Ends the line a drawing tool is placing: a polyline or a path, selected.
void FinishDrawing (Session& s);
void History (Session& s, bool undo);
void DeleteSelection (Session& s);

} // namespace geomsrv::archviz::edithud::detail
#endif
