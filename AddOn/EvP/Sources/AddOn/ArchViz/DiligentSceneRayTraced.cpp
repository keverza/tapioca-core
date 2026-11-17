// The ray-traced display style's GPU half (issue #231): capability, acceleration structures,
// accumulation and dispatch. What can be decided without a device is RayTracedView.hpp; the
// shader is RayTracedShader.hpp.
//
// ⚠️ OPT-IN, LATCHED, AND NEVER IN THE WAY. Nothing here is built until the style is turned on;
// any refusal -- a D3D11 device, no inline ray tracing, no DXC, a pipeline the driver rejects --
// latches with its reason (shown in the HUD) and the frame stays the raster frame. Draw() calls
// TraceView after the raster resolve inputs exist; a null return means "use the raster image".
//
// ⚠️ ONE BLAS FOR THE WHOLE MODEL, REBUILT ON A FINGERPRINT. The renderer already retains every
// element's triangles for the section caps (SectionMesh); they are flattened into one soup with
// a per-triangle material slot, so a hit needs no bindless lookup. The fingerprint is the caps'
// own (mesh pointer + hidden per element), so an upsert, a removal or a hide rebuilds and nothing
// else does. Material COLOURS are a per-frame table and never rebuild geometry.
//
// ⚠️ D3D12 PITFALLS (DiligentNativeInterop.hpp): every buffer here is USAGE_DEFAULT and written
// with UpdateBuffer, never USAGE_DYNAMIC, whose contents D3D12 discards at the end of a frame.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/RayTracedShader.hpp"
#include "ArchViz/SurfaceClassifier.hpp"

