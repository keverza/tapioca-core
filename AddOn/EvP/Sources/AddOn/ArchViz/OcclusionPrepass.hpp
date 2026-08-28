#ifndef EVP_ARCHVIZ_OCCLUSIONPREPASS_HPP
#define EVP_ARCHVIZ_OCCLUSIONPREPASS_HPP

// ArchViz/OcclusionPrepass — WHEN the overlay lays the building's depth down,
// and nothing about HOW. PLAT-RE151.
//
// ⚠️ THE PROBLEM THIS ANSWERS. The overlay composites a separate scene over
// Archicad's 3D window, and Archicad's DEPTH BUFFER is not reachable — not even
// from the Present hook, which owns the swap chain and not the depth-stencil
// view Archicad drew with. So preview content (a Grasshopper sphere, a GDL
// result held up against the model) draws over the building whatever its actual
// position, and a sphere INSIDE a room looks like a sphere stuck to the glass.
//
// The depth does not have to come from Archicad. The extraction worker already
// holds Archicad's model, so rendering THAT, depth-only, from the synced camera
// reconstructs the occluding surface and the preview's own depth test does the
// rest.
//
// ⚠️ SO IT OCCLUDES AGAINST ARCHICAD'S *MODEL*, NOT AGAINST ARCHICAD'S *PIXELS*,
// and that limit is real rather than cosmetic: the occlusion is only as good as
// the extraction and the camera match, and any camera lag puts the occlusion
// edge wrong by exactly the lag everything else already has
// (docs/architecture/diligent/HANDOFF-CameraSync.md).
//
// ⚠️ THIS HEADER IS PURE, AND DELIBERATELY. The rule below is the whole of the
// decision and every one of its cases is a picture someone can be shown, so it
// is tested offline (tests/cpp/test_occlusionprepass.cpp) rather than being
// discovered in Archicad. The DRAW is DiligentSceneOcclusion.cpp's, and it
// includes nothing of Diligent, nothing of the DevKit and nothing of the HUD.

#include <cstddef>

#include "ArchViz/ViewerSettings.hpp" // SceneRenderMode -- the mode IS the question

namespace geomsrv {
namespace archviz {

// Everything the rule needs, named so a caller cannot pass them in the wrong
// order. See `OcclusionPrepassWanted` for what each one decides.
struct OcclusionPrepassInputs {
    // The user's switch. Off is a supported picture: preview over everything is
    // what the overlay did before PLAT-RE151, and it is the right answer when
    // the extraction is stale or the camera is being dragged.
    bool enabled = true;
    // Whether the frame drew the model at all. False over the PLAN (the
    // orthographic path) and false while the overlay is blanked.
    bool modelIsDrawn = false;
    SceneRenderMode renderMode = SceneRenderMode::Shaded;
    // Extracted elements the scene is holding. Zero before the first extraction
    // lands.
    size_t elementCount = 0;
};

// True when the prepass must run before the preview draws.
//
// ⚠️ IT IS A *SUFFICIENCY* TEST, NOT A PREFERENCE. The question is not "should
// the preview be occluded" — it always should — but "is the building's depth
// already in the buffer". Four ways it is not, and one way it is:
//
//   • `enabled` false — the user turned it off; nothing to do.
//   • `elementCount` 0 — nothing extracted yet. A prepass over no geometry
//     leaves depth cleared, so it would be a no-op that still costs a PSO bind.
//   • `modelIsDrawn` false — and this is the one worth stating. Over the PLAN
//     the camera is ORTHOGRAPHIC and aimed at a 2D drawing; occluding a preview
//     against a 3D building there hides it behind storeys the plan does not
//     show. Blanked frames are the same answer for a different reason: nothing
//     drawn means nothing to be behind.
//   • Shaded or ShadedWireframe — the OPAQUE pass ALREADY wrote the model into
//     the same depth buffer the preview tests, so the occlusion is a property
//     the frame has for free and the prepass is duplicated work.
//
//   • Wireframe — the ONE mode that needs it, and the mode the overlay lives
//     in. `Wireframe` draws no surfaces, so depth holds the edge PIXELS and
//     nothing between them; the preview then passes the test everywhere the
//     interior of a wall is, which is the reported bug. And it is precisely the
//     mode where the wireframe is a DEVELOPER affordance that release does not
//     show — the building is invisible in the overlay and must still occlude.
bool OcclusionPrepassWanted (const OcclusionPrepassInputs& inputs);

} // namespace archviz
} // namespace geomsrv

#endif // EVP_ARCHVIZ_OCCLUSIONPREPASS_HPP
