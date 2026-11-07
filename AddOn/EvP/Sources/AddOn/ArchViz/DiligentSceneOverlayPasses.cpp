// The line overlays: the wireframe, the selection's outline and edges, and the
// hover and analysis-role outlines. What each looks like, and why, is
// SelectionHighlightStyle.hpp.
//
// ⚠️ WHY THIS IS ITS OWN TRANSLATION UNIT. DiligentSceneImpl.hpp splits the
// scene by concern, and tools/quality/check_cpp.py holds DiligentSceneDraw.cpp
// at the 1000-line cap with an entry that says the next feature EXTRACTS rather
// than grows. The sun study tint was that feature, and this is the extraction it
// paid for: passes that share one pixel shader, one inverted hull and one rule
// about what may be drawn over what. Draw() calls this exactly where the block
// used to be; the pass ORDER below is the contract.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/SelectionEdges.hpp"

#include <cstring>

namespace geomsrv {
namespace archviz {

void DiligentScene::DrawLineOverlays (Diligent::IDeviceContext* context, DiligentSceneConstants& constants,
                                      CullMode cull, bool drawWireframe, float nearClip)
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

    auto drawWhole = [&] (const Entry& e) {
        BindMesh (context, e);
        const MaterialRange whole { -1, 0, e.indexCount };
        DrawEntryRange (context, e, whole);
        ++impl_->drawCalls;
    };
    auto drawable = [] (const Entry& e) { return e.vertexBuffer != nullptr && e.indexBuffer != nullptr; };

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
            if (!drawable (e))
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

    // Which elements carry a mark this frame. Nothing marked, nothing more to do
    // -- and nothing built: the selection's pipelines wait for the first mark.
    const uint32_t hoverId = impl_->hoverId;
    const bool anySelected = !impl_->selectionGuids.empty ();
    const bool anyObstruction = !impl_->obstructionGuids.empty ();
    if (!anySelected && hoverId == kNoPickId && impl_->focusGuids.empty () && impl_->analysisGuids.empty () &&
        !anyObstruction)
        return;
    auto marked = [&] (const Entry& e) {
        return e.selected || e.analysisRole != 0 || (hoverId != kNoPickId && e.pickId == hoverId);
    };

    // ⚠️ BUILT ON FIRST USE, AND ONCE (DiligentScenePso.cpp). A failure latches
    // and is logged once; the marks then fall back to the outline rings alone,
    // which is the look this replaced minus its tint.
    Impl::SelectionPipelines& sel = impl_->selectionPipelines;
    if (sel.depthPso[0] == nullptr && !sel.initFailed) {
        std::string initError;
        if (!CreateSelectionPipelines (impl_->device, impl_->initColorFormat, impl_->initDepthFormat, initError)) {
            sel = Impl::SelectionPipelines {};
            sel.initFailed = true;
            ArchVizLog ("Diligent scene: selection edges unavailable (" + initError + ") -- outline only");
        }
    }
    const bool selectionReady = !sel.initFailed && sel.depthPso[0] != nullptr;
    const int cullIndex = CullIndex (cull);

