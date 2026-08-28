// PLAT-RE151 — the depth-only prepass that lets overlay content go BEHIND
// Archicad's building.
//
// ⚠️ WHY THIS IS A FILE AND NOT THREE MORE FUNCTIONS IN DiligentSceneDraw.cpp.
// The scene is deliberately spread over translation units of one concern each
// (DiligentSceneImpl.hpp says why), and tools/quality/check_cpp.py freezes the
// sizes of the two that would otherwise have taken this: DiligentScene.cpp at
// 1185 for the PSO creation, and DiligentSceneDraw.cpp sits 4 lines under the
// 1000-line soft cap. Both entries say the next feature EXTRACTS rather than
// grows, and this is that extraction: one pass, its one pipeline state, and the
// lazy build that ties them together.
//
// ⚠️ THE RULE FOR *WHEN* THIS RUNS IS NOT HERE. It is OcclusionPrepass.cpp,
// which is pure and tested offline, because every one of its cases is a picture
// a user can be shown and none of them needs a GPU to be wrong about.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/OcclusionPrepass.hpp"

#include <cstring>
#include <string>

namespace geomsrv {
namespace archviz {

bool DiligentScene::CreateOcclusionDepthPipeline (Diligent::IRenderDevice* device, uint32_t colorBufferFormat,
                                                  uint32_t depthBufferFormat, std::string& error)
{
    if (device == nullptr || impl_ == nullptr || impl_->vs == nullptr || impl_->flatPs == nullptr) {
        error = "the occlusion prepass needs the scene's mesh VS and flat PS, which Init builds first";
        return false;
    }

    // ⚠️ THE SAME THREE ELEMENTS AS EVERY OTHER MESH PIPELINE, and the same
    // contract with ArchVizVertex. A mismatch here does not fail: it reads the
    // wrong bytes and occludes confidently wrong, which looks like a camera bug.
    const Diligent::LayoutElement layout[] = {
        Diligent::LayoutElement { 0, 0, 3, Diligent::VT_FLOAT32, Diligent::False }, // position
        Diligent::LayoutElement { 1, 0, 3, Diligent::VT_FLOAT32, Diligent::False }, // normal
        Diligent::LayoutElement { 2, 0, 4, Diligent::VT_UINT8, Diligent::True },    // colour
    };

    Diligent::GraphicsPipelineStateCreateInfo pci;
    pci.PSODesc.Name = "ArchViz occlusion depth prepass PSO";
    Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;

    // ⚠️ ONE RENDER TARGET, MASKED OFF -- *NOT* `NumRenderTargets = 0`. The
    // shadow map gets to declare zero because BeginCascade binds a DSV and no
    // RTV at all; this pass runs inside the frame loop's binding, with the
    // overlay's colour view still attached, and a PSO that declares no targets
    // against a context that has one is a Diligent validation failure inside
    // Archicad's process. Declaring the target and writing NONE of its channels
    // is the same GPU work with none of that risk, and it means the pass needs
    // no SetRenderTargets of its own -- so it cannot be the next thing that
    // leaves a later pass drawing into nothing (see DiligentSceneDraw.cpp's note
    // on the 2026-08-21 binding fault).
    gp.NumRenderTargets = 1;
    gp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
    gp.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (depthBufferFormat);
    gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // ⚠️ NO CULLING, AND IT IS NOT THE VISIBLE PASS'S SETTING. An occluder wants
    // the NEAREST surface, and CULL_MODE_NONE gives exactly that: on closed
    // geometry the front face wins the depth test anyway, so the result is
    // identical to culling; on the geometry Archicad's extraction leaves open or
    // inconsistently wound it is the difference between a solid wall and a wall
    // with holes the preview shines through. A hole in an occluder does not read
    // as a cull setting -- it reads as the occlusion being broken.
    gp.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
    gp.RasterizerDesc.FillMode = Diligent::FILL_MODE_SOLID;
    gp.RasterizerDesc.FrontCounterClockwise = Diligent::False;

    gp.DepthStencilDesc.DepthEnable = Diligent::True;
    gp.DepthStencilDesc.DepthWriteEnable = Diligent::True;

    // ⚠️ THE COLOUR MASK IS THE WHOLE POINT. The building must not APPEAR in the
    // overlay -- release shows heatmaps and preview geometry over Archicad's own
    // rendering, and painting a second copy of the model over the first is the
    // opposite of what the overlay is for. This pass contributes depth and no
    // pixels; blending is off because there is nothing to blend.
    Diligent::RenderTargetBlendDesc& rt = gp.BlendDesc.RenderTargets[0];
    rt.BlendEnable = Diligent::False;
    rt.RenderTargetWriteMask = Diligent::COLOR_MASK_NONE;

    gp.InputLayout.LayoutElements = layout;
    gp.InputLayout.NumElements = _countof (layout);

    // ⚠️ THE FLAT PS IS BOUND EVEN THOUGH NOTHING IT RETURNS SURVIVES THE MASK.
    // A null pixel shader is legal in D3D12 and is the cheaper depth-only form,
    // but it is not the shape of any other pipeline here and the saving is a
    // flat constant-colour shader on geometry already being rasterised for its
    // depth. Matching the rest is worth more than the invocations.
    pci.pVS = impl_->vs;
    pci.pPS = impl_->flatPs;
    pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;

    device->CreateGraphicsPipelineState (pci, &impl_->occlusionDepthPso);
    if (impl_->occlusionDepthPso == nullptr) {
        error = "Diligent CreateGraphicsPipelineState(ArchViz occlusion depth prepass) failed";
        return false;
    }
    const Diligent::SHADER_TYPE stages[2] = { Diligent::SHADER_TYPE_VERTEX, Diligent::SHADER_TYPE_PIXEL };
    for (Diligent::SHADER_TYPE stage : stages) {
        if (Diligent::IShaderResourceVariable* variable =
                impl_->occlusionDepthPso->GetStaticVariableByName (stage, "ArchVizConstants"))
            variable->Set (impl_->constants);
    }
    impl_->occlusionDepthPso->CreateShaderResourceBinding (&impl_->occlusionDepthSrb, true);
    if (impl_->occlusionDepthSrb == nullptr) {
        error = "Diligent CreateShaderResourceBinding(ArchViz occlusion depth prepass) failed";
        return false;
    }
    return true;
}

bool DiligentScene::DrawOcclusionDepth (Diligent::IDeviceContext* context, const float viewProj[16], bool enabled,
                                        bool modelIsDrawn, uint32_t colorBufferFormat, uint32_t depthBufferFormat)
{
    if (impl_ == nullptr || context == nullptr || !impl_->ready)
        return false;

    OcclusionPrepassInputs inputs;
    inputs.enabled = enabled;
    inputs.modelIsDrawn = modelIsDrawn;
    inputs.renderMode = impl_->renderMode;
    inputs.elementCount = impl_->elements.size ();
    if (!OcclusionPrepassWanted (inputs))
        return false;

    // ⚠️ BUILT ON FIRST USE, AND ONCE -- the same shape as DrawGhPreview and
    // DrawStorySlices, for the same two reasons. The formats are taken here
    // rather than cached at Init so a target rebuilt at a new format cannot
    // leave the pass compiled against the old one; and a pipeline that will not
    // compile will not compile on the next frame either, so the failure latches
    // instead of putting the HLSL compiler in the frame loop sixty times a
    // second. A failure is logged once and never fails the scene: the picture
    // that results is exactly the one the overlay had before this task.
    if (impl_->occlusionDepthPso == nullptr) {
        if (impl_->occlusionDepthInitFailed)
            return false;
        std::string initError;
        if (!CreateOcclusionDepthPipeline (impl_->device, colorBufferFormat, depthBufferFormat, initError)) {
            impl_->occlusionDepthInitFailed = true;
            ArchVizLog ("Diligent scene: occlusion prepass unavailable (" + initError +
                        ") -- preview will draw over the building");
            return false;
        }
    }

    // ⚠️ THE CONSTANTS ARE UPLOADED WHOLE AND ONLY `viewProj` IS READ. The
    // buffer is shared by every pass so the camera cannot be right in one and
    // stale in another; leaving the rest at their defaults is safe precisely
    // because the masked-off pixel shader's output goes nowhere.
    DiligentSceneConstants constants;
    std::memcpy (constants.viewProj, viewProj, sizeof (float) * 16);
    UploadConstants (context, impl_->constants, constants);

    context->SetPipelineState (impl_->occlusionDepthPso);
    context->CommitShaderResources (impl_->occlusionDepthSrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // ⚠️ ELEMENTS ONLY. The gnomon, the ground plane and the debug cube are
    // INSTRUMENTS, not architecture -- the id pass and the wireframe exclude
    // them for the same reason. An overlay whose corner gnomon occluded a
    // Grasshopper result would be hiding the answer behind the compass.
    //
    // ⚠️ AND OPAQUE RANGES ONLY, which is the clause that keeps this pass
    // agreeing with the two others that write depth. A pane of glass does not
    // write depth in the main pass (DiligentScene.cpp: `DepthWriteEnable =
    // blended ? False : True`) and does not cast a shadow (RenderShadowMap says
    // why); if it occluded here, a curtain-walled facade would hide every
    // preview inside the building -- and it would do so ONLY in wireframe mode,
    // so one model would occlude differently depending on a display switch.
    size_t draws = 0;
    for (const Entry& e : impl_->elements) {
        if (e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
            continue;
        // ⚠️ BOUND LAZILY, INSIDE THE RANGE LOOP. An element that is entirely
        // glass -- a curtain wall is exactly that -- then costs no vertex-buffer
        // bind at all rather than one per frame per pane.
        bool bound = false;
        for (const MaterialRange& r : e.ranges) {
            if (r.indexCount == 0)
                continue;
            if (impl_->materials.Lookup (r.material).alpha < kOpaqueAlpha)
                continue;
            if (!bound) {
                BindMesh (context, e);
                bound = true;
            }
            DrawEntryRange (context, e, r);
            ++draws;
        }
    }
    impl_->drawCalls += draws;
    return draws > 0;
}

} // namespace archviz
} // namespace geomsrv
