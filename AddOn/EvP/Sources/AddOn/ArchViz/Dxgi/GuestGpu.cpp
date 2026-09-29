// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and each
// cost at least one. Everything drawn here is drawn at Present, after the guest's
// BeginDraw, inside the caller's ScopedPipelineState (§2, §11, §12b).
// ArchViz/Dxgi/GuestGpu -- see the header.

#include "ArchViz/Dxgi/GuestGpu.hpp"

#include <GraphicsTypes.h>
#include <InputLayout.h>
#include <Shader.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace guestgpu {

namespace {

using Diligent::LayoutElement;
using Diligent::RefCntAutoPtr;

constexpr Diligent::VALUE_TYPE F32 = Diligent::VT_FLOAT32;
constexpr Diligent::VALUE_TYPE U8 = Diligent::VT_UINT8;
constexpr Diligent::VALUE_TYPE U32 = Diligent::VT_UINT32;
constexpr Diligent::INPUT_ELEMENT_FREQUENCY kVertex = Diligent::INPUT_ELEMENT_FREQUENCY_PER_VERTEX;
constexpr Diligent::INPUT_ELEMENT_FREQUENCY kInstance = Diligent::INPUT_ELEMENT_FREQUENCY_PER_INSTANCE;

// ⚠️ THE LAYOUTS ARE OverlayScene.hpp's STRUCTS, FIELD BY FIELD, ATTRIBn BY n.
// `IsNormalized` defaults to TRUE in Diligent: every float and integer field says
// False out loud, and only the colours are normalised bytes.
LayoutElement At (uint32_t index, uint32_t components, Diligent::VALUE_TYPE type, uint32_t offset, uint32_t stride,
                  Diligent::INPUT_ELEMENT_FREQUENCY frequency)
{
    return LayoutElement { index,
                           0,
                           components,
                           type,
                           type == U8 ? Diligent::True : Diligent::False,
                           offset,
                           stride,
                           frequency,
                           frequency == kInstance ? 1u : 0u };
}

std::vector<LayoutElement> FillLayout (Kind kind)
{
    if (kind == Kind::Plan) {
        const uint32_t s = sizeof (overlayscene::PlanFillVertex);
        return { At (0, 2, F32, 0, s, kVertex), At (1, 2, F32, 8, s, kVertex), At (2, 2, F32, 16, s, kVertex),
                 At (3, 4, U8, 24, s, kVertex), At (4, 1, F32, 28, s, kVertex) };
    }
    const uint32_t s = sizeof (overlayscene::SceneFillVertex);
    return { At (0, 3, F32, 0, s, kVertex), At (1, 3, F32, 12, s, kVertex), At (2, 2, F32, 24, s, kVertex),
             At (3, 4, U8, 32, s, kVertex), At (4, 1, F32, 36, s, kVertex) };
}

std::vector<LayoutElement> LineLayout (Kind kind)
{
    if (kind == Kind::Plan) {
        const uint32_t s = sizeof (overlayscene::PlanLine);
        return { At (0, 2, F32, 0, s, kInstance),  At (1, 2, F32, 8, s, kInstance), At (2, 2, F32, 16, s, kInstance),
                 At (3, 2, F32, 24, s, kInstance), At (4, 4, U8, 32, s, kInstance), At (5, 2, F32, 36, s, kInstance),
                 At (6, 1, U32, 44, s, kInstance) };
    }
    const uint32_t s = sizeof (overlayscene::SceneLine);
    return { At (0, 3, F32, 0, s, kInstance), At (1, 3, F32, 12, s, kInstance), At (2, 4, U8, 24, s, kInstance),
             At (3, 4, U8, 28, s, kInstance), At (4, 3, F32, 32, s, kInstance), At (5, 1, U32, 44, s, kInstance),
             At (6, 1, U32, 48, s, kInstance) };
}

std::vector<LayoutElement> GlyphLayout (Kind kind)
{
    if (kind == Kind::Plan) {
        const uint32_t s = sizeof (overlayscene::PlanGlyph);
        return { At (0, 2, F32, 0, s, kVertex),  At (1, 2, F32, 8, s, kVertex),  At (2, 2, F32, 16, s, kVertex),
                 At (3, 2, F32, 24, s, kVertex), At (4, 2, F32, 32, s, kVertex), At (5, 4, U8, 40, s, kVertex),
                 At (6, 4, U8, 44, s, kVertex),  At (7, 1, F32, 48, s, kVertex), At (8, 1, U32, 52, s, kVertex),
                 At (9, 1, F32, 56, s, kVertex) };
    }
    const uint32_t s = sizeof (overlayscene::SceneGlyph);
    return { At (0, 3, F32, 0, s, kVertex),  At (1, 3, F32, 12, s, kVertex), At (2, 2, F32, 24, s, kVertex),
             At (3, 2, F32, 32, s, kVertex), At (4, 4, U8, 40, s, kVertex),  At (5, 4, U8, 44, s, kVertex),
             At (6, 1, F32, 48, s, kVertex), At (7, 1, U32, 52, s, kVertex), At (8, 1, F32, 56, s, kVertex) };
}

bool Compile (Diligent::IRenderDevice* device, Diligent::SHADER_TYPE type, const char* name, const std::string& source,
              const char* entry, RefCntAutoPtr<Diligent::IShader>& out, std::string& error)
{
    Diligent::ShaderCreateInfo info;
    info.Desc.Name = name;
    info.Desc.ShaderType = type;
    info.EntryPoint = entry;
    info.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
    info.Source = source.c_str ();
    info.SourceLength = source.size ();
    device->CreateShader (info, &out, nullptr);
    if (out == nullptr) {
        error = std::string ("the overlay guest's shader ") + name + " did not compile on Archicad's device";
        return false;
    }
    return true;
}

RefCntAutoPtr<Diligent::IBuffer> DynamicConstants (Diligent::IRenderDevice* device, const char* name, uint32_t bytes)
{
    Diligent::BufferDesc desc;
    desc.Name = name;
    desc.Size = bytes;
    desc.Usage = Diligent::USAGE_DYNAMIC;
    desc.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    desc.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    RefCntAutoPtr<Diligent::IBuffer> buffer;
    device->CreateBuffer (desc, nullptr, &buffer);
    return buffer;
}

RefCntAutoPtr<Diligent::IBuffer> Immutable (Diligent::IRenderDevice* device, const char* name, const void* data,
                                            size_t bytes)
{
    RefCntAutoPtr<Diligent::IBuffer> buffer;
    if (data == nullptr || bytes == 0)
        return buffer;
    Diligent::BufferDesc desc;
    desc.Name = name;
    desc.Size = bytes;
    desc.Usage = Diligent::USAGE_IMMUTABLE;
    desc.BindFlags = Diligent::BIND_VERTEX_BUFFER;
    Diligent::BufferData initial (data, bytes);
    device->CreateBuffer (desc, &initial, &buffer);
    return buffer;
}

void SetStatic (Diligent::IPipelineState* pso, const char* name, Diligent::IDeviceObject* object)
{
    for (const Diligent::SHADER_TYPE stage : { Diligent::SHADER_TYPE_VERTEX, Diligent::SHADER_TYPE_PIXEL })
        if (Diligent::IShaderResourceVariable* variable = pso->GetStaticVariableByName (stage, name))
            variable->Set (object);
}

void Colour (uint32_t rgba, float out[4])
{
    out[0] = float ((rgba >> 24) & 0xFFu) / 255.0f;
    out[1] = float ((rgba >> 16) & 0xFFu) / 255.0f;
    out[2] = float ((rgba >> 8) & 0xFFu) / 255.0f;
    out[3] = float (rgba & 0xFFu) / 255.0f;
}

DrawConstants Constants (float shading, float opacity, float pass, float screen)
{
    DrawConstants c = {};
    c.mode[0] = shading;
    c.mode[1] = opacity;
    c.mode[2] = pass;
    c.mode[3] = screen;
    return c;
}

void Ramp (const overlayscene::FillDraw& draw, float dpiScale, DrawConstants& c)
{
    if (!draw.heatmap || draw.stopCount < 2)
        return;
    c.ramp[0] = draw.min;
    c.ramp[1] = draw.max > draw.min ? 1.0f / (draw.max - draw.min) : 1.0f;
    c.ramp[2] = float (draw.bands);
    c.ramp[3] = float (draw.stopCount);
    for (uint32_t i = 0; i < draw.stopCount && i < 16; ++i) {
        c.stopAt[i] = draw.stopAt[i];
        Colour (draw.stopRgba[i], c.stopColour[i]);
    }
    c.iso[0] = draw.isolineStep;
    c.iso[1] = draw.isolineWidthPixels * 0.5f * dpiScale;
    Colour (draw.isolineRgba, c.isoColour);
}

// The passes an item with this depth policy is drawn in.
uint32_t Passes (uint32_t behind, bool haveDepth, bool screen, bool xray, uint32_t out[2])
{
    if (!haveDepth) {
        out[0] = kAll;
        return 1;
    }
    if (screen || xray || behind == overlayscene::kBehindShow) {
        out[0] = kOver;
        return 1;
    }
    out[0] = kNear;
    if (behind == overlayscene::kBehindHide)
        return 1;
    out[1] = kBehind;
    return 2;
}

// "all" draws with "over"'s state: untested.
uint32_t DepthState (uint32_t pass)
{
    return pass == kAll ? kOver : pass;
}

bool Upload (Diligent::IDeviceContext* context, Diligent::IBuffer* buffer, const void* data, size_t bytes)
{
    Diligent::PVoid mapped = nullptr;
    context->MapBuffer (buffer, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, mapped);
    if (mapped == nullptr)
        return false;
    std::memcpy (mapped, data, bytes);
    context->UnmapBuffer (buffer, Diligent::MAP_WRITE);
    return true;
}

} // namespace

