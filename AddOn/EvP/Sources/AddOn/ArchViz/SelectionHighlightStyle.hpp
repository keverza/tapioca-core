#ifndef EVP_ARCHVIZ_SELECTIONHIGHLIGHTSTYLE_HPP
#define EVP_ARCHVIZ_SELECTIONHIGHLIGHTSTYLE_HPP

// ArchViz/SelectionHighlightStyle -- how the viewer marks a selected, hovered or analysed element.
//
// ⚠️ THE SELECTION IS A HALO AND A WIREFRAME, NOT A TINT (the user, issue #118). The first
// selection look (PLAT-RE41) was a cyan wash over the whole body, forced translucent, under a hard
// 3 px outline. It said "this one" loudly, and on a large selection it said nothing else: the
// material, the shading and what stood behind it were all painted over in one flat colour. What
// replaced it marks the element at its EDGES and leaves its surfaces exactly as they would draw
// unselected:
//
//   a wide, faint HALO       -- the selection reads from a distance and against any background;
//   a thin CORE silhouette   -- where the element ends, against an abutting one of the same
//                               material;
//   a faint WIREFRAME        -- which faces it has, the part a silhouette cannot show.
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

// The selection's halo and core. ⚠️ HALO FIRST, CORE OVER IT. Both are the same hull pushed out by
// different amounts, so the core's ring lies inside the halo's and is blended over it: the ring
// grades from the core's alpha at the silhouette to the halo's at its outer edge, with no third
// pass and no blur. One hue, the cyan the selection has always had, so nothing that already reads
// "cyan = selected" changes meaning.
constexpr OutlineLayer kSelectionHalo { 4.0f, { 0.05f, 0.85f, 1.0f, 0.18f } };
constexpr OutlineLayer kSelectionCore { 1.25f, { 0.05f, 0.85f, 1.0f, 0.70f } };

// The selection's own wireframe, drawn on its faces in the same pass as the global wireframe.
// ⚠️ FAINT AND THIN: on a dense element a full-strength mesh is a cyan wash again, which is the
// thing this replaced. Skipped while the global wireframe is on -- the element's lines are
// already drawn, and the halo is what tells it apart there.
constexpr float kSelectionWireColor[4] = { 0.10f, 0.85f, 1.0f, 0.30f };
constexpr float kSelectionWirePixels = 1.0f;

// The HOVER silhouette (PLAT-RE136): what a click would take, shown before it is taken.
//
// ⚠️ A DIFFERENT HUE FROM THE SELECTION, NOT A DIMMER ONE. The two are on screen at the same time
// and mean different things -- "this IS selected" against "this WOULD BE" -- and distinguishing
// them by brightness alone fails against a pale facade, which is most of a building. Amber reads
// as distinct from the cyan at any exposure and does not collide with the wireframe either.
//
// ⚠️ NEVER DRAWN ON A SELECTED ELEMENT. Amber over the cyan halo would make a selected element
// look merely hovered; selection is the stronger statement and wins.
constexpr OutlineLayer kHoverOutline { 2.0f, { 1.0f, 0.62f, 0.10f, 1.0f } };

// The visibility ROLE silhouettes (DiligentScene::SetAnalysisHighlight): which element is Focus
// and which is Analysis, so the gradient can be read against what it was measured from. Hues apart
// from the selection's cyan, the hover's amber and the gradient's own red and green.
//
// ⚠️ THEIR OWN WEIGHT, NOT THE SELECTION'S. They used to share the selection's 3 px; the
// selection got lighter (issue #118) and the roles did not ask to, so they keep the old weight.
constexpr float kAnalysisRoleOutlinePixels = 3.0f;
constexpr float kFocusOutlineColor[4] = { 0.95f, 0.25f, 0.75f, 1.0f };   // magenta
constexpr float kAnalysisOutlineColor[4] = { 0.62f, 0.42f, 1.0f, 1.0f }; // violet

// The global wireframe's line colour. Alpha below 1 so overlapping edges on a dense mesh build up
// instead of saturating into a solid mass.
constexpr float kWireframeColor[4] = { 0.10f, 0.85f, 1.0f, 0.75f };

} // namespace archviz
} // namespace geomsrv

#endif
