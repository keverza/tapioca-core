#include "ArchViz/EnvironmentMap.hpp"

#include "ArchViz/DiligentShaders.hpp"   // the prefilter stages and the shared cbuffer
#include "ArchViz/EnvironmentLighting.hpp"

#include <windows.h>
#include <d3d11.h>   // Must precede any Diligent D3D11 interop header (Probe 1a).
#include <DeviceContext.h>
#include <GraphicsTypes.h>
#include <Image.h>
#include <RefCntAutoPtr.hpp>
#include <Buffer.h>
#include <MapHelper.hpp>
#include <PipelineState.h>
#include <RenderDevice.h>
#include <Shader.h>
#include <ShaderResourceBinding.h>
#include <ShaderResourceVariable.h>
#include <Texture.h>
#include <TextureView.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace geomsrv {
namespace archviz {

using Diligent::RefCntAutoPtr;

struct EnvironmentMap::Impl {
    // The texture the model and the sky background sample. Its mip chain is
    // GGX-prefiltered when the pass below runs, box-filtered when it does not.
    RefCntAutoPtr<Diligent::ITexture> texture;
    Diligent::ITextureView* srv = nullptr;   // owned by `texture`
    uint32_t mipLevels = 0;
    bool ready = false;

    // ---- RE51.B6 -----------------------------------------------------------
    // The private upload target. ⚠️ NEVER BOUND FOR SHADING. It exists only to
    // give the prefilter something to READ while it writes `texture`, and it
    // carries the ordinary box mip chain the prefilter's own mip-bias needs --
    // a wide GGX lobe samples a low-resolution average of the sky, and that
    // average has to exist somewhere.
    RefCntAutoPtr<Diligent::ITexture> source;
    Diligent::ITextureView* sourceSrv = nullptr;

    // One render-target view per destination mip. Created once, at Init, for
    // the same binding reason the textures are.
    std::vector<RefCntAutoPtr<Diligent::ITextureView>> mipTargets;

    RefCntAutoPtr<Diligent::IPipelineState> prefilterPso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> prefilterSrb;
    RefCntAutoPtr<Diligent::IBuffer> prefilterConstants;
    std::string prefilterInitError;

    bool prefiltered = false;
    uint32_t prefilteredMips = 0;
    double prefilterMs = 0.0;
    std::string prefilterError;