bool Build (Diligent::IRenderDevice* device, Kind kind, const std::string& source, Pipelines& out, std::string& error)
{
    out = Pipelines {};
    out.kind = kind;
    if (device == nullptr) {
        error = "the overlay guest has no device";
        return false;
    }
    RefCntAutoPtr<Diligent::IShader> vsFill, vsLine, vsGlyph, psFill, psLine, psGlyph;
    const Diligent::SHADER_TYPE VS = Diligent::SHADER_TYPE_VERTEX, PS = Diligent::SHADER_TYPE_PIXEL;
    if (!Compile (device, VS, "Tapioca overlay fill VS", source, "VSFill", vsFill, error) ||
        !Compile (device, VS, "Tapioca overlay line VS", source, "VSLine", vsLine, error) ||
        !Compile (device, VS, "Tapioca overlay glyph VS", source, "VSGlyph", vsGlyph, error) ||
        !Compile (device, PS, "Tapioca overlay fill PS", source, "PSFill", psFill, error) ||
        !Compile (device, PS, "Tapioca overlay line PS", source, "PSLine", psLine, error) ||
        !Compile (device, PS, "Tapioca overlay glyph PS", source, "PSGlyph", psGlyph, error))
        return false;

    out.frameBytes = kind == Kind::Plan ? 64u : 16u;
    out.frame = DynamicConstants (device, "Tapioca overlay frame", out.frameBytes);
    out.draw = DynamicConstants (device, "Tapioca overlay draw", sizeof (DrawConstants));
    if (out.frame == nullptr || out.draw == nullptr) {
        error = "the overlay guest's constant buffers could not be created";
        return false;
    }
    const char* const frameName = kind == Kind::Plan ? "GuestPlanFrame" : "GuestSceneFrame";

    const std::vector<LayoutElement> fillLayout = FillLayout (kind);
    const std::vector<LayoutElement> lineLayout = LineLayout (kind);
    const std::vector<LayoutElement> glyphLayout = GlyphLayout (kind);

    enum Blend { Straight, Premultiplied };
    auto build = [&] (const char* name, Diligent::IShader* vs, Diligent::IShader* ps,
                      const std::vector<LayoutElement>& layout, uint32_t depth, bool cullBack, Blend blend, bool glyph,
                      RefCntAutoPtr<Diligent::IPipelineState>& pso) -> bool {
        Diligent::GraphicsPipelineStateCreateInfo info;
        info.PSODesc.Name = name;
        Diligent::GraphicsPipelineDesc& gp = info.GraphicsPipeline;
        // ⚠️ THE FORMATS ARE FOR DILIGENT'S VALIDATION ONLY: a D3D11 pipeline records
        // none, and the target is Archicad's, bound natively (DiligentGuest.hpp).
        gp.NumRenderTargets = 1;
        gp.RTVFormats[0] = Diligent::TEX_FORMAT_BGRA8_UNORM;
        gp.DSVFormat = kind == Kind::Scene ? Diligent::TEX_FORMAT_D32_FLOAT : Diligent::TEX_FORMAT_UNKNOWN;
        gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        gp.RasterizerDesc.CullMode = cullBack ? Diligent::CULL_MODE_BACK : Diligent::CULL_MODE_NONE;
        // A mesh written with outward normals winds counter-clockwise seen from outside.
        gp.RasterizerDesc.FrontCounterClockwise = Diligent::True;
        gp.RasterizerDesc.DepthClipEnable = Diligent::True;
        // ⚠️ TESTED, NEVER WRITTEN: an overlay must not hide another, nor the reference.
        gp.DepthStencilDesc.DepthEnable = depth == kOver ? Diligent::False : Diligent::True;
        gp.DepthStencilDesc.DepthWriteEnable = Diligent::False;
        gp.DepthStencilDesc.DepthFunc =
            depth == kBehind ? Diligent::COMPARISON_FUNC_GREATER : Diligent::COMPARISON_FUNC_LESS_EQUAL;
        gp.InputLayout.LayoutElements = layout.data ();
        gp.InputLayout.NumElements = uint32_t (layout.size ());
        Diligent::RenderTargetBlendDesc& rt = gp.BlendDesc.RenderTargets[0];
        rt.BlendEnable = Diligent::True;
        rt.SrcBlend = blend == Straight ? Diligent::BLEND_FACTOR_SRC_ALPHA : Diligent::BLEND_FACTOR_ONE;
        rt.DestBlend = Diligent::BLEND_FACTOR_INV_SRC_ALPHA;
        rt.BlendOp = Diligent::BLEND_OPERATION_ADD;
        // Archicad's buffer keeps its own alpha, as the raw overlay leaves it.
        rt.SrcBlendAlpha = Diligent::BLEND_FACTOR_ZERO;
        rt.DestBlendAlpha = Diligent::BLEND_FACTOR_ONE;
        rt.BlendOpAlpha = Diligent::BLEND_OPERATION_ADD;
        rt.RenderTargetWriteMask = Diligent::COLOR_MASK_RGB;
        info.pVS = vs;
        info.pPS = ps;
        info.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
        std::vector<Diligent::ShaderResourceVariableDesc> variables;
        if (glyph)
            variables.push_back (
                { Diligent::SHADER_TYPE_PIXEL, "g_atlas", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE });
        if (kind == Kind::Scene) {
            variables.push_back (
                { Diligent::SHADER_TYPE_VERTEX, "ArchicadView", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE });
            variables.push_back ({ Diligent::SHADER_TYPE_VERTEX, "ArchicadProjection",
                                   Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE });
        }
        info.PSODesc.ResourceLayout.Variables = variables.empty () ? nullptr : variables.data ();
        info.PSODesc.ResourceLayout.NumVariables = uint32_t (variables.size ());
        Diligent::SamplerDesc sampler;
        sampler.MinFilter = Diligent::FILTER_TYPE_LINEAR;
        sampler.MagFilter = Diligent::FILTER_TYPE_LINEAR;
        sampler.MipFilter = Diligent::FILTER_TYPE_LINEAR;
        sampler.AddressU = Diligent::TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = Diligent::TEXTURE_ADDRESS_CLAMP;
        const Diligent::ImmutableSamplerDesc samplers[] = {
            { Diligent::SHADER_TYPE_PIXEL, "g_atlas_sampler", sampler },
        };
        if (glyph) {
            info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
            info.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
        }
        device->CreateGraphicsPipelineState (info, &pso);
        if (pso == nullptr) {
            error = std::string ("the overlay guest's pipeline '") + name + "' could not be created";
            return false;
        }
        SetStatic (pso, frameName, out.frame);
        SetStatic (pso, "GuestDraw", out.draw);
        return true;
    };

    const uint32_t first = kind == Kind::Plan ? kOver : kNear;
    for (uint32_t depth = first; depth < kDepthModes; ++depth) {
        for (int cull = 0; cull < 2; ++cull)
            if (!build ("Tapioca overlay fill", vsFill, psFill, fillLayout, depth, cull != 0, Straight, false,
                        out.fill[depth][cull]))
                return false;
        if (!build ("Tapioca overlay line", vsLine, psLine, lineLayout, depth, false, Straight, false,
                    out.line[depth]) ||
            !build ("Tapioca overlay glyph", vsGlyph, psGlyph, glyphLayout, depth, false, Premultiplied, true,
                    out.glyph[depth]))
            return false;
    }
    // ⚠️ ONE BINDING SERVES EVERY DEPTH VARIANT: they are one shader pair and one
    // resource layout with different fixed-function state, which Diligent's
    // compatibility rule is about.
    out.fill[kOver][0]->CreateShaderResourceBinding (&out.fillSrb, true);
    out.line[kOver]->CreateShaderResourceBinding (&out.lineSrb, true);
    if (out.fillSrb == nullptr || out.lineSrb == nullptr) {
        error = "the overlay guest's shader bindings could not be created";
        return false;
    }
    out.ready = true;
    return true;
}