    // ---- the depth prime -----------------------------------------------------
    // ⚠️ BEFORE THE EDGES AND THE RINGS, AFTER THE GLOBAL WIREFRAME. A marked
    // GLASS element wrote no depth in the transparent pass, so without this its
    // inverted hull is not a ring but the whole expanded back of the element,
    // painted solid through the glass -- and its back edges could not be told
    // from its front ones. Opaque elements already hold this depth, bit for bit,
    // so only elements with a transparent range are drawn. After the global
    // wireframe, so priming a selected element never hides the lines behind it.
    if (selectionReady) {
        bool bound = false;
        for (const Entry& e : impl_->elements) {
            if (!drawable (e) || !marked (e))
                continue;
            bool transparent = false;
            for (const MaterialRange& r : e.ranges)
                transparent = transparent || impl_->materials.Lookup (r.material).alpha < kOpaqueAlpha;
            if (!transparent)
                continue;
            if (!bound) {
                context->SetPipelineState (sel.depthPso[cullIndex]);
                context->CommitShaderResources (sel.depthSrb[cullIndex],
                                                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                bound = true;
            }
            drawWhole (e);
        }
    }

    // ---- the selection's edges (#118) ------------------------------------------
    // HIDDEN FIRST, FAINT; THEN VISIBLE, FULL STRENGTH. The two passes are the
    // two halves of one wire, split by complementary depth tests, so no fragment
    // is drawn twice. Selected elements only: hover and the analysis roles are
    // outlines and nothing else.
    //
    // ⚠️ THE PULL, THE MASK SHIFT AND THE LINE WIDTH ARE PUT BACK. All three
    // ride the shared constants; the global wireframe reads them on the next
    // frame and must find no pull, its own edges and the HUD's own width.
    if (selectionReady && (anySelected || anyObstruction)) {
        const float lineWidth = constants.wireParams[1];
        constants.edgeParams[0] = sel.semantic ? kSelectionEdgeDepthPull * nearClip : 0.0f;
        // Only the shape-defining edges (ArchViz/SelectionEdges.hpp), not every
        // polygon boundary: a column or a terrain is otherwise all lines.
        constants.edgeParams[1] = float (kSelectionEdgeShift);
        struct EdgePass {
            int index;
            const float* colour;
            float pixels;
        };
        // The selection's cyan, then the obstruction's pink (#167): the same two
        // passes, one group of elements each. A selected blocker stays cyan.
        const EdgePass passes[4] = {
            { Impl::kHiddenEdges, kSelectionHiddenEdgeColor, kSelectionHiddenEdgePixels },
            { Impl::kVisibleEdges, kSelectionEdgeColor, kSelectionEdgePixels },
            { Impl::kHiddenEdges, kObstructionHiddenEdgeColor, kSelectionHiddenEdgePixels },
            { Impl::kVisibleEdges, kObstructionEdgeColor, kSelectionEdgePixels },
        };
        for (int passIndex = 0; passIndex < 4; ++passIndex) {
            const EdgePass& pass = passes[passIndex];
            const bool obstructionPass = passIndex >= 2;
            if (obstructionPass ? !anyObstruction : !anySelected)
                continue;
            constants.wireParams[1] = pass.pixels;
            uploadConstants (pass.colour[0], pass.colour[1], pass.colour[2], pass.colour[3]);
            Diligent::IShaderResourceVariable* edgeVariable =
                sel.semantic
                    ? sel.edgeSrb[pass.index]->GetVariableByName (Diligent::SHADER_TYPE_HULL, "g_wirePatchFlags")
                    : nullptr;
            context->SetPipelineState (sel.edgePso[pass.index]);
            for (const Entry& e : impl_->elements) {
                const bool member = obstructionPass ? (e.analysisRole == 3 && !e.selected) : e.selected;
                if (!member || !drawable (e))
                    continue;
                // ⚠️ A FACE-AWARE PASS CANNOT DRAW AN ELEMENT WITHOUT ITS EDGE
                // FLAGS -- the hull stage would read an unbound buffer. Such an
                // element keeps its outline and goes without edges.
                if (sel.semantic) {
                    if (e.wireEdgeBuffer == nullptr || edgeVariable == nullptr)
                        continue;
                    edgeVariable->Set (e.wireEdgeBuffer->GetDefaultView (Diligent::BUFFER_VIEW_SHADER_RESOURCE));
                }
                context->CommitShaderResources (sel.edgeSrb[pass.index],
                                                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                drawWhole (e);
            }
        }
        constants.wireParams[1] = lineWidth;
        constants.edgeParams[0] = 0.0f;
        constants.edgeParams[1] = 0.0f;
    }

    // ---- the outlines (PLAT-RE41, PLAT-RE136) -----------------------------------
    // ⚠️ LAST, AFTER THE TRANSPARENT PASS. The outline is what says where the
    // element ENDS, so anything drawn over it takes that away -- and a selected
    // element is very often behind glass, which is drawn in the pass above.
    //
    // ⚠️ SKIPPED ENTIRELY UNDER CullMode::None. The hull is exactly the faces the
    // visible pass throws away; with nothing thrown away there is no hull, and
    // the pass would paint a solid expanded copy of the model over the model.
    //
    // The selection, the analysis roles and the hover share one pipeline state
    // because they are the same inverted hull -- a second PSO would be two
    // objects that must agree about depth, cull and blend forever.
    if (cull == CullMode::None || impl_->outlinePso[cullIndex] == nullptr)
        return;
    context->SetPipelineState (impl_->outlinePso[cullIndex]);
    context->CommitShaderResources (impl_->outlineSrb[cullIndex], Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    if (anySelected) {
        setOutlinePixels (kSelectionOutline.pixels);
        uploadConstants (kSelectionOutline.color[0], kSelectionOutline.color[1], kSelectionOutline.color[2],
                         kSelectionOutline.color[3]);
        for (const Entry& e : impl_->elements) {
            if (e.selected && drawable (e))
                drawWhole (e);
        }
    }

    // The visibility roles, at their own weight. A selected element keeps its
    // cyan: the selection is what the user is doing right now.
    for (const uint8_t role : { uint8_t (1), uint8_t (2), uint8_t (3) }) {
        const float* colour = role == 1   ? kFocusOutlineColor
                              : role == 2 ? kAnalysisOutlineColor
                                          : kObstructionOutline.color;
        bool uploaded = false;
        for (const Entry& e : impl_->elements) {
            if (e.analysisRole != role || e.selected || !drawable (e))
                continue;
            if (!uploaded) {
                setOutlinePixels (role == 3 ? kObstructionOutline.pixels : kAnalysisRoleOutlinePixels);
                uploadConstants (colour[0], colour[1], colour[2], colour[3]);
                uploaded = true;
            }
            drawWhole (e);
        }
    }

    // ⚠️ SKIPPED WHEN THE HOVERED ELEMENT IS ALREADY SELECTED. Two hulls on one
    // element paint the amber over the cyan, so hovering a selected element
    // would make it look merely hovered -- the opposite of what both marks are
    // for. Selection is the stronger statement and wins.
    if (hoverId != kNoPickId) {
        for (const Entry& e : impl_->elements) {
            if (e.pickId != hoverId || e.selected || !drawable (e))
                continue;
            setOutlinePixels (kHoverOutline.pixels);
            uploadConstants (kHoverOutline.color[0], kHoverOutline.color[1], kHoverOutline.color[2],
                             kHoverOutline.color[3]);
            drawWhole (e);
            break; // one element carries an id; nothing after it can match
        }
    }
}

} // namespace archviz
} // namespace geomsrv