    bool loaded = false;
    std::string loadedPath;
    ShIrradiance sh;
    float average[3] = {0.0f, 0.0f, 0.0f};
};

namespace {

// ---- RE51.B6: the GGX prefilter, CPU side ---------------------------------

// How many GGX samples each destination mip is filtered with.
//
// ⚠️ IT RISES WITH ROUGHNESS BECAUSE THE LOBE DOES. A near-mirror mip integrates
// over a few square degrees and is converged at 32 samples; the roughest mip
// integrates over most of the hemisphere and shows visible banding under a
// hundred. Spending the high count everywhere would multiply a load that is
// already the longest thing in the frame it happens on, for no visible gain --
// and the expensive mips ARE the sharp ones, because they are the large ones.
//
// The whole ladder is roughly 12 M lobe samples on a 2048x1024 chain, which is
// tens of milliseconds on any GPU that can run this viewport at all.
uint32_t PrefilterSampleCount (uint32_t mip, uint32_t mipCount)
{
    const float roughness = mipCount > 1 ? float (mip) / float (mipCount - 1) : 0.0f;
    return uint32_t (32.0f + 224.0f * roughness);
}

// Build the prefilter PSO, its SRB, its constant buffer and one render-target
// view per destination mip. Returns an empty string on success, or the reason
// the caller should fall back to the box mip chain.
//
// ⚠️ EVERY RESOURCE HERE IS CREATED ONCE, AT Init, exactly like the textures --
// and for a weaker but real version of the same reason. The per-mip render
// target views hold references to `texture`, so recreating them per load would
// churn views of a texture that never changes; and building a PSO inside a load
// would put shader compilation on the render thread at an unpredictable moment.
std::string BuildPrefilterPipeline (Diligent::IRenderDevice* device, EnvironmentMap::Impl& impl)
{
    const uint32_t mipCount = impl.mipLevels;
    if (mipCount < 2)
        return "the environment map has no mip chain to prefilter";

    RefCntAutoPtr<Diligent::IShader> vs;
    RefCntAutoPtr<Diligent::IShader> ps;
    {
        // ⚠️ THE SOURCE MUST OUTLIVE CreateShader -- `sci.Source` is borrowed.
        // The same trap DiligentScene::Init names; a temporary here compiles and
        // reads freed memory.
        const std::string vsSource = ArchVizShaderSource (kArchVizEnvPrefilterVS);
        Diligent::ShaderCreateInfo sci;
        sci.Desc.Name = "ArchViz environment prefilter VS";
        sci.Desc.ShaderType = Diligent::SHADER_TYPE_VERTEX;
        sci.EntryPoint = "main";
        sci.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
        sci.Source = vsSource.c_str ();
        sci.SourceLength = vsSource.size ();
        device->CreateShader (sci, &vs, nullptr);
    }
    if (vs == nullptr)
        return "the environment prefilter vertex shader did not compile";
    {
        // ⚠️ kArchVizEnvCommonPS IS THE PRELUDE, and it is not optional: EnvUv
        // and EnvDir live there, and they are the ONE equirectangular convention
        // this tree has. A prefilter with its own copy would be free to drift
        // from the shader that reads the result, and the symptom of that drift
        // is a reflection that is subtly rotated -- which nobody would attribute
        // to the prefilter.
        const std::string psSource = ArchVizShaderSource (kArchVizEnvCommonPS, kArchVizEnvPrefilterPS);
        Diligent::ShaderCreateInfo sci;
        sci.Desc.Name = "ArchViz environment prefilter PS";
        sci.Desc.ShaderType = Diligent::SHADER_TYPE_PIXEL;
        sci.EntryPoint = "main";
        sci.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
        sci.Source = psSource.c_str ();
        sci.SourceLength = psSource.size ();
        device->CreateShader (sci, &ps, nullptr);
    }
    if (ps == nullptr)
        return "the environment prefilter pixel shader did not compile";

    // The pass's own copy of the scene constant buffer. ⚠️ ONLY THREE FIELDS
    // MATTER -- g_envParams (for the mip count and the zero rotation) and
    // g_prefilterParams -- but the whole struct is uploaded because the cbuffer
    // declaration is shared, and a partial upload would read the rest as
    // whatever the buffer last held.
    {
        Diligent::BufferDesc bd;
        bd.Name = "ArchViz environment prefilter constants";
        bd.Size = sizeof (DiligentSceneConstants);
        bd.Usage = Diligent::USAGE_DYNAMIC;
        bd.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
        bd.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
        device->CreateBuffer (bd, nullptr, &impl.prefilterConstants);
    }
    if (impl.prefilterConstants == nullptr)
        return "the environment prefilter constant buffer could not be created";

    // ⚠️ LINEAR, WRAP IN U, CLAMP IN V, MIP-LINEAR -- the SAME sampler the scene
    // binds for g_envMap, and for the same reasons (DiligentScene::Init). It is
    // declared again rather than shared because EnvironmentMap must not depend on
    // the scene's construction order; if these two ever disagree, the prefilter
    // integrates a different sky from the one that gets displayed.
    Diligent::SamplerDesc sampler;
    sampler.MinFilter = Diligent::FILTER_TYPE_LINEAR;
    sampler.MagFilter = Diligent::FILTER_TYPE_LINEAR;
    sampler.MipFilter = Diligent::FILTER_TYPE_LINEAR;
    sampler.AddressU = Diligent::TEXTURE_ADDRESS_WRAP;
    sampler.AddressV = Diligent::TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = Diligent::TEXTURE_ADDRESS_CLAMP;
    const Diligent::ImmutableSamplerDesc immutable[] = {
        { Diligent::SHADER_TYPE_PIXEL, "g_envSource_sampler", sampler },
    };

    Diligent::GraphicsPipelineStateCreateInfo pci;
    pci.PSODesc.Name = "ArchViz environment prefilter PSO";
    pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    pci.PSODesc.ResourceLayout.ImmutableSamplers = immutable;
    pci.PSODesc.ResourceLayout.NumImmutableSamplers = _countof (immutable);
    Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
    gp.NumRenderTargets = 1;
    gp.RTVFormats[0] = Diligent::TEX_FORMAT_RGBA16_FLOAT;
    gp.DSVFormat = Diligent::TEX_FORMAT_UNKNOWN;
    gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    gp.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
    gp.DepthStencilDesc.DepthEnable = Diligent::False;
    gp.DepthStencilDesc.DepthWriteEnable = Diligent::False;
    pci.pVS = vs;
    pci.pPS = ps;
    device->CreateGraphicsPipelineState (pci, &impl.prefilterPso);
    if (impl.prefilterPso == nullptr)
        return "the environment prefilter pipeline state could not be created";

    for (Diligent::SHADER_TYPE stage : { Diligent::SHADER_TYPE_VERTEX, Diligent::SHADER_TYPE_PIXEL }) {
        if (Diligent::IShaderResourceVariable* cb =
                impl.prefilterPso->GetStaticVariableByName (stage, "ArchVizConstants"))
            cb->Set (impl.prefilterConstants);
    }
    if (Diligent::IShaderResourceVariable* src =
            impl.prefilterPso->GetStaticVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_envSource"))
        src->Set (impl.sourceSrv);
    else
        return "the environment prefilter shader has no g_envSource variable";

    impl.prefilterPso->CreateShaderResourceBinding (&impl.prefilterSrb, true);
    if (impl.prefilterSrb == nullptr)
        return "the environment prefilter shader resource binding could not be created";

    // One render-target view per mip 1..N-1. ⚠️ MIP 0 IS DELIBERATELY ABSENT.
    // At roughness 0 the GGX lobe is a delta function and the correct
    // prefiltered result IS the source, so mip 0 is copied rather than
    // integrated -- which also keeps the sky BACKGROUND (which samples mip 0)
    // pixel-exact with the file the user chose.
    impl.mipTargets.resize (mipCount);
    for (uint32_t mip = 1; mip < mipCount; ++mip) {
        Diligent::TextureViewDesc vd;
        vd.Name = "ArchViz environment prefilter target";
        vd.ViewType = Diligent::TEXTURE_VIEW_RENDER_TARGET;
        vd.TextureDim = Diligent::RESOURCE_DIM_TEX_2D;
        vd.MostDetailedMip = mip;
        vd.NumMipLevels = 1;
        impl.texture->CreateView (vd, &impl.mipTargets[mip]);
        if (impl.mipTargets[mip] == nullptr) {
            impl.mipTargets.clear ();
            return "an environment prefilter render target view could not be created";
        }
    }
    return std::string ();
}

// Run the prefilter over the whole destination chain. Called only after the
// source has been uploaded and its box mips generated.
//
// ⚠️ IT RESTORES NOTHING, AND ITS CALLER HAS TO. This binds each mip of the
// environment map in turn and leaves the LAST one -- a 1x1 texel -- bound.
// DiligentScene::Draw rebinds the frame's targets immediately after the
// deferred load for exactly this reason; without that, every draw for the rest
// of the frame an HDR arrives would go into that mip, blanking the image and
// corrupting the mip. An earlier version of this comment asserted the caller
// rebound "unconditionally at the top of every frame". It did not, and the
// sibling fault in the ambient-occlusion prepass is what exposed that.
void RunPrefilter (Diligent::IDeviceContext* context, EnvironmentMap::Impl& impl)
{
    const uint32_t mipCount = impl.mipLevels;

    // Mip 0 first: the roughness-0 result, copied rather than integrated.
    Diligent::CopyTextureAttribs copy;
    copy.pSrcTexture = impl.source;
    copy.SrcMipLevel = 0;
    copy.pDstTexture = impl.texture;
    copy.DstMipLevel = 0;
    copy.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    copy.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->CopyTexture (copy);

    context->SetPipelineState (impl.prefilterPso);

    for (uint32_t mip = 1; mip < mipCount; ++mip) {
        Diligent::ITextureView* target = impl.mipTargets[mip];
        if (target == nullptr)
            continue;

        {
            Diligent::MapHelper<DiligentSceneConstants> constants (
                context, impl.prefilterConstants, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD);
            // ⚠️ ROTATION ZERO. The prefilter integrates the sky as STORED; the
            // rotation the user picks is applied at sample time, in EnvUv, on
            // every read. Baking it in here would rotate the reflection twice
            // and make the control non-linear in a way nobody could debug.
            constants->envParams[0] = 1.0f;
            constants->envParams[1] = 0.0f;
            constants->envParams[2] = 1.0f;
            constants->envParams[3] = float (mipCount);

            constants->prefilterParams[0] =
                mipCount > 1 ? float (mip) / float (mipCount - 1) : 0.0f;
            constants->prefilterParams[1] = float (PrefilterSampleCount (mip, mipCount));
            constants->prefilterParams[2] = float (EnvironmentMap::kWidth);
            constants->prefilterParams[3] = float (EnvironmentMap::kHeight);
        }

        Diligent::ITextureView* targets[] = { target };
        context->SetRenderTargets (1, targets, nullptr,
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        // ⚠️ AFTER THE BIND, NOT BEFORE. SetRenderTargets resets the viewport
        // whenever the framebuffer actually changes, so a viewport set first is
        // silently discarded -- and every mip here IS a different framebuffer,
        // at half the previous size. Getting this wrong renders each mip into
        // the top-left quarter of itself and leaves the rest of the level black,
        // which reads as a broken sky at high roughness only.
        Diligent::Viewport viewport;
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = float (std::max<uint32_t> (EnvironmentMap::kWidth >> mip, 1u));
        viewport.Height = float (std::max<uint32_t> (EnvironmentMap::kHeight >> mip, 1u));
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context->SetViewports (1, &viewport, 0, 0);

        context->CommitShaderResources (impl.prefilterSrb,
                                        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Diligent::DrawAttribs draw;
        draw.NumVertices = 3;
        draw.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
        context->Draw (draw);
    }
}

}   // namespace

EnvironmentMap::EnvironmentMap () : impl_ (new Impl ()) {}
EnvironmentMap::~EnvironmentMap ()
{
    Shutdown ();
    delete impl_;
}

bool EnvironmentMap::IsReady () const { return impl_ != nullptr && impl_->ready; }
bool EnvironmentMap::IsLoaded () const { return impl_ != nullptr && impl_->loaded; }
const char* EnvironmentMap::LoadedPath () const
{
    return impl_ != nullptr ? impl_->loadedPath.c_str () : "";
}
Diligent::ITextureView* EnvironmentMap::ShaderView () const
{
    return impl_ != nullptr ? impl_->srv : nullptr;
}
uint32_t EnvironmentMap::MipLevels () const { return impl_ != nullptr ? impl_->mipLevels : 0; }

bool EnvironmentMap::IsPrefiltered () const { return impl_ != nullptr && impl_->prefiltered; }
uint32_t EnvironmentMap::PrefilteredMips () const { return impl_ != nullptr ? impl_->prefilteredMips : 0; }
double EnvironmentMap::PrefilterMilliseconds () const { return impl_ != nullptr ? impl_->prefilterMs : 0.0; }
const char* EnvironmentMap::PrefilterError () const
{
    return impl_ != nullptr ? impl_->prefilterError.c_str () : "";
}

void EnvironmentMap::CopyShCoefficients (float out[9][4]) const
{
    for (int i = 0; i < 9; ++i) {
        out[i][0] = impl_ != nullptr ? impl_->sh.c[i][0] : 0.0f;
        out[i][1] = impl_ != nullptr ? impl_->sh.c[i][1] : 0.0f;
        out[i][2] = impl_ != nullptr ? impl_->sh.c[i][2] : 0.0f;
        out[i][3] = 0.0f;
    }
}

void EnvironmentMap::AverageRadiance (float out[3]) const
{
    for (int c = 0; c < 3; ++c)
        out[c] = impl_ != nullptr ? impl_->average[c] : 0.0f;
}

bool EnvironmentMap::Init (Diligent::IRenderDevice* device, std::string& error)
{
    if (device == nullptr) {
        error = "EnvironmentMap::Init called with a null device";
        return false;
    }
    Shutdown ();

    Diligent::TextureDesc td;
    td.Name = "ArchViz environment map";
    td.Type = Diligent::RESOURCE_DIM_TEX_2D;
    td.Width = kWidth;
    td.Height = kHeight;
    // ⚠️ RGBA16_FLOAT, NOT RGBA32_FLOAT AND NOT AN 8-BIT FORMAT. The whole point
    // of an HDR sky is values above 1.0 -- a sun disc is thousands -- so an 8-bit
    // format would clip exactly the part that produces a highlight. 32-bit float
    // would carry it too, at four times the bandwidth for precision no lighting
    // calculation here can use.
    td.Format = Diligent::TEX_FORMAT_RGBA16_FLOAT;
    // A full chain down to 1x1: the roughness-to-mip mapping wants the top end
    // for polished surfaces and something close to a single average colour for
    // fully rough ones.
    td.MipLevels = 0;   // 0 = as many as the size allows
    // ⚠️ BIND_RENDER_TARGET IS REQUIRED BY GenerateMips, not by any draw. Diligent's
    // D3D11 mip generation goes through the hardware's own path, which needs the
    // texture to be usable as a render target. Without it the texture is created,
    // the upload succeeds, and only mip 0 is ever valid -- so a rough surface
    // samples an undefined mip and the reflection is garbage or black.
    td.BindFlags = Diligent::BIND_SHADER_RESOURCE | Diligent::BIND_RENDER_TARGET;
    td.Usage = Diligent::USAGE_DEFAULT;
    td.MiscFlags = Diligent::MISC_TEXTURE_FLAG_GENERATE_MIPS;

    device->CreateTexture (td, nullptr, &impl_->texture);
    if (impl_->texture == nullptr) {
        error = "Diligent CreateTexture(ArchViz environment map) failed";
        return false;
    }
    impl_->srv = impl_->texture->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    if (impl_->srv == nullptr) {
        error = "the ArchViz environment map has no shader resource view";
        impl_->texture.Release ();
        return false;
    }
    impl_->mipLevels = impl_->texture->GetDesc ().MipLevels;

    // ---- RE51.B6: the private source, and the prefilter pipeline ------------
    //
    // ⚠️ THE SOURCE IS A SECOND ALLOCATION OF THE SAME DESCRIPTOR, and it is
    // allocated even on a device where the prefilter cannot be built. That is
    // deliberate: the upload path then has ONE shape rather than two, and the
    // fallback is a single CopyTexture of the whole chain rather than a second
    // upload path nobody exercises.
    td.Name = "ArchViz environment source (prefilter input)";
    device->CreateTexture (td, nullptr, &impl_->source);
    if (impl_->source == nullptr) {
        error = "Diligent CreateTexture(ArchViz environment source) failed";
        impl_->texture.Release ();
        impl_->srv = nullptr;
        return false;
    }
    impl_->sourceSrv = impl_->source->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    if (impl_->sourceSrv == nullptr) {
        error = "the ArchViz environment source has no shader resource view";
        impl_->source.Release ();
        impl_->texture.Release ();
        impl_->srv = nullptr;
        return false;
    }

    impl_->ready = true;

    // ⚠️ FROM HERE ON A FAILURE IS RECORDED, NOT RETURNED. Everything above is
    // what the renderer cannot run without; everything below is the quality
    // improvement B6 adds. A driver that cannot build this pipeline still gets
    // the box-filtered sky the renderer had before, which is why the map is
    // marked ready first and why none of these branches touch `error`.
    impl_->prefilterInitError = BuildPrefilterPipeline (device, *impl_);
    return true;
}

void EnvironmentMap::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->prefilterSrb.Release ();
    impl_->prefilterPso.Release ();
    impl_->prefilterConstants.Release ();
    impl_->mipTargets.clear ();
    impl_->prefilterInitError.clear ();
    impl_->sourceSrv = nullptr;
    impl_->source.Release ();
    impl_->srv = nullptr;
    impl_->texture.Release ();
    impl_->mipLevels = 0;
    impl_->ready = false;
    Clear ();
}

void EnvironmentMap::Clear ()
{
    if (impl_ == nullptr)
        return;
    impl_->loaded = false;
    impl_->loadedPath.clear ();
    impl_->sh = ShIrradiance {};
    impl_->average[0] = impl_->average[1] = impl_->average[2] = 0.0f;
    impl_->prefiltered = false;
    impl_->prefilteredMips = 0;
    impl_->prefilterMs = 0.0;
    impl_->prefilterError.clear ();
}

bool EnvironmentMap::Load (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                           const char* path, std::string& error)
{
    if (!IsReady () || device == nullptr || context == nullptr) {
        error = "the environment map is not initialised";
        return false;
    }
    if (path == nullptr || *path == '\0') {
        error = "no environment map path was given";
        return false;
    }

    // ⚠️ DILIGENT'S OWN LOADER, NOT A HAND-ROLLED RGBE PARSER. DiligentTools
    // decodes IMAGE_FILE_FORMAT_HDR through stb to VT_FLOAT32 and sniffs the
    // "#?RADIANCE" magic itself, so the Radiance RLE variants are somebody
    // else's problem. It is already linked -- the texture loader comes in with
    // DiligentTools, which the ImGui HUD already required.
    RefCntAutoPtr<Diligent::Image> image;
    Diligent::ImageLoadInfo loadInfo;
    loadInfo.Format = Diligent::IMAGE_FILE_FORMAT_HDR;
    Diligent::CreateImageFromFile (path, &image, nullptr);
    if (image == nullptr) {
        error = "could not read '";
        error += path;
        error += "' as a Radiance .hdr (Diligent's loader supports 32-bit RLE RGBE only)";
        return false;
    }

    const Diligent::ImageDesc& desc = image->GetDesc ();
    if (desc.Width == 0 || desc.Height == 0 || desc.ComponentType != Diligent::VT_FLOAT32) {
        error = "the environment image did not decode to float32 components";
        return false;
    }

    // Repack to tightly-packed RGB floats, which is what EnvironmentLighting
    // speaks. ⚠️ THE ROW STRIDE IS NOT WIDTH * COMPONENTS -- Diligent pads rows,
    // and reading them as if it did not skews the image progressively, which
    // looks like a sheared sky rather than like a stride bug.
    const auto* pixels = static_cast<const float*> (image->GetData ()->GetConstDataPtr ());
    const size_t rowFloats = desc.RowStride / sizeof (float);
    const uint32_t components = desc.NumComponents;

    EquirectImage source;
    source.width = desc.Width;
    source.height = desc.Height;
    source.rgb.resize (size_t (desc.Width) * desc.Height * 3);
    for (uint32_t y = 0; y < desc.Height; ++y) {
        for (uint32_t x = 0; x < desc.Width; ++x) {
            const size_t src = size_t (y) * rowFloats + size_t (x) * components;
            const size_t dst = (size_t (y) * desc.Width + x) * 3;
            source.rgb[dst + 0] = pixels[src + 0];
            source.rgb[dst + 1] = components > 1 ? pixels[src + 1] : pixels[src];
            source.rgb[dst + 2] = components > 2 ? pixels[src + 2] : pixels[src];
        }
    }

    const EquirectImage resampled = Resample (source, kWidth, kHeight);
    if (!resampled.IsValid ()) {
        error = "the environment image could not be resampled";
        return false;
    }

    // ⚠️ THE SH COMES FROM THE FULL-RESOLUTION SOURCE, NOT THE RESAMPLED COPY.
    // The diffuse term is an integral over the whole sky, and integrating the
    // original costs one pass over an image that is already in memory. Using the
    // 512x256 copy would throw away small bright things -- the sun's disc most of
    // all -- from the one quantity they contribute most to.
    ShIrradiance sh = ProjectIrradiance (source);
    float average[3] = {};
    archviz::AverageRadiance (source, average);

    // Widen to RGBA16F. Done on the CPU because the upload wants the texture's
    // own format and there is no blit that would convert it.
    std::vector<uint16_t> texels (size_t (kWidth) * kHeight * 4);
    auto toHalf = [] (float value) -> uint16_t {
        // A float32 -> float16 conversion by bit surgery. ⚠️ NEGATIVES AND
        // INFINITIES ARE NOT EXPECTED but are handled rather than assumed away:
        // an HDR with a NaN texel would otherwise become a NaN in the sky and
        // spread through the SH into every surface.
        if (!(value > 0.0f))
            return 0;
        if (value > 65504.0f)
            value = 65504.0f;
        uint32_t bits;
        std::memcpy (&bits, &value, sizeof (bits));
        const int32_t exponent = int32_t ((bits >> 23) & 0xFF) - 127 + 15;
        if (exponent <= 0)
            return 0;
        if (exponent >= 31)
            return 0x7BFF;
        return uint16_t ((exponent << 10) | ((bits >> 13) & 0x3FF));
    };
    for (size_t i = 0; i < size_t (kWidth) * kHeight; ++i) {
        texels[i * 4 + 0] = toHalf (resampled.rgb[i * 3 + 0]);
        texels[i * 4 + 1] = toHalf (resampled.rgb[i * 3 + 1]);
        texels[i * 4 + 2] = toHalf (resampled.rgb[i * 3 + 2]);
        texels[i * 4 + 3] = 0x3C00;   // 1.0 in half
    }

    Diligent::Box box;
    box.MaxX = kWidth;
    box.MaxY = kHeight;
    Diligent::TextureSubResData sub;
    sub.pData = texels.data ();
    sub.Stride = kWidth * 4 * sizeof (uint16_t);
    // ⚠️ THE UPLOAD GOES TO THE *SOURCE*, NOT TO THE BOUND TEXTURE (RE51.B6).
    // The bound texture's contents are produced entirely by the prefilter below.
    // Writing here instead would put an unfiltered image in mip 0 of the map the
    // model samples -- correct by accident, until the prefilter's own mip 0 copy
    // overwrote it, and wrong in every mip above.
    context->UpdateTexture (impl_->source, 0, 0, box, sub,
                            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    // The BOX chain, which is now an INPUT to the prefilter rather than the
    // thing the model reflects: a wide GGX lobe reads a low mip so that one
    // sample stands for the whole region it represents. Without it the prefilter
    // would sample mip 0 everywhere and produce fireflies at high roughness.
    context->GenerateMips (impl_->sourceSrv);

    const auto prefilterStart = std::chrono::steady_clock::now ();
    if (impl_->prefilterInitError.empty () && impl_->prefilterPso != nullptr) {
        RunPrefilter (context, *impl_);
        impl_->prefiltered = true;
        impl_->prefilteredMips = impl_->mipLevels > 1 ? impl_->mipLevels - 1 : 0;
        impl_->prefilterError.clear ();
    } else {
        // ⚠️ THE FALLBACK IS THE RENDERER AS IT SHIPPED BEFORE B6, not a
        // degraded mode. Copy the whole box-filtered chain across and record
        // WHY, so the probe can say "box-filtered because X" rather than the
        // user being left to wonder why a mirror still smears.
        Diligent::CopyTextureAttribs copy;
        copy.pSrcTexture = impl_->source;
        copy.pDstTexture = impl_->texture;
        copy.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        copy.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        for (uint32_t mip = 0; mip < impl_->mipLevels; ++mip) {
            copy.SrcMipLevel = mip;
            copy.DstMipLevel = mip;
            context->CopyTexture (copy);
        }
        impl_->prefiltered = false;
        impl_->prefilteredMips = 0;
        impl_->prefilterError = impl_->prefilterInitError.empty ()
                                    ? std::string ("the prefilter pipeline was never built")
                                    : impl_->prefilterInitError;
    }
    impl_->prefilterMs = std::chrono::duration<double, std::milli> (
                             std::chrono::steady_clock::now () - prefilterStart)
                             .count ();

    // ⚠️ COMMITTED ONLY NOW. Everything above can fail, and a failed load must
    // leave the previously loaded sky untouched rather than half-replaced --
    // otherwise a typo in a path silently blanks a working environment and the
    // error message scrolls past.
    impl_->sh = sh;
    std::memcpy (impl_->average, average, sizeof (average));
    impl_->loadedPath = path;
    impl_->loaded = true;
    return true;
}

}   // namespace archviz
}   // namespace geomsrv
