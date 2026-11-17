// The section plane's back faces, caps and section line (issue #14 P2): when to cut, where the
// cut runs, and how it is drawn. The cut itself is pure and lives in SectionCaps.cpp; this is the
// scene's half -- the back-face pass, the snapshot, the worker, the upload and the draw.
//
// ⚠️ OFF THE RENDER THREAD, LATEST WINS. Cutting every element the plane crosses is milliseconds on
// a house and far more on a campus, and the plane is dragged. So a build runs on a worker from a
// SNAPSHOT of shared meshes; while it runs the previous caps stay on screen (`building` tells the
// HUD), and when it lands, a plane that moved again in the meantime simply starts the next one.
// Nothing here blocks a frame on a build, except teardown (DiligentSceneImpl ShutdownStorySlices).
// The back faces need no build at all, which is why they are drawn first and always.
//
// ⚠️ STALENESS IS A FINGERPRINT, NOT A REVISION COUNTER. An upsert replaces an element's mesh
// object and a hide flips `hidden`, in different files and command paths; hashing (mesh pointer,
// hidden) per element each frame the caps are on catches every one of them at O(elements), and
// cannot be forgotten by the next path that changes geometry. The cap's SHAPE (line width, hatch)
// is in the key too; its colours are not, so recolouring never re-cuts.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/MatrixMath.hpp"

#include <cstring>
#include <string>