bool Upload (Diligent::IRenderDevice* device, const Pipelines& pipelines, std::vector<Page>& pages,
             const Arrays& arrays, const std::vector<overlayscene::FillDraw>& fillDraws,
             const std::vector<overlayscene::GlyphDraw>& glyphDraws,
             const std::vector<std::shared_ptr<const overlaytext::Page>>& atlas, Content& out, std::string& error)
{
    out = Content {};
    out.fillStride = uint32_t (arrays.fillStride);
    out.lineStride = uint32_t (arrays.lineStride);
    out.glyphStride = uint32_t (arrays.glyphStride);
    out.fills = Immutable (device, "Tapioca overlay fills", arrays.fills, arrays.fillCount * arrays.fillStride);
    out.lines = Immutable (device, "Tapioca overlay lines", arrays.lines, arrays.lineCount * arrays.lineStride);
    out.glyphs = Immutable (device, "Tapioca overlay glyphs", arrays.glyphs, arrays.glyphCount * arrays.glyphStride);
    if ((arrays.fillCount > 0 && out.fills == nullptr) || (arrays.lineCount > 0 && out.lines == nullptr) ||
        (arrays.glyphCount > 0 && out.glyphs == nullptr)) {
        error = "the overlay guest's vertex buffers could not be created";
        out = Content {};
        return false;
    }
    out.lineCount = uint32_t (arrays.lineCount);
    out.fillDraws = fillDraws;
    out.glyphDraws = glyphDraws;
    if (arrays.dashes != nullptr)
        out.dashes.assign (
            arrays.dashes->begin (),
            arrays.dashes->begin () +
                std::ptrdiff_t ((std::min) (arrays.dashes->size (), sizeof (DrawConstants::dashes) / sizeof (float))));

    // ⚠️ THE CACHE KEEPS ONLY WHAT THIS CONTENT SAMPLES. A HUD panel's font atlas is a
    // new page every time ImGui grows it (OverlayHud.hpp), and a cache that only ever
    // added would hold every version for the session. A text page dropped here is
    // uploaded again the day a label needs it.
    pages.erase (std::remove_if (pages.begin (), pages.end (),
                                 [&atlas] (const Page& cached) {
                                     return std::none_of (atlas.begin (), atlas.end (), [&cached] (const auto& wanted) {
                                         return wanted != nullptr && wanted->id == cached.id;
                                     });
                                 }),
                 pages.end ());

    // The pages this content samples, uploaded once each; a page that fails is left
    // out, and so are the draws that need it -- counted by the caller as missing text.
    out.pageSlot.assign (atlas.size (), UINT32_MAX);
    for (size_t index = 0; index < atlas.size (); ++index) {
        if (atlas[index] == nullptr)
            continue;
        const overlaytext::Page& page = *atlas[index];
        size_t slot = 0;
        while (slot < pages.size () && pages[slot].id != page.id)
            ++slot;
        if (slot == pages.size ()) {
            Page uploaded;
            uploaded.id = page.id;
            Diligent::TextureDesc desc;
            desc.Name = "Tapioca overlay atlas page";
            desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
            desc.Width = uint32_t (page.width);
            desc.Height = uint32_t (page.height);
            // ⚠️ LINEAR DATA, NEVER AN sRGB VIEW: the channels are distances.
            desc.Format = Diligent::TEX_FORMAT_RGBA8_UNORM;
            desc.Usage = Diligent::USAGE_IMMUTABLE;
            desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
            Diligent::TextureSubResData level;
            level.pData = page.pixels.data ();
            level.Stride = Diligent::Uint64 (page.width) * 4u;
            Diligent::TextureData initial;
            initial.pSubResources = &level;
            initial.NumSubresources = 1;
            device->CreateTexture (desc, &initial, &uploaded.texture);
            if (uploaded.texture == nullptr)
                continue;
            pipelines.glyph[kOver]->CreateShaderResourceBinding (&uploaded.srb, true);
            Diligent::IShaderResourceVariable* variable =
                uploaded.srb != nullptr ? uploaded.srb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_atlas")
                                        : nullptr;
            if (variable == nullptr)
                continue;
            variable->Set (uploaded.texture->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
            uploaded.invWidth = 1.0f / float (page.width);
            uploaded.invHeight = 1.0f / float (page.height);
            pages.push_back (std::move (uploaded));
        }
        out.pageSlot[index] = uint32_t (slot);
    }
    return true;
}

void BindCamera (Pipelines& pipelines, std::vector<Page>& pages, Diligent::IBuffer* view, Diligent::IBuffer* projection)
{
    auto bind = [&] (Diligent::IShaderResourceBinding* srb) {
        if (srb == nullptr)
            return;
        if (auto* v = srb->GetVariableByName (Diligent::SHADER_TYPE_VERTEX, "ArchicadView"))
            v->Set (view, Diligent::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
        if (auto* v = srb->GetVariableByName (Diligent::SHADER_TYPE_VERTEX, "ArchicadProjection"))
            v->Set (projection, Diligent::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    };
    bind (pipelines.fillSrb);
    bind (pipelines.lineSrb);
    for (Page& page : pages)
        bind (page.srb);
}

void Draw (Diligent::IDeviceContext* context, const Pipelines& pipelines, const std::vector<Page>& pages,
           const Content& content, const void* frame, bool haveDepth, float dpiScale, DrawStats& stats)
{
    if (!pipelines.ready || context == nullptr || content.Empty ())
        return;
    if (!Upload (context, pipelines.frame, frame, pipelines.frameBytes)) {
        ++stats.mapFailures;
        return;
    }
    const Diligent::Uint64 offset = 0;
    auto setVertices = [&] (Diligent::IBuffer* buffer) {
        Diligent::IBuffer* buffers[] = { buffer };
        context->SetVertexBuffers (0, 1, buffers, &offset, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                   Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
    };
    auto constants = [&] (const DrawConstants& values) {
        if (Upload (context, pipelines.draw, &values, sizeof (values)))
            return true;
        ++stats.mapFailures;
        return false;
    };

    if (content.fills != nullptr) {
        setVertices (content.fills);
        for (const overlayscene::FillDraw& draw : content.fillDraws) {
            uint32_t passes[2] = {};
            const bool xray = draw.shading == 3u;
            const uint32_t count = Passes (draw.behind, haveDepth, draw.screen, xray, passes);
            for (uint32_t p = 0; p < count; ++p) {
                DrawConstants values =
                    Constants (float (draw.shading), draw.opacity, float (passes[p]), draw.screen ? 1.0f : 0.0f);
                Ramp (draw, dpiScale, values);
                if (!constants (values))
                    return;
                context->SetPipelineState (pipelines.fill[DepthState (passes[p])][draw.cullBack ? 1 : 0]);
                context->CommitShaderResources (pipelines.fillSrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                Diligent::DrawAttribs attribs (draw.count, Diligent::DRAW_FLAG_NONE, 1, draw.first);
                context->Draw (attribs);
                ++stats.drawCalls;
            }
        }
    }

    if (content.lines != nullptr && content.lineCount > 0) {
        setVertices (content.lines);
        // Every line in every pass; the vertex shader keeps the ones the pass is for.
        const uint32_t plan[] = { kOver };
        const uint32_t scene[] = { kNear, kBehind, kOver };
        const uint32_t all[] = { kAll };
        const uint32_t* passes = pipelines.kind == Kind::Plan ? plan : (haveDepth ? scene : all);
        const uint32_t count = pipelines.kind == Kind::Plan || !haveDepth ? 1u : 3u;
        for (uint32_t p = 0; p < count; ++p) {
            DrawConstants values = Constants (0.0f, 1.0f, float (passes[p]), 0.0f);
            std::memcpy (values.dashes, content.dashes.data (), content.dashes.size () * sizeof (float));
            if (!constants (values))
                return;
            context->SetPipelineState (pipelines.line[DepthState (passes[p])]);
            context->CommitShaderResources (pipelines.lineSrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            Diligent::DrawAttribs attribs (6, Diligent::DRAW_FLAG_NONE, content.lineCount, 0);
            context->Draw (attribs);
            ++stats.drawCalls;
        }
    }

    if (content.glyphs != nullptr) {
        setVertices (content.glyphs);
        for (const overlayscene::GlyphDraw& draw : content.glyphDraws) {
            if (draw.page >= content.pageSlot.size () || content.pageSlot[draw.page] >= pages.size ())
                continue;
            const Page& page = pages[content.pageSlot[draw.page]];
            uint32_t passes[2] = {};
            const uint32_t count = Passes (draw.behind, haveDepth, false, false, passes);
            for (uint32_t p = 0; p < count; ++p) {
                DrawConstants values = Constants (0.0f, 1.0f, float (passes[p]), 0.0f);
                values.atlas[0] = page.invWidth;
                values.atlas[1] = page.invHeight;
                values.atlas[2] = overlaytext::Engine::DistanceRangePixels ();
                values.atlas[3] = overlaytext::Engine::EmPixels ();
                if (!constants (values))
                    return;
                context->SetPipelineState (pipelines.glyph[DepthState (passes[p])]);
                context->CommitShaderResources (page.srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                Diligent::DrawAttribs attribs (draw.count, Diligent::DRAW_FLAG_NONE, 1, draw.first);
                context->Draw (attribs);
                ++stats.drawCalls;
            }
        }
    }
}

} // namespace guestgpu
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