#include <BottomLevelAS.h>
#include <TopLevelAS.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace geomsrv {
namespace archviz {

namespace {

// ⚠️ FIELD FOR FIELD WITH kRayTracedCS's cbuffer.
struct RayTracedConstants {
    float invViewProj[16];
    float sun[4];
    float sky[4];
    float ground[4];
    float env[4];
    float plane[4];
    float cap[4];
    uint32_t frame[4];
    float ray[4];
};
static_assert (sizeof (RayTracedConstants) == 16 * 4 + 8 * 16, "kRayTracedCS cbuffer layout");

struct GpuMaterial {
    float colorAlpha[4];
    float params[4];
};

float SrgbToLinear (float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow ((c + 0.055f) / 1.055f, 2.4f);
}

} // namespace

struct RayTracedGpu {
    RefCntAutoPtr<Diligent::IPipelineState> pso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
    RefCntAutoPtr<Diligent::IBuffer> constants;
    RefCntAutoPtr<Diligent::IBuffer> positions, indices, triangleMaterial, materials;
    RefCntAutoPtr<Diligent::IBottomLevelAS> blas;
    RefCntAutoPtr<Diligent::ITopLevelAS> tlas;
    RefCntAutoPtr<Diligent::IBuffer> blasScratch, tlasScratch, instances;
    RefCntAutoPtr<Diligent::ITexture> accum;
    RefCntAutoPtr<Diligent::ITexture> dummyEnv;
    std::vector<int32_t> materialIds;
    std::vector<GpuMaterial> lastMaterials;
    uint64_t geometryKey = 0;
    uint64_t triangles = 0;
    Diligent::ITextureView* boundEnv = nullptr;
    bool srbDirty = true;
};

void DiligentScene::SetRayTraced (const RayTracedSettings& settings)
{
    if (impl_ == nullptr)
        return;
    Impl::RayTraced& rt = impl_->rayTraced;
    if (!settings.enabled && rt.settings.enabled)
        rt.accumulator.Reset ();
    rt.settings = settings;
}

// Why this device cannot run the style, or empty when it may try.
std::string DiligentScene::RayTracingRefusal () const
{
    if (impl_ == nullptr || impl_->device == nullptr)
        return "no device";
    const Diligent::RenderDeviceInfo& info = impl_->device->GetDeviceInfo ();
    if (info.Type != Diligent::RENDER_DEVICE_TYPE_D3D12)
        return "the viewer runs on D3D11 (no hardware ray tracing on this adapter, or D3D11 forced)";
    if (info.Features.RayTracing != Diligent::DEVICE_FEATURE_STATE_ENABLED)
        return "the D3D12 device has ray tracing disabled";
    if ((impl_->device->GetAdapterInfo ().RayTracing.CapFlags & Diligent::RAY_TRACING_CAP_FLAG_INLINE_RAY_TRACING) == 0)
        return "the adapter has no inline ray tracing (DXR 1.1)";
    if (impl_->rayTraced.failed)
        return impl_->rayTraced.failure;
    return {};
}

bool DiligentScene::CreateRayTracedPipeline (std::string& error)
{
    Diligent::IRenderDevice* device = impl_->device;
    auto gpu = std::make_shared<RayTracedGpu> ();

    Diligent::ShaderCreateInfo sci;
    sci.Desc.Name = "ArchViz ray-traced CS";
    sci.Desc.ShaderType = Diligent::SHADER_TYPE_COMPUTE;
    sci.EntryPoint = "main";
    sci.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
    // ⚠️ ONLY DXC COMPILES RayQuery (SM 6.5). It is bundled beside Tapioca.apx and loaded by
    // DiligentDeviceFactory; without it this is the refusal the HUD shows.
    sci.ShaderCompiler = Diligent::SHADER_COMPILER_DXC;
    sci.HLSLVersion = { 6, 5 };
    sci.Source = kRayTracedCS;
    RefCntAutoPtr<Diligent::IShader> cs;
    device->CreateShader (sci, &cs, nullptr);
    if (cs == nullptr) {
        error = "the ray-traced shader did not compile (dxcompiler.dll missing beside Tapioca.apx, or rejected)";
        return false;
    }

    Diligent::SamplerDesc envSampler;
    envSampler.MinFilter = Diligent::FILTER_TYPE_LINEAR;
    envSampler.MagFilter = Diligent::FILTER_TYPE_LINEAR;
    envSampler.MipFilter = Diligent::FILTER_TYPE_LINEAR;
    envSampler.AddressU = Diligent::TEXTURE_ADDRESS_WRAP;
    envSampler.AddressV = Diligent::TEXTURE_ADDRESS_CLAMP;
    envSampler.AddressW = Diligent::TEXTURE_ADDRESS_CLAMP;
    const Diligent::ImmutableSamplerDesc samplers[] = {
        { Diligent::SHADER_TYPE_COMPUTE, "g_envMap_sampler", envSampler },
    };
    Diligent::ComputePipelineStateCreateInfo pci;
    pci.PSODesc.Name = "ArchViz ray-traced PSO";
    pci.PSODesc.PipelineType = Diligent::PIPELINE_TYPE_COMPUTE;
    pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    pci.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    pci.PSODesc.ResourceLayout.NumImmutableSamplers = _countof (samplers);
    pci.pCS = cs;
    device->CreateComputePipelineState (pci, &gpu->pso);
    if (gpu->pso == nullptr) {
        error = "Diligent CreateComputePipelineState(ray-traced) failed";
        return false;
    }

    Diligent::BufferDesc cb;
    cb.Name = "ArchViz ray-traced constants";
    cb.Usage = Diligent::USAGE_DEFAULT;
    cb.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    cb.Size = sizeof (RayTracedConstants);
    device->CreateBuffer (cb, nullptr, &gpu->constants);

    const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    Diligent::TextureDesc td;
    td.Name = "ArchViz ray-traced empty sky";
    td.Type = Diligent::RESOURCE_DIM_TEX_2D;
    td.Width = td.Height = 1;
    td.Format = Diligent::TEX_FORMAT_RGBA32_FLOAT;
    td.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    td.Usage = Diligent::USAGE_IMMUTABLE;
    Diligent::TextureSubResData sub { zero, sizeof (zero) };
    Diligent::TextureData data { &sub, 1 };
    device->CreateTexture (td, &data, &gpu->dummyEnv);
    if (gpu->constants == nullptr || gpu->dummyEnv == nullptr) {
        error = "the ray-traced constants or sky placeholder could not be created";
        return false;
    }
    impl_->rayTraced.gpu = gpu;
    return true;
}

// Flattens the visible elements and builds one BLAS and one TLAS over them.
bool DiligentScene::BuildRayTracedScene (Diligent::IDeviceContext* context, uint64_t key, std::string& error)
{
    RayTracedGpu& gpu = *impl_->rayTraced.gpu;
    Diligent::IRenderDevice* device = impl_->device;
    std::vector<RayTracedSource> sources;
    sources.reserve (impl_->elements.size ());
    for (const Entry& e : impl_->elements)
        if (!e.hidden && e.sectionMesh != nullptr)
            sources.push_back ({ e.sectionMesh, e.ranges });
    const RayTracedGeometry geometry = BuildRayTracedGeometry (sources);

    gpu.positions.Release ();
    gpu.indices.Release ();
    gpu.triangleMaterial.Release ();
    gpu.blas.Release ();
    gpu.tlas.Release ();
    gpu.srbDirty = true;
    gpu.geometryKey = key;
    gpu.triangles = geometry.TriangleCount ();
    gpu.materialIds = geometry.materials;
    gpu.lastMaterials.clear ();
    if (geometry.TriangleCount () == 0)
        return true; // an empty model traces as sky

    auto makeBuffer = [&] (const char* name, const void* bytes, size_t size, uint32_t stride, bool tracing,
                           RefCntAutoPtr<Diligent::IBuffer>& out) {
        Diligent::BufferDesc desc;
        desc.Name = name;
        desc.Usage = Diligent::USAGE_DEFAULT;
        desc.BindFlags = Diligent::BIND_SHADER_RESOURCE | (tracing ? Diligent::BIND_RAY_TRACING : Diligent::BIND_NONE);
        desc.Mode = Diligent::BUFFER_MODE_STRUCTURED;
        desc.ElementByteStride = stride;
        desc.Size = size;
        Diligent::BufferData init { bytes, size };
        device->CreateBuffer (desc, &init, &out);
        return out != nullptr;
    };
    if (!makeBuffer ("ArchViz ray-traced positions", geometry.positions.data (),
                     geometry.positions.size () * sizeof (float), 12, true, gpu.positions) ||
        !makeBuffer ("ArchViz ray-traced indices", geometry.indices.data (),
                     geometry.indices.size () * sizeof (uint32_t), 4, true, gpu.indices) ||
        !makeBuffer ("ArchViz ray-traced triangle materials", geometry.triangleMaterial.data (),
                     geometry.triangleMaterial.size () * sizeof (uint32_t), 4, false, gpu.triangleMaterial)) {
        error = "the ray-traced geometry buffers could not be created";
        return false;
    }

    const uint32_t vertexCount = uint32_t (geometry.positions.size () / 3);
    const uint32_t triangleCount = uint32_t (geometry.TriangleCount ());
    Diligent::BLASTriangleDesc triangles;
    triangles.GeometryName = "model";
    triangles.MaxVertexCount = vertexCount;
    triangles.VertexValueType = Diligent::VT_FLOAT32;
    triangles.VertexComponentCount = 3;
    triangles.MaxPrimitiveCount = triangleCount;
    triangles.IndexType = Diligent::VT_UINT32;
    Diligent::BottomLevelASDesc blasDesc;
    blasDesc.Name = "ArchViz ray-traced BLAS";
    blasDesc.Flags = Diligent::RAYTRACING_BUILD_AS_PREFER_FAST_TRACE;
    blasDesc.pTriangles = &triangles;
    blasDesc.TriangleCount = 1;
    device->CreateBLAS (blasDesc, &gpu.blas);
    if (gpu.blas == nullptr) {
        error = "Diligent CreateBLAS failed";
        return false;
    }
    auto ensureScratch = [&] (RefCntAutoPtr<Diligent::IBuffer>& scratch, uint64_t size, const char* name) {
        if (scratch != nullptr && scratch->GetDesc ().Size >= size)
            return true;
        scratch.Release ();
        Diligent::BufferDesc desc;
        desc.Name = name;
        desc.Usage = Diligent::USAGE_DEFAULT;
        desc.BindFlags = Diligent::BIND_RAY_TRACING;
        desc.Size = std::max<uint64_t> (size, 256);
        device->CreateBuffer (desc, nullptr, &scratch);
        return scratch != nullptr;
    };
    if (!ensureScratch (gpu.blasScratch, gpu.blas->GetScratchBufferSizes ().Build, "ArchViz ray-traced BLAS scratch")) {
        error = "the BLAS scratch buffer could not be created";
        return false;
    }
    Diligent::BLASBuildTriangleData triangleData;
    triangleData.GeometryName = triangles.GeometryName;
    triangleData.pVertexBuffer = gpu.positions;
    triangleData.VertexStride = 12;
    triangleData.VertexCount = vertexCount;
    triangleData.VertexValueType = Diligent::VT_FLOAT32;
    triangleData.VertexComponentCount = 3;
    triangleData.pIndexBuffer = gpu.indices;
    triangleData.IndexType = Diligent::VT_UINT32;
    triangleData.PrimitiveCount = triangleCount;
    // ⚠️ NOT OPAQUE: every candidate goes through the shader's loop, which is where the section
    // plane and glass are decided.
    triangleData.Flags = Diligent::RAYTRACING_GEOMETRY_FLAG_NONE;
    Diligent::BuildBLASAttribs blasBuild;
    blasBuild.pBLAS = gpu.blas;
    blasBuild.pTriangleData = &triangleData;
    blasBuild.TriangleDataCount = 1;
    blasBuild.pScratchBuffer = gpu.blasScratch;
    blasBuild.BLASTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    blasBuild.GeometryTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    blasBuild.ScratchBufferTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->BuildBLAS (blasBuild);

    Diligent::TopLevelASDesc tlasDesc;
    tlasDesc.Name = "ArchViz ray-traced TLAS";
    tlasDesc.MaxInstanceCount = 1;
    tlasDesc.Flags = Diligent::RAYTRACING_BUILD_AS_PREFER_FAST_TRACE;
    device->CreateTLAS (tlasDesc, &gpu.tlas);
    if (gpu.tlas == nullptr ||
        !ensureScratch (gpu.tlasScratch, gpu.tlas->GetScratchBufferSizes ().Build, "ArchViz ray-traced TLAS scratch")) {
        error = "Diligent CreateTLAS or its scratch buffer failed";
        return false;
    }
    if (gpu.instances == nullptr) {
        Diligent::BufferDesc desc;
        desc.Name = "ArchViz ray-traced instances";
        desc.Usage = Diligent::USAGE_DEFAULT;
        desc.BindFlags = Diligent::BIND_RAY_TRACING;
        desc.Size = Diligent::TLAS_INSTANCE_DATA_SIZE;
        device->CreateBuffer (desc, nullptr, &gpu.instances);
        if (gpu.instances == nullptr) {
            error = "the TLAS instance buffer could not be created";
            return false;
        }
    }
    Diligent::TLASBuildInstanceData instance;
    instance.InstanceName = "model";
    instance.pBLAS = gpu.blas;
    instance.CustomId = 0;
    instance.Mask = 0xFF;
    Diligent::BuildTLASAttribs tlasBuild;
    tlasBuild.pTLAS = gpu.tlas;
    tlasBuild.pInstances = &instance;
    tlasBuild.InstanceCount = 1;
    tlasBuild.pInstanceBuffer = gpu.instances;
    tlasBuild.pScratchBuffer = gpu.tlasScratch;
    tlasBuild.TLASTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    tlasBuild.BLASTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    tlasBuild.InstanceBufferTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    tlasBuild.ScratchBufferTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->BuildTLAS (tlasBuild);
    return true;
}

Diligent::ITextureView* DiligentScene::TraceView (Diligent::IDeviceContext* context,
                                                  const DiligentSceneConstants& constants, const float viewProj[16],
                                                  const float eye[3])
{
    Impl::RayTraced& rt = impl_->rayTraced;
    rt.stats = RayTracedStats {};
    rt.stats.maxSamples = rt.settings.maxSamples;
    if (!rt.settings.enabled || context == nullptr || impl_->hdrColorTexture == nullptr)
        return nullptr;
    rt.stats.reason = RayTracingRefusal ();
    if (!rt.stats.reason.empty ()) {
        rt.stats.state = RayTracedState::Unavailable;
        return nullptr;
    }
    auto fail = [&] (const std::string& why) -> Diligent::ITextureView* {
        rt.failed = true;
        rt.failure = why;
        rt.gpu.reset ();
        rt.stats.state = RayTracedState::Unavailable;
        rt.stats.reason = why;
        ArchVizLog ("Diligent scene: ray-traced style unavailable (" + why + ")");
        return nullptr;
    };
    std::string error;
    if (rt.gpu == nullptr && !CreateRayTracedPipeline (error))
        return fail (error);
    RayTracedGpu& gpu = *rt.gpu;

    // ---- geometry: rebuilt only when an element's mesh or visibility changed ---------------
    uint64_t geometryKey = kHashSeed;
    for (const Entry& e : impl_->elements) {
        const uintptr_t mesh = reinterpret_cast<uintptr_t> (e.sectionMesh.get ());
        geometryKey = HashBytes (geometryKey, &mesh, sizeof (mesh));
        const uint8_t hidden = e.hidden ? 1 : 0;
        geometryKey = HashBytes (geometryKey, &hidden, 1);
    }
    if (geometryKey != gpu.geometryKey && !BuildRayTracedScene (context, geometryKey, error))
        return fail (error);
    rt.stats.triangles = gpu.triangles;

    // ---- the material table, resolved as the raster pass resolves it ----------------------
    const ClayLook look = impl_->SurfaceLook (impl_->StudyModelShown ());
    std::vector<GpuMaterial> table (std::max<size_t> (gpu.materialIds.size (), 1));
    for (size_t k = 0; k < gpu.materialIds.size (); ++k) {
        const SurfaceMaterial& mat = impl_->materials.Lookup (gpu.materialIds[k]);
        const DrawnSurface drawn = ResolveDrawnSurface (mat, PresetFor (mat), look);
        table[k] = { { drawn.r, drawn.g, drawn.b, drawn.alpha }, { drawn.roughness, drawn.metallic, 0.0f, 0.0f } };
    }
    const size_t tableBytes = table.size () * sizeof (GpuMaterial);
    if (gpu.materials == nullptr || gpu.materials->GetDesc ().Size < tableBytes) {
        gpu.materials.Release ();
        Diligent::BufferDesc desc;
        desc.Name = "ArchViz ray-traced materials";
        desc.Usage = Diligent::USAGE_DEFAULT;
        desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
        desc.Mode = Diligent::BUFFER_MODE_STRUCTURED;
        desc.ElementByteStride = sizeof (GpuMaterial);
        desc.Size = tableBytes;
        impl_->device->CreateBuffer (desc, nullptr, &gpu.materials);
        if (gpu.materials == nullptr)
            return fail ("the ray-traced material table could not be created");
        gpu.lastMaterials.clear ();
        gpu.srbDirty = true;
    }
    if (gpu.lastMaterials.size () != table.size () ||
        std::memcmp (gpu.lastMaterials.data (), table.data (), tableBytes) != 0) {
        context->UpdateBuffer (gpu.materials, 0, tableBytes, table.data (),
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        gpu.lastMaterials = table;
    }

    // ---- the accumulation target, at the HDR target's size --------------------------------
    const Diligent::TextureDesc& hdr = impl_->hdrColorTexture->GetDesc ();
    if (gpu.accum == nullptr || gpu.accum->GetDesc ().Width != hdr.Width ||
        gpu.accum->GetDesc ().Height != hdr.Height) {
        gpu.accum.Release ();
        Diligent::TextureDesc desc;
        desc.Name = "ArchViz ray-traced accumulation";
        desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
        desc.Width = hdr.Width;
        desc.Height = hdr.Height;
        desc.Format = Diligent::TEX_FORMAT_RGBA32_FLOAT;
        desc.BindFlags = Diligent::BIND_SHADER_RESOURCE | Diligent::BIND_UNORDERED_ACCESS;
        desc.Usage = Diligent::USAGE_DEFAULT;
        impl_->device->CreateTexture (desc, nullptr, &gpu.accum);
        if (gpu.accum == nullptr)
            return fail ("the ray-traced accumulation target could not be created");
        gpu.srbDirty = true;
        rt.accumulator.Reset ();
    }

    // ---- the constants, and the key everything above feeds ---------------------------------
    RayTracedConstants c {};
    if (!InvertMatrix4 (viewProj, c.invViewProj))
        return nullptr;
    // The near plane's NDC depth: 0, or 1 under reversed Z -- whichever unprojects nearer the eye.
    auto distanceAt = [&] (float z) {
        const float v[4] = { 0.0f, 0.0f, z, 1.0f };
        float p[4];
        TransformPoint (p, v, c.invViewProj);
        if (std::fabs (p[3]) < 1e-12f)
            return 1e30f;
        const float d[3] = { p[0] / p[3] - eye[0], p[1] / p[3] - eye[1], p[2] / p[3] - eye[2] };
        return std::sqrt (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    };
    const bool envEnabled = constants.envParams[2] > 0.5f;
    const float directWeight =
        envEnabled ? std::clamp (constants.materialParams[1], 0.0f, 1.0f) : 1.0f - constants.ambient;
    for (int k = 0; k < 3; ++k) {
        c.sun[k] = constants.sunDir[k];
        c.sky[k] = constants.skyColor[k];
        c.ground[k] = constants.groundColor[k];
        c.plane[k] = impl_->sectionPlane[k];
    }
    c.sun[3] = directWeight * constants.skyColor[3];
    c.sky[3] = constants.ambient;
    c.env[0] = constants.envParams[0];
    c.env[1] = constants.envParams[1];
    Diligent::ITextureView* envView = envEnabled ? impl_->environment.ShaderView () : nullptr;
    c.env[2] = envView != nullptr ? 1.0f : 0.0f;
    c.env[3] = impl_->environmentBackground ? 1.0f : 0.0f;
    c.plane[3] = impl_->sectionPlane[3];
    const uint32_t capRgba = impl_->sectionCaps.style.fillRgba;
    for (int k = 0; k < 3; ++k)
        c.cap[k] = SrgbToLinear (float ((capRgba >> (24 - 8 * k)) & 0xFF) / 255.0f);
    c.cap[3] = impl_->sectionCaps.planeEnabled ? 1.0f : 0.0f;
    c.frame[0] = hdr.Width;
    c.frame[1] = hdr.Height;
    c.frame[3] = std::clamp (rt.settings.bounces, 0u, 8u);
    c.ray[0] = distanceAt (0.0f) <= distanceAt (1.0f) ? 0.0f : 1.0f;

    uint64_t key = HashBytes (kHashSeed, &c, sizeof (c)); // frame[2] and ray[1] are still zero here
    key = HashBytes (key, &geometryKey, sizeof (geometryKey));
    key = HashBytes (key, table.data (), tableBytes);
    key = HashBytes (key, &envView, sizeof (envView));
    const RayTracedStep step = rt.accumulator.Advance (key, (std::max) (rt.settings.maxSamples, 1u));
    rt.stats.samples = rt.accumulator.Samples ();
    if (!step.show) {
        rt.stats.state = RayTracedState::Moving;
        return nullptr;
    }
    rt.stats.state = step.trace > 0 ? RayTracedState::Accumulating : RayTracedState::Converged;

    // ---- trace ---------------------------------------------------------------------------
    if (step.trace > 0 && gpu.tlas != nullptr) {
        Diligent::ITextureView* env =
            envView != nullptr ? envView : gpu.dummyEnv->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
        if (gpu.srbDirty || gpu.srb == nullptr || gpu.boundEnv != env) {
            gpu.srb.Release ();
            gpu.pso->CreateShaderResourceBinding (&gpu.srb, true);
            if (gpu.srb == nullptr)
                return fail ("Diligent CreateShaderResourceBinding(ray-traced) failed");
            auto set = [&] (const char* name, Diligent::IDeviceObject* object) {
                if (Diligent::IShaderResourceVariable* v =
                        gpu.srb->GetVariableByName (Diligent::SHADER_TYPE_COMPUTE, name))
                    v->Set (object);
            };
            set ("RayTracedConstants", gpu.constants);
            set ("g_scene", gpu.tlas);
            set ("g_positions", gpu.positions->GetDefaultView (Diligent::BUFFER_VIEW_SHADER_RESOURCE));
            set ("g_indices", gpu.indices->GetDefaultView (Diligent::BUFFER_VIEW_SHADER_RESOURCE));
            set ("g_triangleMaterial", gpu.triangleMaterial->GetDefaultView (Diligent::BUFFER_VIEW_SHADER_RESOURCE));
            set ("g_materials", gpu.materials->GetDefaultView (Diligent::BUFFER_VIEW_SHADER_RESOURCE));
            set ("g_envMap", env);
            set ("g_accum", gpu.accum->GetDefaultView (Diligent::TEXTURE_VIEW_UNORDERED_ACCESS));
            gpu.boundEnv = env;
            gpu.srbDirty = false;
        }
        c.frame[2] = step.firstSample;
        c.ray[1] = float (step.trace);
        context->UpdateBuffer (gpu.constants, 0, sizeof (c), &c, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->SetPipelineState (gpu.pso);
        context->CommitShaderResources (gpu.srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Diligent::DispatchComputeAttribs dispatch;
        dispatch.ThreadGroupCountX = (hdr.Width + 7) / 8;
        dispatch.ThreadGroupCountY = (hdr.Height + 7) / 8;
        dispatch.ThreadGroupCountZ = 1;
        context->DispatchCompute (dispatch);
    }
    else if (gpu.tlas == nullptr) {
        return nullptr; // an empty model: the raster sky is the same picture
    }
    return gpu.accum->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
}

RayTracedStats DiligentScene::RayTracedStatus () const
{
    if (impl_ == nullptr)
        return {};
    RayTracedStats stats = impl_->rayTraced.stats;
    if (!impl_->rayTraced.settings.enabled) {
        stats.state = RayTracedState::Off;
        stats.reason = RayTracingRefusal ();
    }
    return stats;
}

} // namespace archviz
} // namespace geomsrv
