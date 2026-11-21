// The ray-traced display style's GPU half (issue #231) and the Rendered style's traced
// reflections (#232): capability, acceleration structures, accumulation and dispatch. What can be decided without a
// device is RayTracedView.hpp; the shader is RayTracedShader.hpp.
//
// ⚠️ OPT-IN, LATCHED, AND NEVER IN THE WAY. Nothing here is built until the style is turned on;
// any refusal -- a D3D11 device, no inline ray tracing, no DXC, a pipeline the driver rejects --
// latches with its reason (shown in the HUD) and the frame stays the raster frame. Draw() calls
// TraceView after the raster resolve inputs exist; a null return means "use the raster image".
// PrepareScreenSpaceReflection calls TraceReflections first; a null return means "run SSR".
//
// ⚠️ ONE SCENE, TWO PASSES. The style and the reflections share the pipeline object, the BLAS/TLAS,
// the material table and the constants (PrepareRayTracedFrame); only their outputs differ. So
// turning on Ray traced after Rendered costs no second acceleration structure.
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
#include <Query.h>
#include <TopLevelAS.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace geomsrv {
namespace archviz {

// ⚠️ FIELD FOR FIELD WITH kRayTracedCommon's cbuffer.
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
    float cone[4]; // #231: x the camera's spread per pixel, radians
};
static_assert (sizeof (RayTracedConstants) == 16 * 4 + 9 * 16, "kRayTracedCommon cbuffer layout");

namespace {

struct GpuMaterial {
    float colorAlpha[4];
    float params[4];
    float texture[4]; // #231: x slot (-1 untextured), yz 1 / repeat size, w mirror bits
};

float SrgbToLinear (float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow ((c + 0.055f) / 1.055f, 2.4f);
}

} // namespace

struct RayTracedGpu {
    RefCntAutoPtr<Diligent::IPipelineState> pso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
    // #232: the reflection pass -- its own pipeline and binding over the same scene.
    RefCntAutoPtr<Diligent::IPipelineState> reflectPso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> reflectSrb;
    RefCntAutoPtr<Diligent::ITexture> reflection;
    Diligent::ITextureView* reflectBound[4] = {}; // depth, normal, roughness, env the SRB holds
    bool reflectSrbDirty = true;
    // #233: the sky-light pass, the same way.
    RefCntAutoPtr<Diligent::IPipelineState> skyPso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> skySrb;
    RefCntAutoPtr<Diligent::ITexture> skyLight;
    Diligent::ITextureView* skyBound[4] = {}; // depth, normal, occlusion, env the SRB holds
    bool skySrbDirty = true;
    RefCntAutoPtr<Diligent::IBuffer> constants;
    RefCntAutoPtr<Diligent::IBuffer> positions, uvs, normals, indices, triangleMaterial, materials;
    // #231: the texture array, one view per slot (the white texel where a slot is unused), and the
    // image keys in slot order -- compared every frame, and every binding is rebuilt on a change.
    RefCntAutoPtr<Diligent::ITexture> whiteTexel;
    std::vector<Diligent::IDeviceObject*> textureViews;
    std::vector<uint64_t> textureKeys;
    uint32_t texturedMaterials = 0;
    uint32_t textureOverflow = 0;
    // #231: timestamp pairs around the style's dispatch, read back without waiting a few frames
    // later (RayTracedBudget). Empty where the device has no timestamp queries: one sample a frame.
    struct Timing {
        RefCntAutoPtr<Diligent::IQuery> begin, end;
        uint32_t samples = 0;
        bool pending = false;
    };
    std::vector<Timing> timings;
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
    Diligent::ITextureView* envView = nullptr; // this frame's sky, or the placeholder
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

