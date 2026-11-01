#ifndef EVP_ARCHVIZ_SELECTIONHIGHLIGHTSTYLE_HPP
#define EVP_ARCHVIZ_SELECTIONHIGHLIGHTSTYLE_HPP

// ArchViz/SelectionHighlightStyle -- how the viewer marks a selected, hovered or analysed element.
//
// ⚠️ THE SELECTION IS AN OUTLINE AND ITS EDGES, NOT A TINT (the user, issue #118). The first
// selection look (PLAT-RE41) was a cyan wash over the whole body, forced translucent, under a hard
// 3 px outline: on a large selection the material, the shading and what stood behind it were all
// painted over in one flat colour. What replaced it is the 3D modeller's convention -- the
// element's surfaces draw exactly as they would unselected, and the selection is its LINES:
//
//   a solid OUTLINE         -- where the element ends, against the background and against an
//                              abutting element of the same material;
//   its visible EDGES       -- the real edges of its faces, not their triangulation, so its shape
//                              reads at a glance;
//   its HIDDEN edges, faint -- the same edges where something is in front of them, the back of the
//                              element included, so the whole of it is shown and not just the part
//                              that faces the camera.
//
// ⚠️ A FAINTER FIRST ATTEMPT WAS REJECTED LIVE. A 4 px halo at 0.18 alpha under a 1.25 px core at
// 0.70, with a 0.30 alpha wireframe, read as barely selected on an opaque white model; on glass the
// hull showed as a solid fill, because glass writes no depth for the hull to be masked by (see
// SelectionPipelines::depthPso). Weight belongs on the lines; the faces stay untouched.
//
// ⚠️ NO SURFACE CHANGE MEANS NO TRANSPARENT PASS. An untinted selected wall keeps its own alpha,
// so it stays in the opaque pass and keeps writing depth; nothing behind it is revealed by
// selecting it, and nothing drawn after it can show through it.
//
// Pure, header-only, so the relationships between the marks are tested offline
// (test_selectionhighlightstyle.cpp). The look itself is checked by eye: see that file's header.

namespace geomsrv {
namespace archviz {

// One inverted-hull ring: how far it is pushed out, in pixels, and what it paints there.
struct OutlineLayer {
    float pixels;
    float color[4];
};

// The selection's outline. The cyan the selection has always had, so nothing that already reads
// "cyan = selected" changes meaning.
constexpr OutlineLayer kSelectionOutline { 2.0f, { 0.05f, 0.85f, 1.0f, 1.0f } };

// The selection's edges, in the outline's hue: full strength where visible, faint where hidden.
// Drawn in every render mode -- under the global wireframe they are what tells the selected
// element's lines from everyone else's.
constexpr float kSelectionEdgeColor[4] = { 0.05f, 0.85f, 1.0f, 1.0f };
constexpr float kSelectionEdgePixels = 1.5f;
constexpr float kSelectionHiddenEdgeColor[4] = { 0.05f, 0.85f, 1.0f, 0.35f };
constexpr float kSelectionHiddenEdgePixels = 1.0f;

// The crease angle above which two faces' shared edge DEFINES the shape (BuildSelectionEdgeMasks).
// Above a 10-sided prism's 36 degrees, so a column, a pipe or a handrail keeps its end circles and
// loses its facets; below an octagon's 45, which reads as eight faces and should. A terrain's
// gentle slopes merge; its ridges and cuts do not.
constexpr float kSelectionCreaseDegrees = 40.0f;

// How far an edge is pulled toward the camera before the visible/hidden test, as a fraction of
// its own distance (DiligentSceneConstants::edgeParams). Large enough that an edge lying ON a
// visible face never z-fights it into the hidden pass; small enough that an edge genuinely behind
// a face -- a few centimetres at a room's distance -- is still hidden.
constexpr float kSelectionEdgeDepthPull = 0.002f;

// The HOVER silhouette (PLAT-RE136): what a click would take, shown before it is taken.
//
// ⚠️ A DIFFERENT HUE FROM THE SELECTION, NOT A DIMMER ONE. The two are on screen at the same time
// and mean different things -- "this IS selected" against "this WOULD BE" -- and distinguishing
// them by brightness alone fails against a pale facade, which is most of a building. Amber reads
// as distinct from the cyan at any exposure and does not collide with the wireframe either.
//
// ⚠️ NEVER DRAWN ON A SELECTED ELEMENT. Amber over the cyan outline would make a selected element
// look merely hovered; selection is the stronger statement and wins.
constexpr OutlineLayer kHoverOutline { 2.0f, { 1.0f, 0.62f, 0.10f, 1.0f } };

// The visibility ROLE silhouettes (DiligentScene::SetAnalysisHighlight): which element is Focus
// and which is Analysis, so the gradient can be read against what it was measured from. Hues apart
// from the selection's cyan, the hover's amber and the gradient's own red and green.
//
// ⚠️ THEIR OWN WEIGHT, NOT THE SELECTION'S. They used to share the selection's 3 px; the
// selection's moved to 2 px (issue #118) and the roles did not ask to, so they keep the old weight.
constexpr float kAnalysisRoleOutlinePixels = 3.0f;
constexpr float kFocusOutlineColor[4] = { 0.95f, 0.25f, 0.75f, 1.0f };   // magenta
constexpr float kAnalysisOutlineColor[4] = { 0.62f, 0.42f, 1.0f, 1.0f }; // violet

// The global wireframe's line colour. Alpha below 1 so overlapping edges on a dense mesh build up
// instead of saturating into a solid mass.
constexpr float kWireframeColor[4] = { 0.10f, 0.85f, 1.0f, 0.75f };

} // namespace archviz
} // namespace geomsrv

#endif
