// The line overlays: the wireframe, the selection's wireframe, halo and core
// silhouette, and the hover silhouette. What each looks like, and why, is
// SelectionHighlightStyle.hpp.
//
// ⚠️ WHY THIS IS ITS OWN TRANSLATION UNIT. DiligentSceneImpl.hpp splits the
// scene by concern, and tools/quality/check_cpp.py holds DiligentSceneDraw.cpp
// at the 1000-line cap with an entry that says the next feature EXTRACTS rather
// than grows. The sun study tint was that feature, and this is the extraction it
// paid for: three passes that share one pixel shader, one inverted hull and one
// rule about what may be drawn over what.
//
// ⚠️ THE CODE IS UNCHANGED FROM WHERE IT SAT AT THE END OF Draw(). Only the
// two values it used to read from that scope -- the frame's constants and its
// cull mode -- are parameters now, and the per-draw colour upload it shared with
// the mesh pass is restated here as a local lambda over the SAME constant
// buffer. Nothing about the pass order moved: Draw() calls this exactly where
// the block used to be.

#include "ArchViz/DiligentSceneImpl.hpp"

#include <cstring>

namespace geomsrv {
namespace archviz {

void DiligentScene::DrawLineOverlays (Diligent::IDeviceContext* context, DiligentSceneConstants& constants,
                                      CullMode cull, bool drawWireframe)
{
    if (impl_ == nullptr || context == nullptr)
        return;

    // The same upload the mesh pass uses, over the same buffer: only the base
    // colour changes between these draws.
    //
    // ⚠️ THE THREE MATERIAL LANES ARE RESET TO THE SAME DEFAULTS Draw()'s OWN
    // HELPER TOOK -- matte, neutral dielectric, non-metal. These passes all run
    // through kArchVizFlatPS, which has no lighting to apply them to, so nothing
    // reads them today; leaving them holding the LAST MATERIAL RANGE's finish
    // would make that silently untrue the day anything here grows a highlight.
    auto uploadConstants = [&] (float r, float g, float b, float a) {
        constants.baseColor[0] = r;
        constants.baseColor[1] = g;
        constants.baseColor[2] = b;
        constants.baseColor[3] = a;
        constants.outlineParams[3] = 1.0f;
        constants.materialParams[0] = 0.5f;
        constants.materialParams[2] = 0.0f;
        UploadConstants (context, impl_->constants, constants);
    };

    // The hull's push, in NDC, for one ring. ⚠️ EVERY RING SETS ITS OWN: the
    // thickness lives in `outlineParams`, which `uploadConstants` does not touch,
    // so a ring that skipped this would silently inherit the previous ring's
    // weight and its own constant would do nothing.
    auto setOutlinePixels = [&] (float pixels) {
        constants.outlineParams[0] = impl_->viewportWidth > 0 ? pixels * 2.0f / float (impl_->viewportWidth) : 0.0f;
        constants.outlineParams[1] = impl_->viewportHeight > 0 ? pixels * 2.0f / float (impl_->viewportHeight) : 0.0f;
    };

    // One element's lines, the face-aware tessellated wire where the device has
    // it and the plain fill-mode wire where it does not.
    auto drawWire = [&] (const Entry& e) {
        BindMesh (context, e);
        if (impl_->semanticWirePso != nullptr && e.wireEdgeBuffer != nullptr) {
            Diligent::IShaderResourceVariable* edgeVariable =
                impl_->semanticWireSrb->GetVariableByName (Diligent::SHADER_TYPE_HULL, "g_wirePatchFlags");
            if (edgeVariable != nullptr)
                edgeVariable->Set (e.wireEdgeBuffer->GetDefaultView (Diligent::BUFFER_VIEW_SHADER_RESOURCE));
            context->SetPipelineState (impl_->semanticWirePso);
            context->CommitShaderResources (impl_->semanticWireSrb,
                                            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        else {
            context->SetPipelineState (impl_->wirePso);
            context->CommitShaderResources (impl_->wireSrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        const MaterialRange whole { -1, 0, e.indexCount };
        DrawEntryRange (context, e, whole);
        ++impl_->drawCalls;
    };

    // ---- the wireframe overlay ---------------------------------------------
    // ⚠️ ELEMENTS ONLY, AND THE WHOLE ELEMENT IN ONE DRAW. Material ranges do not
    // matter to a line colour, and drawing per range would multiply the pass's
    // cost by the material count for an identical picture. The gnomon and the
    // debug cube are excluded for the same reason they are excluded from the id
    // pass: they are not the model, and in the overlay's wireframe they would be
    // the only things NOT matching Archicad's window.
    if (drawWireframe) {
        uploadConstants (kWireframeColor[0], kWireframeColor[1], kWireframeColor[2], kWireframeColor[3]);
        for (const Entry& e : impl_->elements) {
            if (e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
                continue;
            drawWire (e);
        }
    }

    // ---- the selection's wireframe (issue #118) ------------------------------
    // The same lines, on the selected elements only, faint and thin. ⚠️ NOT
    // UNDER THE GLOBAL WIREFRAME: those elements' lines were drawn just above,
    // and a second faint copy over them would change nothing but the cost.
    //
    // ⚠️ THE LINE WIDTH IS PUT BACK. It is the HUD's wireframe setting, read by
    // the next frame's global pass from the same constants.
    const bool anySelected = !impl_->selectionGuids.empty ();
    if (!drawWireframe && anySelected && (impl_->semanticWirePso != nullptr || impl_->wirePso != nullptr)) {
        const float lineWidth = constants.wireParams[1];
        constants.wireParams[1] = kSelectionWirePixels;
        uploadConstants (kSelectionWireColor[0], kSelectionWireColor[1], kSelectionWireColor[2],
                         kSelectionWireColor[3]);
        for (const Entry& e : impl_->elements) {
            if (!e.selected || e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
                continue;
            drawWire (e);
        }
        constants.wireParams[1] = lineWidth;
    }

    // ---- the selection halo and core (PLAT-RE41, restyled by issue #118) -----
    // ⚠️ LAST, AFTER THE TRANSPARENT PASS. The outline is what says where the
    // element ENDS, so anything drawn over it takes that away -- and a selected
    // element is very often behind glass, which is drawn in the pass above.
    //
    // ⚠️ SKIPPED ENTIRELY UNDER CullMode::None. The hull is exactly the faces the
    // visible pass throws away; with nothing thrown away there is no hull, and
    // the pass would paint a solid expanded copy of the model over the model.
    // ---- the hover silhouette (PLAT-RE136) ----------------------------------
    // Same pass, same hull, a different colour and a thinner offset: what a click
    // WOULD take, drawn before the user commits to it. It shares the selection's
    // pipeline state because it is the same inverted hull -- a second PSO would be
    // two objects that must agree about depth, cull and blend forever.
    const uint32_t hoverId = impl_->hoverId;
    const int outlineIndex = CullIndex (cull);
    const bool anyOutline =
        anySelected || hoverId != kNoPickId || !impl_->focusGuids.empty () || !impl_->analysisGuids.empty ();
    if (cull != CullMode::None && impl_->outlinePso[outlineIndex] != nullptr && anyOutline) {
        context->SetPipelineState (impl_->outlinePso[outlineIndex]);
        context->CommitShaderResources (impl_->outlineSrb[outlineIndex],
                                        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        auto drawHull = [&] (const Entry& e) {
            BindMesh (context, e);
            const MaterialRange whole { -1, 0, e.indexCount };
            DrawEntryRange (context, e, whole);
            ++impl_->drawCalls;
        };

        // ⚠️ HALO FIRST, THEN THE CORE OVER IT -- see kSelectionHalo.
        for (const OutlineLayer* ring : { &kSelectionHalo, &kSelectionCore }) {
            if (!anySelected)
                break;
            setOutlinePixels (ring->pixels);
            uploadConstants (ring->color[0], ring->color[1], ring->color[2], ring->color[3]);
            for (const Entry& e : impl_->elements) {
                if (!e.selected || e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
                    continue;
                drawHull (e);
            }
        }

        // The visibility roles, at their own weight. A selected element keeps its
        // cyan: the selection is what the user is doing right now.
        for (const uint8_t role : { uint8_t (1), uint8_t (2) }) {
            const float* colour = role == 1 ? kFocusOutlineColor : kAnalysisOutlineColor;
            bool uploaded = false;
            for (const Entry& e : impl_->elements) {
                if (e.analysisRole != role || e.selected || e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
                    continue;
                if (!uploaded) {
                    setOutlinePixels (kAnalysisRoleOutlinePixels);
                    uploadConstants (colour[0], colour[1], colour[2], colour[3]);
                    uploaded = true;
                }
                drawHull (e);
            }
        }

        // ⚠️ SKIPPED WHEN THE HOVERED ELEMENT IS ALREADY SELECTED. Two hulls on
        // one element paint the amber over the cyan halo, so hovering a selected
        // element would make it look merely hovered -- the opposite of what both
        // marks are for. Selection is the stronger statement and wins.
        if (hoverId != kNoPickId) {
            for (const Entry& e : impl_->elements) {
                if (e.pickId != hoverId || e.selected || e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
                    continue;
                setOutlinePixels (kHoverOutline.pixels);
                uploadConstants (kHoverOutline.color[0], kHoverOutline.color[1], kHoverOutline.color[2],
                                 kHoverOutline.color[3]);
                drawHull (e);
                break; // one element carries an id; nothing after it can match
            }
        }
    }
}

} // namespace archviz
} // namespace geomsrv
