// The line overlays: the wireframe, the selection silhouette and the hover
// silhouette.
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
        }
    }

    // ---- the selection silhouette (PLAT-RE41) -------------------------------
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
    const bool anyOutline = !impl_->selectionGuids.empty () || hoverId != kNoPickId;
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

        uploadConstants (kSelectionOutlineColor[0], kSelectionOutlineColor[1], kSelectionOutlineColor[2],
                         kSelectionOutlineColor[3]);
        for (const Entry& e : impl_->elements) {
            if (!e.selected || e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
                continue;
            drawHull (e);
        }

        // ⚠️ SKIPPED WHEN THE HOVERED ELEMENT IS ALREADY SELECTED. Two hulls on
        // one element paint the amber over the cyan, so hovering a selected
        // element would make it look deselected -- the opposite of what both
        // marks are for. Selection is the stronger statement and wins.
        if (hoverId != kNoPickId) {
            for (const Entry& e : impl_->elements) {
                if (e.pickId != hoverId || e.selected || e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
                    continue;
                // ⚠️ THE OFFSET IS RE-UPLOADED, NOT JUST THE COLOUR. The hull's
                // thickness lives in `outlineParams`, which was filled for the
                // SELECTION's 3 px far above; `uploadConstants` only touches
                // `baseColor`, so without this the hover would silently inherit
                // the selection's weight and the constant below would do nothing.
                constants.outlineParams[0] =
                    impl_->viewportWidth > 0 ? kHoverOutlinePixels * 2.0f / float (impl_->viewportWidth) : 0.0f;
                constants.outlineParams[1] =
                    impl_->viewportHeight > 0 ? kHoverOutlinePixels * 2.0f / float (impl_->viewportHeight) : 0.0f;
                uploadConstants (kHoverOutlineColor[0], kHoverOutlineColor[1], kHoverOutlineColor[2],
                                 kHoverOutlineColor[3]);
                drawHull (e);
                break; // one element carries an id; nothing after it can match
            }
        }
    }
}

} // namespace archviz
} // namespace geomsrv