    // ⚠️ ONLY DXC COMPILES RayQuery (SM 6.5). It is bundled beside Tapioca.apx and loaded by
    // DiligentDeviceFactory; without it this is the refusal the HUD shows.
    auto compile = [&] (const char* shaderName, const char* main, RefCntAutoPtr<Diligent::IShader>& out) {
        const std::string source = "#define RT_TEXTURE_SLOTS " + std::to_string (kRayTracedTextureSlots) + "\n" +
                                   std::string (kRayTracedCommon) + main;
        Diligent::ShaderCreateInfo sci;
        sci.Desc.Name = shaderName;
        sci.Desc.ShaderType = Diligent::SHADER_TYPE_COMPUTE;
        sci.EntryPoint = "main";
        sci.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
        sci.ShaderCompiler = Diligent::SHADER_COMPILER_DXC;
        sci.HLSLVersion = { 6, 5 };
        sci.Source = source.c_str ();
        sci.SourceLength = source.size ();
        device->CreateShader (sci, &out, nullptr);
        return out != nullptr;
    };
    RefCntAutoPtr<Diligent::IShader> cs, reflectCs, skyCs;
    if (!compile ("ArchViz ray-traced CS", kRayTracedAccumulateMain, cs) ||
        !compile ("ArchViz traced reflections CS", kRayTracedReflectMain, reflectCs) ||
        !compile ("ArchViz traced sky light CS", kRayTracedSkyMain, skyCs)) {
        error = "the ray-traced shaders did not compile (dxcompiler.dll missing beside Tapioca.apx, or rejected)";
        return false;
    }