namespace geomsrv {
namespace archviz {

namespace {

uint64_t Mix (uint64_t h, uint64_t value)
{
    return (h ^ value) * 1099511628211ull;
}

uint64_t MixFloat (uint64_t h, float value)
{
    uint32_t bits = 0;
    std::memcpy (&bits, &value, sizeof (bits));
    return Mix (h, bits);
}

void UnpackRgba (uint32_t rgba, float out[4])
{
    for (int k = 0; k < 4; ++k)
        out[k] = float ((rgba >> (24 - 8 * k)) & 0xFF) / 255.0f;
}

} // namespace

void DiligentScene::SetSectionCapStyle (const SectionCapStyle& style)
{
    if (impl_ != nullptr)
        impl_->sectionCaps.style = style;
}

void DiligentScene::DrawSectionCaps (Diligent::IDeviceContext* context, DiligentSceneConstants& constants,
                                     CullMode cull)
{
    if (impl_ == nullptr || context == nullptr || impl_->device == nullptr)
        return;
    Impl::SectionCaps& caps = impl_->sectionCaps;
    if (!caps.planeEnabled)
        return;

    // ---- 0. the back faces, flat in the poche colour -------------------------------------------
    // ⚠️ AFTER THE SHADED IMAGE, DEPTH-TESTED, AT THE INVERSE CULL: only back faces nothing nearer
    // covers survive -- exactly the inside of a cut solid. Writing depth keeps the caps and every
    // line after them measured against the cut rather than against the empty space behind it.
    // Glass is left alone: its back faces are glass, and painting them would turn panes solid.
    // Under CullMode::None the shaded pass already drew the back faces, lit; nothing to add.
    const int cullIndex = CullIndex (cull);
    if (caps.style.backFaces && cull != CullMode::None && !caps.backFailed) {
        if (caps.backPso[cullIndex] == nullptr) {
            std::string error;
            if (!CreateSectionBackPipelines (error)) {
                caps.backFailed = true;
                ArchVizLog ("Diligent scene: section back faces unavailable (" + error + ")");
            }
        }
        if (!caps.backFailed) {
            float rgba[4];
            UnpackRgba (caps.style.fillRgba, rgba);
            rgba[3] = 1.0f;
            for (int k = 0; k < 4; ++k)
                constants.baseColor[k] = rgba[k];
            impl_->Upload (context, constants);
            context->SetPipelineState (caps.backPso[cullIndex]);
            context->CommitShaderResources (caps.backSrb[cullIndex],
                                            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            const ClayLook look = impl_->SurfaceLook (impl_->StudyModelShown ());
            for (const Entry& e : impl_->elements) {
                if (e.vertexBuffer == nullptr || e.indexBuffer == nullptr || e.hidden)
                    continue;
                BindMesh (context, e);
                for (const MaterialRange& r : e.ranges) {
                    if (DrawnAlpha (impl_->materials.Lookup (r.material), look) < kOpaqueAlpha)
                        continue;
                    DrawEntryRange (context, e, r);
                    ++impl_->drawCalls;
                }
            }
        }
    }
    if (!caps.style.caps || caps.initFailed)
        return;

    // ---- 1. what the caps should show now -------------------------------------------------
    const SectionCapShape& shape = caps.style.shape;
    uint64_t key = 1469598103934665603ull;
    key = Mix (key, uint64_t (caps.axis));
    key = MixFloat (key, caps.coordinate);
    key = Mix (key, caps.flipped ? 1u : 0u);
    key = MixFloat (key, shape.lineWidth);
    key = Mix (key, shape.hatch ? 1u : 0u);
    if (shape.hatch) {
        key = MixFloat (key, shape.hatchSpacing);
        key = MixFloat (key, shape.hatchAngleDegrees);
        key = MixFloat (key, shape.hatchWidth);
    }
    for (const Entry& e : impl_->elements) {
        key = Mix (key, uint64_t (reinterpret_cast<uintptr_t> (e.sectionMesh.get ())));
        key = Mix (key, e.hidden ? 1u : 0u);
    }
    if (key == 0)
        key = 1; // 0 means "nothing built"

    // ---- 2. a finished build lands ----------------------------------------------------------
    if (caps.inFlight.valid () && caps.inFlight.wait_for (std::chrono::seconds (0)) == std::future_status::ready) {
        SectionCapResult result = caps.inFlight.get ();
        const SectionCapMesh* meshes[3] = { &result.poche, &result.hatch, &result.line };
        bool uploaded = true;
        for (int k = 0; k < 3 && uploaded; ++k) {
            std::string error;
            if (!caps.layers[k].IsReady () &&
                !caps.layers[k].Init (impl_->device, impl_->initColorFormat, impl_->initDepthFormat, error)) {
                caps.initFailed = true;
                ArchVizLog ("Diligent scene: section caps unavailable (" + error + ")");
                return;
            }
            if (!caps.layers[k].Upload (impl_->device, context, {}, meshes[k]->vertices, meshes[k]->indices, error)) {
                ArchVizLog ("Diligent scene: section caps not uploaded (" + error + ")");
                uploaded = false;
            }
        }
        if (uploaded) {
            caps.builtKey = caps.inFlightKey;
            caps.builtAxis = caps.inFlightAxis;
            caps.counts = result.counts;
        }
    }

    // ---- 3. a stale one starts the next build -----------------------------------------------
    const bool stale = key != caps.builtKey;
    if (stale && !caps.inFlight.valid ()) {
        SectionCapRequest request;
        request.axis = caps.axis;
        request.coordinate = caps.coordinate;
        request.flipped = caps.flipped;
        request.shape = shape;
        request.meshes.reserve (impl_->elements.size ());
        for (const Entry& e : impl_->elements)
            if (!e.hidden && e.sectionMesh != nullptr)
                request.meshes.push_back (e.sectionMesh);
        caps.inFlightKey = key;
        caps.inFlightAxis = caps.axis;
        caps.inFlight =
            std::async (std::launch::async, [request = std::move (request)] () { return BuildSectionCaps (request); });
    }
    caps.counts.building = stale;

    // ---- 4. draw what was built, in its own plane's frame -----------------------------------
    // ⚠️ THE BUILT AXIS, NOT THE CURRENT ONE: caps cut on X drawn through Z's matrix would lie
    // across the model for the frames until the Z build lands.
    if (caps.builtKey == 0)
        return;
    float localToWorld[16];
    SectionLocalToWorld (caps.builtAxis, localToWorld);
    float capViewProj[16];
    Multiply (capViewProj, localToWorld, constants.viewProj);
    // Poche, hatch, line: coplanar and none writes depth, so the later one is simply on top.
    const uint32_t colours[3] = { caps.style.fillRgba, caps.style.hatchRgba, caps.style.lineRgba };
    for (int k = 0; k < 3; ++k) {
        if (!caps.layers[k].IsReady () || (k == 1 && !shape.hatch))
            continue;
        StorySliceLayer::DrawParams params;
        params.fillRgba = colours[k];
        params.drawFill = true;
        // Behind the kept geometry, a cap is hidden like any face: it is the solid's own cut surface.
        params.occluded = OccludedStyle::Hidden;
        caps.layers[k].Draw (context, capViewProj, impl_->viewportWidth, impl_->viewportHeight, params);
    }
}

// The back-face pass's pipelines: the shaded pass's own VS (so the clip is identical) and the
// flat PS, opaque, depth-written, culling what the visible pass keeps.
bool DiligentScene::CreateSectionBackPipelines (std::string& error)
{
    if (impl_->device == nullptr || impl_->vs == nullptr || impl_->flatPs == nullptr) {
        error = "the section back faces need the scene's mesh VS and flat PS, which Init builds first";
        return false;
    }
    Impl::SectionCaps& caps = impl_->sectionCaps;
    const Diligent::LayoutElement layout[] = {
        Diligent::LayoutElement { 0, 0, 3, Diligent::VT_FLOAT32, Diligent::False },
        Diligent::LayoutElement { 1, 0, 3, Diligent::VT_FLOAT32, Diligent::False },
        Diligent::LayoutElement { 2, 0, 4, Diligent::VT_UINT8, Diligent::True },
    };
    for (int i = 0; i < kCullModeCount; ++i) {
        const CullMode cull = i == 0 ? CullMode::Ccw : (i == 1 ? CullMode::Cw : CullMode::None);
        Diligent::GraphicsPipelineStateCreateInfo pci;
        pci.PSODesc.Name = "ArchViz section back faces PSO";
        pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
        Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
        gp.NumRenderTargets = 1;
        gp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (impl_->initColorFormat);
        gp.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (impl_->initDepthFormat);
        gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        gp.RasterizerDesc.FillMode = Diligent::FILL_MODE_SOLID;
        gp.RasterizerDesc.CullMode = ToDiligentCull (InverseCull (cull));
        gp.RasterizerDesc.FrontCounterClockwise = Diligent::False;
        gp.DepthStencilDesc.DepthEnable = Diligent::True;
        gp.DepthStencilDesc.DepthWriteEnable = Diligent::True;
        gp.DepthStencilDesc.DepthFunc = Diligent::COMPARISON_FUNC_LESS_EQUAL;
        gp.InputLayout.LayoutElements = layout;
        gp.InputLayout.NumElements = _countof (layout);
        pci.pVS = impl_->vs;
        pci.pPS = impl_->flatPs;
        impl_->device->CreateGraphicsPipelineState (pci, &caps.backPso[i]);
        if (caps.backPso[i] == nullptr) {
            error = "Diligent CreateGraphicsPipelineState(section back faces) failed";
            return false;
        }
        for (const Diligent::SHADER_TYPE stage : { Diligent::SHADER_TYPE_VERTEX, Diligent::SHADER_TYPE_PIXEL })
            if (Diligent::IShaderResourceVariable* variable =
                    caps.backPso[i]->GetStaticVariableByName (stage, "ArchVizConstants"))
                variable->Set (impl_->constants);
        caps.backPso[i]->CreateShaderResourceBinding (&caps.backSrb[i], true);
        if (caps.backSrb[i] == nullptr) {
            error = "Diligent CreateShaderResourceBinding(section back faces) failed";
            return false;
        }
    }
    return true;
}

} // namespace archviz
} // namespace geomsrv
