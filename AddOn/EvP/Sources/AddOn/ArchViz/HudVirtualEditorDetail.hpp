#ifndef EVP_ARCHVIZ_HUDVIRTUALEDITORDETAIL_HPP
#define EVP_ARCHVIZ_HUDVIRTUALEDITORDETAIL_HPP

// What the virtual editor's view host, its drawing and its panel share: colours, the
// view's mapping between model metres and pixels, model-anchored drawing, and the edits
// they can all start (undo, redo, delete). Not an interface: only HudVirtualEditor*.cpp
// include it.
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/OverlayHud.hpp"
#include <imgui.h>
#include <string>

namespace geomsrv::archviz::edithud::detail {

inline const ImU32 kAccent = IM_COL32 (255, 176, 32, 255);
inline const ImU32 kSnap = IM_COL32 (255, 214, 10, 255);
inline const ImU32 kGuide = IM_COL32 (180, 110, 240, 255);
inline const ImU32 kHover = IM_COL32 (255, 255, 255, 255);
inline const ImU32 kInk = IM_COL32 (30, 34, 40, 255);
inline const ImU32 kPaper = IM_COL32 (245, 247, 250, 235);
inline const ImU32 kAxis[3] = { IM_COL32 (229, 72, 77, 255), IM_COL32 (70, 167, 88, 255), IM_COL32 (62, 99, 221, 255) };

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

const edit::Document& Doc (const Session& s);
void History (Session& s, bool undo);
void DeleteSelection (Session& s);

} // namespace geomsrv::archviz::edithud::detail
#endif