    Diligent::SamplerDesc envSampler;
    envSampler.MinFilter = Diligent::FILTER_TYPE_LINEAR;
    envSampler.MagFilter = Diligent::FILTER_TYPE_LINEAR;
    envSampler.MipFilter = Diligent::FILTER_TYPE_LINEAR;
    envSampler.AddressU = Diligent::TEXTURE_ADDRESS_WRAP;
    envSampler.AddressV = Diligent::TEXTURE_ADDRESS_CLAMP;
    envSampler.AddressW = Diligent::TEXTURE_ADDRESS_CLAMP;
    // #231: the textures repeat both ways. Compute has no derivatives, so the mip is chosen in the
    // shader (ray cone) and the filter is trilinear, not anisotropic.
    Diligent::SamplerDesc albedoSampler;
    albedoSampler.MinFilter = Diligent::FILTER_TYPE_LINEAR;
    albedoSampler.MagFilter = Diligent::FILTER_TYPE_LINEAR;
    albedoSampler.MipFilter = Diligent::FILTER_TYPE_LINEAR;
    albedoSampler.AddressU = Diligent::TEXTURE_ADDRESS_WRAP;
    albedoSampler.AddressV = Diligent::TEXTURE_ADDRESS_WRAP;
    albedoSampler.AddressW = Diligent::TEXTURE_ADDRESS_WRAP;
    const Diligent::ImmutableSamplerDesc samplers[] = {
        { Diligent::SHADER_TYPE_COMPUTE, "g_envMap_sampler", envSampler },
        { Diligent::SHADER_TYPE_COMPUTE, "g_albedoSampler", albedoSampler },
    };
    Diligent::ComputePipelineStateCreateInfo pci;
    pci.PSODesc.Name = "ArchViz ray-traced PSO";
    pci.PSODesc.PipelineType = Diligent::PIPELINE_TYPE_COMPUTE;
    pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    pci.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    pci.PSODesc.ResourceLayout.NumImmutableSamplers = _countof (samplers);
    pci.pCS = cs;
    device->CreateComputePipelineState (pci, &gpu->pso);
    pci.PSODesc.Name = "ArchViz traced reflections PSO";
    pci.pCS = reflectCs;
    device->CreateComputePipelineState (pci, &gpu->reflectPso);
    pci.PSODesc.Name = "ArchViz traced sky light PSO";
    pci.pCS = skyCs;
    device->CreateComputePipelineState (pci, &gpu->skyPso);
    if (gpu->pso == nullptr || gpu->reflectPso == nullptr || gpu->skyPso == nullptr) {
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
    const uint8_t white[4] = { 255, 255, 255, 255 };
    td.Name = "ArchViz ray-traced empty texture slot";
    td.Format = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
    Diligent::TextureSubResData whiteSub { white, sizeof (white) };
    Diligent::TextureData whiteData { &whiteSub, 1 };
    device->CreateTexture (td, &whiteData, &gpu->whiteTexel);
    if (gpu->constants == nullptr || gpu->dummyEnv == nullptr || gpu->whiteTexel == nullptr) {
        error = "the ray-traced constants or sky/texture placeholders could not be created";
        return false;
    }
    if (device->GetDeviceInfo ().Features.TimestampQueries == Diligent::DEVICE_FEATURE_STATE_ENABLED) {
        gpu->timings.resize (4);
        for (RayTracedGpu::Timing& t : gpu->timings) {
            Diligent::QueryDesc qd { Diligent::QUERY_TYPE_TIMESTAMP };
            qd.Name = "ArchViz ray-traced timing";
            device->CreateQuery (qd, &t.begin);
            device->CreateQuery (qd, &t.end);
            if (t.begin == nullptr || t.end == nullptr) {
                gpu->timings.clear (); // no budget, not a failure: one sample a frame
                break;
            }
        }
    }
    gpu->textureViews.assign (kRayTracedTextureSlots,
                              gpu->whiteTexel->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
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
    gpu.uvs.Release ();
    gpu.normals.Release ();
    gpu.indices.Release ();
    gpu.triangleMaterial.Release ();
    gpu.blas.Release ();
    gpu.tlas.Release ();
    gpu.srbDirty = true;
    gpu.reflectSrbDirty = true;
    gpu.skySrbDirty = true;
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
        !makeBuffer ("ArchViz ray-traced texture coordinates", geometry.uvs.data (),
                     geometry.uvs.size () * sizeof (float), 8, false, gpu.uvs) ||
        !makeBuffer ("ArchViz ray-traced normals", geometry.normals.data (), geometry.normals.size () * sizeof (float),
                     12, false, gpu.normals) ||
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

void DiligentScene::LatchRayTracingFailure (const std::string& why)
{
    Impl::RayTraced& rt = impl_->rayTraced;
    rt.failed = true;
    rt.failure = why;
    rt.gpu.reset ();
    rt.stats.state = RayTracedState::Unavailable;
    rt.stats.reason = why;
    ArchVizLog ("Diligent scene: ray tracing unavailable (" + why + ")");
}

// Everything both passes need this frame: the pipelines, the scene (rebuilt on its fingerprint),
// the material table, and the constants with their view key. False means latched (or nothing to do).
bool DiligentScene::PrepareRayTracedFrame (Diligent::IDeviceContext* context, const DiligentSceneConstants& constants,
                                           const float viewProj[16], const float eye[3], uint32_t width,
                                           uint32_t height, RayTracedConstants& c, uint64_t& key)
{
    Impl::RayTraced& rt = impl_->rayTraced;
    if (context == nullptr || width == 0 || height == 0 || !RayTracingRefusal ().empty ())
        return false;
    std::string error;
    if (rt.gpu == nullptr && !CreateRayTracedPipeline (error)) {
        LatchRayTracingFailure (error);
        return false;
    }
    RayTracedGpu& gpu = *rt.gpu;

    // ---- geometry: rebuilt only when an element's mesh or visibility changed ---------------
    uint64_t geometryKey = kHashSeed;
    for (const Entry& e : impl_->elements) {
        const uintptr_t mesh = reinterpret_cast<uintptr_t> (e.sectionMesh.get ());
        geometryKey = HashBytes (geometryKey, &mesh, sizeof (mesh));
        const uint8_t hidden = e.hidden ? 1 : 0;
        geometryKey = HashBytes (geometryKey, &hidden, 1);
    }
    if (geometryKey != gpu.geometryKey && !BuildRayTracedScene (context, geometryKey, error)) {
        LatchRayTracingFailure (error);
        return false;
    }
    rt.stats.triangles = gpu.triangles;

    // ---- the material table, resolved as the raster pass resolves it ----------------------
    const ClayLook look = impl_->SurfaceLook (impl_->StudyModelShown ());
    // #231: the textures, gated exactly as the raster's (TexturesShown), and only images the GPU
    // holds -- a key without one draws its surface colour in both styles.
    const bool texturesShown = impl_->TexturesShown (look);
    std::vector<uint64_t> imageKeys (gpu.materialIds.size (), 0);
    for (size_t k = 0; k < gpu.materialIds.size (); ++k) {
        const SurfaceMaterial& mat = impl_->materials.Lookup (gpu.materialIds[k]);
        if (texturesShown && mat.textureKey != 0 && impl_->albedoTextures.count (mat.textureKey) != 0)
            imageKeys[k] = mat.textureKey;
    }
    const RayTracedTextureSlots slots = AssignTextureSlots (imageKeys, kRayTracedTextureSlots);
    std::vector<GpuMaterial> table (std::max<size_t> (gpu.materialIds.size (), 1));
    gpu.texturedMaterials = 0;
    for (size_t k = 0; k < gpu.materialIds.size (); ++k) {
        const SurfaceMaterial& mat = impl_->materials.Lookup (gpu.materialIds[k]);
        const DrawnSurface drawn = ResolveDrawnSurface (mat, PresetFor (mat), look);
        table[k] = { { drawn.r, drawn.g, drawn.b, drawn.alpha },
                     { drawn.roughness, drawn.metallic, drawn.reflectance, 0.0f },
                     { -1.0f, 1.0f, 1.0f, 0.0f } };
        if (slots.slotOf[k] >= 0) {
            table[k].texture[0] = float (slots.slotOf[k]);
            table[k].texture[1] = 1.0f / (std::max) (mat.textureSizeX, 1e-6f);
            table[k].texture[2] = 1.0f / (std::max) (mat.textureSizeY, 1e-6f);
            table[k].texture[3] = float ((mat.textureMirrorX ? 1 : 0) | (mat.textureMirrorY ? 2 : 0));
            ++gpu.texturedMaterials;
        }
    }
    gpu.textureOverflow = slots.overflow;
    rt.stats.texturedMaterials = gpu.texturedMaterials;
    rt.stats.textureOverflow = gpu.textureOverflow;
    if (slots.keys != gpu.textureKeys) {
        // ⚠️ EVERY BINDING HOLDS THE ARRAY, so a changed image set rebuilds all three.
        Diligent::IDeviceObject* white = gpu.whiteTexel->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
        gpu.textureViews.assign (kRayTracedTextureSlots, white);
        for (size_t slot = 0; slot < slots.keys.size (); ++slot)
            gpu.textureViews[slot] =
                impl_->albedoTextures.at (slots.keys[slot])->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
        gpu.textureKeys = slots.keys;
        gpu.srbDirty = true;
        gpu.reflectSrbDirty = true;
        gpu.skySrbDirty = true;
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
        if (gpu.materials == nullptr) {
            LatchRayTracingFailure ("the ray-traced material table could not be created");
            return false;
        }
        gpu.lastMaterials.clear ();
        gpu.srbDirty = true;
        gpu.reflectSrbDirty = true;
        gpu.skySrbDirty = true;
    }
    if (gpu.lastMaterials.size () != table.size () ||
        std::memcmp (gpu.lastMaterials.data (), table.data (), tableBytes) != 0) {
        context->UpdateBuffer (gpu.materials, 0, tableBytes, table.data (),
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        gpu.lastMaterials = table;
    }
    if (gpu.tlas == nullptr)
        return false; // an empty model: the raster picture is the same picture

    // ---- the constants, and the key everything above feeds ---------------------------------
    c = RayTracedConstants {};
    if (!InvertMatrix4 (viewProj, c.invViewProj))
        return false;
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
    c.frame[0] = width;
    c.frame[1] = height;
    c.frame[3] = std::clamp (rt.settings.bounces, 0u, 8u);
    c.ray[0] = distanceAt (0.0f) <= distanceAt (1.0f) ? 0.0f : 1.0f;
    // The environment's last mip: the hit's diffuse sky in the reflection pass.
    c.ray[2] = (std::max) (constants.envParams[3] - 1.0f, 0.0f);
    // #231: the angle between neighbouring pixels' rays at the centre, for the texture cone.
    auto rayThrough = [&] (float x, float y, float out[3]) {
        const float nearPoint[4] = { x, y, c.ray[0], 1.0f };
        float w[4];
        TransformPoint (w, nearPoint, c.invViewProj);
        const float inv = std::fabs (w[3]) > 1e-12f ? 1.0f / w[3] : 0.0f;
        float length = 0.0f;
        for (int k = 0; k < 3; ++k) {
            out[k] = w[k] * inv - eye[k];
            length += out[k] * out[k];
        }
        length = std::sqrt (length);
        for (int k = 0; k < 3; ++k)
            out[k] = length > 0.0f ? out[k] / length : 0.0f;
    };
    float centre[3], next[3];
    rayThrough (0.0f, 0.0f, centre);
    rayThrough (0.0f, 2.0f / float (height), next);
    const float cosine = std::clamp (centre[0] * next[0] + centre[1] * next[1] + centre[2] * next[2], -1.0f, 1.0f);
    c.cone[0] = std::acos (cosine);

    key = HashBytes (kHashSeed, &c, sizeof (c)); // frame[2], ray[1] and ray[3] are still zero here
    key = HashBytes (key, &geometryKey, sizeof (geometryKey));
    key = HashBytes (key, table.data (), tableBytes);
    key = HashBytes (key, &envView, sizeof (envView));
    key = HashBytes (key, gpu.textureKeys.data (), gpu.textureKeys.size () * sizeof (uint64_t));
    gpu.envView = envView != nullptr ? envView : gpu.dummyEnv->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    return true;
}

// Binds the scene's shared resources on a fresh SRB of either pipeline.
static void BindRayTracedScene (RayTracedGpu& gpu, Diligent::IShaderResourceBinding* srb)
{
    auto set = [&] (const char* name, Diligent::IDeviceObject* object) {
        if (Diligent::IShaderResourceVariable* v = srb->GetVariableByName (Diligent::SHADER_TYPE_COMPUTE, name))
            v->Set (object);
    };
    set ("RayTracedConstants", gpu.constants);
    set ("g_scene", gpu.tlas);
    set ("g_positions", gpu.positions->GetDefaultView (Diligent::BUFFER_VIEW_SHADER_RESOURCE));
    set ("g_uvs", gpu.uvs->GetDefaultView (Diligent::BUFFER_VIEW_SHADER_RESOURCE));
    set ("g_normals", gpu.normals->GetDefaultView (Diligent::BUFFER_VIEW_SHADER_RESOURCE));
    if (Diligent::IShaderResourceVariable* v = srb->GetVariableByName (Diligent::SHADER_TYPE_COMPUTE, "g_albedo"))
        v->SetArray (gpu.textureViews.data (), 0, Diligent::Uint32 (gpu.textureViews.size ()));
    set ("g_indices", gpu.indices->GetDefaultView (Diligent::BUFFER_VIEW_SHADER_RESOURCE));
    set ("g_triangleMaterial", gpu.triangleMaterial->GetDefaultView (Diligent::BUFFER_VIEW_SHADER_RESOURCE));
    set ("g_materials", gpu.materials->GetDefaultView (Diligent::BUFFER_VIEW_SHADER_RESOURCE));
    set ("g_envMap", gpu.envView);
}

Diligent::ITextureView* DiligentScene::TraceView (Diligent::IDeviceContext* context,
                                                  const DiligentSceneConstants& constants, const float viewProj[16],
                                                  const float eye[3])
{
    Impl::RayTraced& rt = impl_->rayTraced;
    rt.stats.state = RayTracedState::Off;
    rt.stats.samples = 0;
    rt.stats.maxSamples = rt.settings.maxSamples;
    rt.stats.reason.clear ();
    if (!rt.settings.enabled || context == nullptr || impl_->hdrColorTexture == nullptr)
        return nullptr;
    rt.stats.reason = RayTracingRefusal ();
    if (!rt.stats.reason.empty ()) {
        rt.stats.state = RayTracedState::Unavailable;
        return nullptr;
    }
    const Diligent::TextureDesc& hdr = impl_->hdrColorTexture->GetDesc ();
    RayTracedConstants c;
    uint64_t key = 0;
    if (!PrepareRayTracedFrame (context, constants, viewProj, eye, hdr.Width, hdr.Height, c, key))
        return nullptr;
    RayTracedGpu& gpu = *rt.gpu;

    // ---- the accumulation target, at the HDR target's size --------------------------------
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
        if (gpu.accum == nullptr) {
            LatchRayTracingFailure ("the ray-traced accumulation target could not be created");
            return nullptr;
        }
        gpu.srbDirty = true;
        rt.accumulator.Reset ();
    }

    // #231: finished dispatches report their cost; the budget turns it into this frame's count.
    for (RayTracedGpu::Timing& t : gpu.timings) {
        if (!t.pending)
            continue;
        Diligent::QueryDataTimestamp a, b;
        if (!t.begin->GetData (&a, sizeof (a), false) || !t.end->GetData (&b, sizeof (b), false))
            continue;
        t.pending = false;
        if (a.Frequency != 0 && b.Frequency != 0 && b.Counter >= a.Counter)
            rt.budget.Measured (
                (double (b.Counter) / double (b.Frequency) - double (a.Counter) / double (a.Frequency)) * 1000.0,
                t.samples);
    }
    const uint32_t perFrame = gpu.timings.empty () ? 1u : rt.budget.SamplesThisFrame (rt.settings.budgetMs);
    rt.stats.samplesPerFrame = perFrame;
    rt.stats.msPerSample = rt.budget.MsPerSample ();
    const RayTracedStep step = rt.accumulator.Advance (key, (std::max) (rt.settings.maxSamples, 1u), perFrame);
    rt.stats.samples = rt.accumulator.Samples ();
    if (!step.show) {
        rt.stats.state = RayTracedState::Moving;
        return nullptr;
    }
    rt.stats.state = step.trace > 0 ? RayTracedState::Accumulating : RayTracedState::Converged;
    if (step.trace > 0) {
        if (gpu.srbDirty || gpu.srb == nullptr || gpu.boundEnv != gpu.envView) {
            gpu.srb.Release ();
            gpu.pso->CreateShaderResourceBinding (&gpu.srb, true);
            if (gpu.srb == nullptr) {
                LatchRayTracingFailure ("Diligent CreateShaderResourceBinding(ray-traced) failed");
                return nullptr;
            }
            BindRayTracedScene (gpu, gpu.srb);
            if (Diligent::IShaderResourceVariable* v =
                    gpu.srb->GetVariableByName (Diligent::SHADER_TYPE_COMPUTE, "g_accum"))
                v->Set (gpu.accum->GetDefaultView (Diligent::TEXTURE_VIEW_UNORDERED_ACCESS));
            gpu.boundEnv = gpu.envView;
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
        RayTracedGpu::Timing* timing = nullptr;
        for (RayTracedGpu::Timing& t : gpu.timings)
            if (!t.pending) {
                timing = &t;
                break;
            }
        if (timing != nullptr)
            context->EndQuery (timing->begin);
        context->DispatchCompute (dispatch);
        if (timing != nullptr) {
            context->EndQuery (timing->end);
            timing->samples = step.trace;
            timing->pending = true;
        }
    }
    return gpu.accum->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
}

// #232: traced reflections in SSR's place. Same output contract as DiligentFX SSR -- reflected
// radiance in rgb, confidence in a -- so kArchVizSsrCompositePS composites it unchanged; the
// confidence is 1 wherever a ray was traced, because a traced ray never "misses" the way a
// screen-space one does: no hit is the sky, which is the right answer.
Diligent::ITextureView* DiligentScene::TraceReflections (Diligent::IDeviceContext* context,
                                                         const DiligentSceneConstants& constants,
                                                         const float viewProj[16], const float eye[3],
                                                         Diligent::ITextureView* depth, Diligent::ITextureView* normal,
                                                         Diligent::ITextureView* roughness, uint32_t frameIndex,
                                                         float roughnessThreshold)
{
    Impl::RayTraced& rt = impl_->rayTraced;
    rt.stats.reflectionsTraced = false;
    // ⚠️ ONLY UNDER TAA (see TraceSkyLight): a gloss gets four rays a frame, and TAA is what averages them.
    if (!rt.settings.reflections || !impl_->taaEnabled || depth == nullptr || normal == nullptr || roughness == nullptr)
        return nullptr;
    const Diligent::TextureDesc& size = depth->GetTexture ()->GetDesc ();
    RayTracedConstants c;
    uint64_t key = 0;
    if (!PrepareRayTracedFrame (context, constants, viewProj, eye, size.Width, size.Height, c, key))
        return nullptr;
    RayTracedGpu& gpu = *rt.gpu;

    if (gpu.reflection == nullptr || gpu.reflection->GetDesc ().Width != size.Width ||
        gpu.reflection->GetDesc ().Height != size.Height) {
        gpu.reflection.Release ();
        Diligent::TextureDesc desc;
        desc.Name = "ArchViz traced reflections";
        desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
        desc.Width = size.Width;
        desc.Height = size.Height;
        desc.Format = Diligent::TEX_FORMAT_RGBA16_FLOAT;
        desc.BindFlags = Diligent::BIND_SHADER_RESOURCE | Diligent::BIND_UNORDERED_ACCESS;
        desc.Usage = Diligent::USAGE_DEFAULT;
        impl_->device->CreateTexture (desc, nullptr, &gpu.reflection);
        if (gpu.reflection == nullptr) {
            LatchRayTracingFailure ("the traced reflection target could not be created");
            return nullptr;
        }
        gpu.reflectSrbDirty = true;
    }
    // ⚠️ THE G-BUFFER VIEWS ARE COMPARED, NOT ASSUMED: a resize recreates them, and a binding
    // that still holds the old ones reads freed textures.
    Diligent::ITextureView* bound[4] = { depth, normal, roughness, gpu.envView };
    if (gpu.reflectSrbDirty || gpu.srbDirty || gpu.reflectSrb == nullptr ||
        std::memcmp (bound, gpu.reflectBound, sizeof (bound)) != 0) {
        gpu.reflectSrb.Release ();
        gpu.reflectPso->CreateShaderResourceBinding (&gpu.reflectSrb, true);
        if (gpu.reflectSrb == nullptr) {
            LatchRayTracingFailure ("Diligent CreateShaderResourceBinding(traced reflections) failed");
            return nullptr;
        }
        BindRayTracedScene (gpu, gpu.reflectSrb);
        auto set = [&] (const char* name, Diligent::IDeviceObject* object) {
            if (Diligent::IShaderResourceVariable* v =
                    gpu.reflectSrb->GetVariableByName (Diligent::SHADER_TYPE_COMPUTE, name))
                v->Set (object);
        };
        set ("g_depth", depth);
        set ("g_normal", normal);
        set ("g_roughness", roughness);
        set ("g_reflection", gpu.reflection->GetDefaultView (Diligent::TEXTURE_VIEW_UNORDERED_ACCESS));
        std::memcpy (gpu.reflectBound, bound, sizeof (bound));
        gpu.reflectSrbDirty = false;
    }
    c.frame[2] = frameIndex;
    c.ray[3] = roughnessThreshold;
    context->UpdateBuffer (gpu.constants, 0, sizeof (c), &c, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->SetPipelineState (gpu.reflectPso);
    context->CommitShaderResources (gpu.reflectSrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::DispatchComputeAttribs dispatch;
    dispatch.ThreadGroupCountX = (size.Width + 7) / 8;
    dispatch.ThreadGroupCountY = (size.Height + 7) / 8;
    dispatch.ThreadGroupCountZ = 1;
    context->DispatchCompute (dispatch);
    rt.stats.reflectionsTraced = true;
    return gpu.reflection->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
}

// #233: traced sky light and sun in the AO texture's place (kRayTracedSkyMain). True when
// impl_->aoView now holds them -- with the AO intensity already applied, so the caller sets the AO
// lane to 2: the mesh shader reads that as intensity 1 plus "y is the traced sun".
bool DiligentScene::TraceSkyLight (Diligent::IDeviceContext* context, const DiligentSceneConstants& constants,
                                   const float viewProj[16], const float eye[3], uint32_t frameIndex)
{
    Impl::RayTraced& rt = impl_->rayTraced;
    rt.stats.skyLightTraced = false;
    // ⚠️ ONLY UNDER TAA. Four rays a pixel a frame is a noisy estimate that TAA averages; with TAA
    // off -- the Architecture preset -- that noise WAS the picture: every surface grainy, every
    // frame different (#231, RenderBench `glazing` architecture). Without TAA, GTAO stays.
    if (!rt.settings.skyLight || !impl_->taaEnabled || impl_->aoView == nullptr || impl_->gBuffer == nullptr)
        return false;
    Diligent::ITextureView* depth =
        impl_->gBuffer->GetBuffer (kGBufferDepth)->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    Diligent::ITextureView* normal =
        impl_->gBuffer->GetBuffer (kGBufferNormal)->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    const Diligent::TextureDesc& size = depth->GetTexture ()->GetDesc ();
    RayTracedConstants c;
    uint64_t key = 0;
    if (!PrepareRayTracedFrame (context, constants, viewProj, eye, size.Width, size.Height, c, key))
        return false;
    RayTracedGpu& gpu = *rt.gpu;

    if (gpu.skyLight == nullptr || gpu.skyLight->GetDesc ().Width != size.Width ||
        gpu.skyLight->GetDesc ().Height != size.Height) {
        gpu.skyLight.Release ();
        Diligent::TextureDesc desc;
        desc.Name = "ArchViz traced sky light";
        desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
        desc.Width = size.Width;
        desc.Height = size.Height;
        desc.Format = Diligent::TEX_FORMAT_RG16_FLOAT;
        desc.BindFlags = Diligent::BIND_SHADER_RESOURCE | Diligent::BIND_UNORDERED_ACCESS;
        desc.Usage = Diligent::USAGE_DEFAULT;
        impl_->device->CreateTexture (desc, nullptr, &gpu.skyLight);
        if (gpu.skyLight == nullptr) {
            LatchRayTracingFailure ("the traced sky light target could not be created");
            return false;
        }
        gpu.skySrbDirty = true;
    }
    // Compared, not assumed, as the reflection pass's views are: a resize recreates them.
    Diligent::ITextureView* bound[4] = { depth, normal, impl_->aoView, gpu.envView };
    if (gpu.skySrbDirty || gpu.srbDirty || gpu.skySrb == nullptr ||
        std::memcmp (bound, gpu.skyBound, sizeof (bound)) != 0) {
        gpu.skySrb.Release ();
        gpu.skyPso->CreateShaderResourceBinding (&gpu.skySrb, true);
        if (gpu.skySrb == nullptr) {
            LatchRayTracingFailure ("Diligent CreateShaderResourceBinding(traced sky light) failed");
            return false;
        }
        BindRayTracedScene (gpu, gpu.skySrb);
        auto set = [&] (const char* name, Diligent::IDeviceObject* object) {
            if (Diligent::IShaderResourceVariable* v =
                    gpu.skySrb->GetVariableByName (Diligent::SHADER_TYPE_COMPUTE, name))
                v->Set (object);
        };
        set ("g_depth", depth);
        set ("g_normal", normal);
        set ("g_occlusion", impl_->aoView);
        set ("g_skyLight", gpu.skyLight->GetDefaultView (Diligent::TEXTURE_VIEW_UNORDERED_ACCESS));
        std::memcpy (gpu.skyBound, bound, sizeof (bound));
        gpu.skySrbDirty = false;
    }
    c.frame[2] = frameIndex;
    c.ray[1] = constants.shadowParams[2]; // the raster's shadows on: only then does the ray shadow
    c.ray[3] = impl_->aoIntensity;
    context->UpdateBuffer (gpu.constants, 0, sizeof (c), &c, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->SetPipelineState (gpu.skyPso);
    context->CommitShaderResources (gpu.skySrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::DispatchComputeAttribs dispatch;
    dispatch.ThreadGroupCountX = (size.Width + 7) / 8;
    dispatch.ThreadGroupCountY = (size.Height + 7) / 8;
    dispatch.ThreadGroupCountZ = 1;
    context->DispatchCompute (dispatch);
    impl_->aoView = gpu.skyLight->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    rt.stats.skyLightTraced = true;
    return true;
}

RayTracedStats DiligentScene::RayTracedStatus () const
{
    if (impl_ == nullptr)
        return {};
    RayTracedStats stats = impl_->rayTraced.stats;
    if (!impl_->rayTraced.settings.enabled) {
        stats.state = RayTracedState::Off;
        stats.samples = 0;
        stats.reason = RayTracingRefusal ();
    }
    return stats;
}

} // namespace archviz
} // namespace geomsrv
