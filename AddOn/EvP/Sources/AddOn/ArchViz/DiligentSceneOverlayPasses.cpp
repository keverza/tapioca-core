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
#include "ArchViz/TechnicalEdges.hpp"

#include <algorithm>
#include <cstring>

namespace geomsrv {
namespace archviz {

void DiligentScene::DrawLineOverlays (Diligent::IDeviceContext* context, DiligentSceneConstants& constants,
                                      CullMode cull, bool drawWireframe, float nearClip)
{
    if (impl_ == nullptr || context == nullptr)
        return;

    // ---- the section's back faces, caps and section line (#14 P2, DiligentSceneSection.cpp) ---
    // FIRST: the poche is a surface, so the wireframe, the edges and the selection draw over it.
    DrawSectionCaps (context, constants, cull);

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
        impl_->Upload (context, constants);
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
    auto drawable = [] (const Entry& e) { return e.vertexBuffer != nullptr && e.indexBuffer != nullptr && !e.hidden; };

    // ---- the wireframe overlay ---------------------------------------------
    // ⚠️ ELEMENTS ONLY, AND THE WHOLE ELEMENT IN ONE DRAW. Material ranges do not
    // matter to a line colour, and drawing per range would multiply the pass's
    // cost by the material count for an identical picture. The gnomon and the
    // debug cube are excluded for the same reason they are excluded from the id
    // pass: they are not the model, and in the overlay's wireframe they would be
    // the only things NOT matching Archicad's window.
    if (drawWireframe) {
        const float* wire = impl_->wireColor;
        uploadConstants (wire[0], wire[1], wire[2], wire[3]);
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
    const bool anyMark = anySelected || hoverId != kNoPickId || !impl_->focusGuids.empty () ||
                         !impl_->analysisGuids.empty () || anyObstruction;
    const TechnicalEdgeStyle& technical = impl_->technicalEdges;
    if (!anyMark && !technical.enabled)
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

    // ---- the technical edges (issue #14 P2, ArchViz/TechnicalEdges.hpp) -------------------------
    // The selection's two edge passes over EVERY element, at the HUD's angle tier, width and
    // colour: hidden first and faint (only when asked), then visible. Before the depth prime and
    // the selection's edges, so a selected element's own edges draw over these.
    // ⚠️ FACE-AWARE ONLY. Without the semantic pipeline the edge PSO draws every triangle edge --
    // the triangulation this view must never show -- so the fallback draws nothing.
    // ⚠️ PULL, SHIFT AND WIDTH ARE PUT BACK, for the reason the selection's block gives below.
    if (technical.enabled && selectionReady && sel.semantic) {
        const float lineWidth = constants.wireParams[1];
        constants.edgeParams[0] = kSelectionEdgeDepthPull * nearClip;
        constants.edgeParams[1] = float (technical.maskShift);
        constants.edgeParams[2] = nearClip; // #14: kArchVizTechnicalEdgePS's tolerance in metres
        const size_t tier =
            (std::min) ((technical.maskShift - kSelectionEdgeShift) / 3u, uint32_t (kCreaseTierCount - 1));
        for (const int passIndex : { Impl::kHiddenEdges, Impl::kVisibleEdges }) {
            const bool hidden = passIndex == Impl::kHiddenEdges;
            if (hidden && !technical.showHidden)
                continue;
            constants.wireParams[1] = hidden ? technical.hiddenPixels : technical.pixels;
            uploadConstants (technical.rgba[0], technical.rgba[1], technical.rgba[2],
                             technical.rgba[3] * (hidden ? technical.hiddenAlpha : 1.0f));
            Diligent::IShaderResourceVariable* edgeVariable =
                sel.edgeSrb[passIndex]->GetVariableByName (Diligent::SHADER_TYPE_HULL, "g_wirePatchFlags");
            if (edgeVariable == nullptr)
                continue;
            // #14: the visible pass drops the coplanar joints where it can (kArchVizTechnicalEdgePS).
            Diligent::IShaderResourceBinding* srb = sel.edgeSrb[passIndex];
            Diligent::IPipelineState* pso = sel.edgePso[passIndex];
            if (!hidden && sel.technicalPso != nullptr) {
                if (Diligent::ITextureView* depth = CopyTechnicalEdgeDepth (context)) {
                    Diligent::IShaderResourceVariable* technicalEdges =
                        sel.technicalSrb->GetVariableByName (Diligent::SHADER_TYPE_HULL, "g_wirePatchFlags");
                    Diligent::IShaderResourceVariable* depthVariable =
                        sel.technicalSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_edgeDepth");
                    if (technicalEdges != nullptr && depthVariable != nullptr) {
                        depthVariable->Set (depth);
                        edgeVariable = technicalEdges;
                        srb = sel.technicalSrb;
                        pso = sel.technicalPso;
                    }
                }
            }
            context->SetPipelineState (pso);
            const float alpha = technical.rgba[3] * (hidden ? technical.hiddenAlpha : 1.0f);
            float uploadedFade = 1.0f;
            for (const Entry& e : impl_->elements) {
                if (!drawable (e) || e.wireEdgeBuffer == nullptr)
                    continue;
                // #14: edges crowded closer than a few pixels fade out (TechnicalEdges.hpp).
                float fade = 1.0f;
                if (e.hasBounds) {
                    const float centre[3] = { 0.5f * (e.boundsMin[0] + e.boundsMax[0]),
                                              0.5f * (e.boundsMin[1] + e.boundsMax[1]),
                                              0.5f * (e.boundsMin[2] + e.boundsMax[2]) };
                    const float pixelsPerMetre = PixelsPerMetreAt (
                        constants.viewProj, centre, float (impl_->viewportWidth), float (impl_->viewportHeight));
                    const float spacing = TechnicalEdgeSpacingPixels (
                        e.boundsMin, e.boundsMax, e.technicalEdgeLengths.metres[tier], pixelsPerMetre);
                    if (spacing > 0.0f)
                        fade = TechnicalEdgeFade (spacing);
                }
                if (fade < 0.02f)
                    continue;
                if (fade != uploadedFade) {
                    uploadConstants (technical.rgba[0], technical.rgba[1], technical.rgba[2], alpha * fade);
                    uploadedFade = fade;
                }
                edgeVariable->Set (e.wireEdgeBuffer->GetDefaultView (Diligent::BUFFER_VIEW_SHADER_RESOURCE));
                context->CommitShaderResources (srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                drawWhole (e);
            }
        }
        constants.wireParams[1] = lineWidth;
        constants.edgeParams[0] = 0.0f;
        constants.edgeParams[1] = 0.0f;
        constants.edgeParams[2] = 0.0f;
    }
    if (!anyMark)
        return;

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

// #14: the depth the Technical edges test their coplanar joints against. The frame's depth is the
// bound depth-stencil target, which the edge pass also tests, so it is copied into a texture the
// pixel shader can read, and the targets are bound again. Null when it cannot be: the edges then
// draw every joint, as before.
Diligent::ITextureView* DiligentScene::CopyTechnicalEdgeDepth (Diligent::IDeviceContext* context)
{
    Impl::SelectionPipelines& sel = impl_->selectionPipelines;
    Diligent::ITextureView* depthTarget = impl_->overlayDepth;
    if (depthTarget == nullptr || impl_->overlayColor == nullptr || sel.technicalPso == nullptr)
        return nullptr;
    Diligent::ITexture* source = depthTarget->GetTexture ();
    const Diligent::TextureDesc& want = source->GetDesc ();
    if (want.SampleCount != 1)
        return nullptr;
    RefCntAutoPtr<Diligent::ITexture>& copy = sel.technicalDepth;
    if (copy == nullptr || copy->GetDesc ().Width != want.Width || copy->GetDesc ().Height != want.Height ||
        copy->GetDesc ().Format != want.Format) {
        copy.Release ();
        Diligent::TextureDesc desc;
        desc.Name = "ArchViz technical edge depth";
        desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
        desc.Width = want.Width;
        desc.Height = want.Height;
        desc.Format = want.Format;
        desc.MipLevels = 1;
        desc.BindFlags = Diligent::BIND_SHADER_RESOURCE | Diligent::BIND_DEPTH_STENCIL;
        desc.Usage = Diligent::USAGE_DEFAULT;
        impl_->device->CreateTexture (desc, nullptr, &copy);
        if (copy == nullptr) {
            ArchVizLog ("Diligent scene: technical edges keep their coplanar joints (no depth copy)");
            sel.technicalPso.Release ();
            sel.technicalSrb.Release ();
            return nullptr;
        }
    }
    Diligent::CopyTextureAttribs attribs (source, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, copy,
                                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->CopyTexture (attribs);
    Diligent::ITextureView* view = copy->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    // BOUND AGAIN: the copy moved the depth target out of its depth-write state.
    Diligent::ITextureView* color = impl_->overlayColor;
    context->SetRenderTargets (1, &color, depthTarget, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->SetViewports (1, nullptr, 0, 0);
    return view;
}

} // namespace archviz
} // namespace geomsrv
