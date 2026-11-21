// The shaded mesh pipelines' resources -- their samplers, their mutable variables and their vertex
// layout -- and the Archicad material textures they sample (#252).
//
// ⚠️ ITS OWN TRANSLATION UNIT BECAUSE DiligentScene.cpp IS FROZEN IN SIZE (tools/quality/
// check_cpp.py). Init built these tables inline; #252 adds a texture, its sampler and a second
// vertex stream to them, so the tables moved here whole and Init reads them from MeshResources ().

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/ClayLook.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {

namespace {

MeshPipelineResources BuildMeshResources ()
{
    MeshPipelineResources r;

    // The PCF representation is a comparison texture; VSM/EVSM are ordinary
    // filterable arrays. Their sampler types are part of the shader signature.
    r.shadow.MinFilter = Diligent::FILTER_TYPE_COMPARISON_LINEAR;
    r.shadow.MagFilter = Diligent::FILTER_TYPE_COMPARISON_LINEAR;
    r.shadow.MipFilter = Diligent::FILTER_TYPE_COMPARISON_LINEAR;
    r.shadow.AddressU = Diligent::TEXTURE_ADDRESS_CLAMP;
    r.shadow.AddressV = Diligent::TEXTURE_ADDRESS_CLAMP;
    r.shadow.AddressW = Diligent::TEXTURE_ADDRESS_CLAMP;
    r.shadow.ComparisonFunc = Diligent::COMPARISON_FUNC_LESS;

    r.filterableShadow.MinFilter = Diligent::FILTER_TYPE_ANISOTROPIC;
    r.filterableShadow.MagFilter = Diligent::FILTER_TYPE_ANISOTROPIC;
    r.filterableShadow.MipFilter = Diligent::FILTER_TYPE_ANISOTROPIC;
    r.filterableShadow.AddressU = Diligent::TEXTURE_ADDRESS_CLAMP;
    r.filterableShadow.AddressV = Diligent::TEXTURE_ADDRESS_CLAMP;
    r.filterableShadow.AddressW = Diligent::TEXTURE_ADDRESS_CLAMP;
    r.filterableShadow.MaxAnisotropy = 4;

    // The environment's sampler is a DIFFERENT one, and every field differs for
    // a reason. ⚠️ U WRAPS AND V CLAMPS, WHICH IS NOT A DETAIL: an
    // equirectangular map is periodic in longitude (u) and bounded in latitude
    // (v), so wrapping v mirrors the sky through the poles and clamping u puts a
    // visible seam of stretched pixels down one side of every reflection.
    // Linear + mip-linear because the roughness blur IS the mip selection --
    // point mip filtering would make a surface's reflection jump between blur
    // levels as its roughness or its angle changes.
    r.env.MinFilter = Diligent::FILTER_TYPE_LINEAR;
    r.env.MagFilter = Diligent::FILTER_TYPE_LINEAR;
    r.env.MipFilter = Diligent::FILTER_TYPE_LINEAR;
    r.env.AddressU = Diligent::TEXTURE_ADDRESS_WRAP;
    r.env.AddressV = Diligent::TEXTURE_ADDRESS_CLAMP;
    r.env.AddressW = Diligent::TEXTURE_ADDRESS_CLAMP;

    // #252: a surface texture repeats both ways; anisotropic, because architecture is seen at
    // grazing angles (floors, long walls) where a trilinear tile blurs into a smear.
    r.albedo.MinFilter = Diligent::FILTER_TYPE_ANISOTROPIC;
    r.albedo.MagFilter = Diligent::FILTER_TYPE_ANISOTROPIC;
    r.albedo.MipFilter = Diligent::FILTER_TYPE_ANISOTROPIC;
    r.albedo.AddressU = Diligent::TEXTURE_ADDRESS_WRAP;
    r.albedo.AddressV = Diligent::TEXTURE_ADDRESS_WRAP;
    r.albedo.AddressW = Diligent::TEXTURE_ADDRESS_WRAP;
    r.albedo.MaxAnisotropy = 8;

    r.samplers[0][0] = { Diligent::SHADER_TYPE_PIXEL, "g_shadowMap_sampler", r.shadow };
    r.samplers[1][0] = { Diligent::SHADER_TYPE_PIXEL, "g_filterableShadowMap_sampler", r.filterableShadow };
    for (int mode = 0; mode < 2; ++mode) {
        r.samplers[mode][1] = { Diligent::SHADER_TYPE_PIXEL, "g_envMap_sampler", r.env };
        r.samplers[mode][2] = { Diligent::SHADER_TYPE_PIXEL, "g_albedoMap_sampler", r.albedo };
    }

    // See the ⚠️ at the mesh PSO's resource layout for why these are not STATIC like everything
    // else the mesh shader reads. #252: the albedo map changes per draw range.
    for (int mode = 0; mode < 2; ++mode) {
        r.variables[mode][0] = { Diligent::SHADER_TYPE_PIXEL, "g_ambientOcclusion",
                                 Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC };
        r.variables[mode][1] = { Diligent::SHADER_TYPE_PIXEL, mode == 0 ? "g_shadowMap" : "g_filterableShadowMap",
                                 Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC };
        r.variables[mode][2] = { Diligent::SHADER_TYPE_PIXEL, "g_albedoMap",
                                 Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC };
    }

    // ⚠️ THIS LAYOUT AND ArchVizVertex ARE ONE CONTRACT (see Init's `layout`), plus #252's texture
    // coordinate in its own buffer at slot 1 (Entry::uvBuffer), which every entry has.
    r.texturedLayout[0] = Diligent::LayoutElement { 0, 0, 3, Diligent::VT_FLOAT32, Diligent::False };
    r.texturedLayout[1] = Diligent::LayoutElement { 1, 0, 3, Diligent::VT_FLOAT32, Diligent::False };
    r.texturedLayout[2] = Diligent::LayoutElement { 2, 0, 4, Diligent::VT_UINT8, Diligent::True };
    r.texturedLayout[3] = Diligent::LayoutElement { 3, 1, 2, Diligent::VT_FLOAT32, Diligent::False };
    return r;
}

// The mip chain of an sRGB RGBA8 image, top level first, each half the one before (box filter).
// Filtered in the encoded space: a little darker than a linear filter at the smallest mips, and
// nothing a viewer at architectural distances can see.
std::vector<std::vector<uint8_t>> BuildMips (const TexturePixels& image)
{
    std::vector<std::vector<uint8_t>> levels;
    levels.push_back (image.rgba);
    TexturePixels level = image;
    while (level.width > 1 || level.height > 1) {
        const uint32_t side = (std::max) (level.width, level.height) / 2;
        FitTexturePixels (level, (std::max) (side, 1u));
        levels.push_back (level.rgba);
    }
    return levels;
}

} // namespace

const MeshPipelineResources& MeshResources ()
{
    static const MeshPipelineResources resources = BuildMeshResources ();
    return resources;
}

// #252: the GPU copies of the table's images. Uploads the ones not yet held, drops the ones no
// longer named. Called whenever a material table lands (SetMaterials).
void DiligentScene::SyncAlbedoTextures (Diligent::IRenderDevice* device)
{
    if (impl_ == nullptr || device == nullptr)
        return;
    const auto& wanted = impl_->materials.Textures ();
    for (auto it = impl_->albedoTextures.begin (); it != impl_->albedoTextures.end ();)
        it = wanted.count (it->first) == 0 ? impl_->albedoTextures.erase (it) : std::next (it);

    if (impl_->albedoFallback == nullptr) {
        const uint8_t white[4] = { 255, 255, 255, 255 };
        Diligent::TextureDesc desc;
        desc.Name = "ArchViz albedo fallback";
        desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
        desc.Width = desc.Height = 1;
        desc.Format = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
        desc.Usage = Diligent::USAGE_IMMUTABLE;
        desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
        Diligent::TextureSubResData sub { white, 4 };
        Diligent::TextureData data { &sub, 1 };
        device->CreateTexture (desc, &data, &impl_->albedoFallback);
    }

    for (const auto& [key, image] : wanted) {
        if (image == nullptr || !image->Valid () || impl_->albedoTextures.count (key) != 0)
            continue;
        const std::vector<std::vector<uint8_t>> mips = BuildMips (*image);
        std::vector<Diligent::TextureSubResData> subresources;
        uint32_t w = image->width, h = image->height;
        for (const std::vector<uint8_t>& level : mips) {
            subresources.push_back ({ level.data (), Diligent::Uint64 (w) * 4 });
            w = (std::max) (w / 2, 1u);
            h = (std::max) (h / 2, 1u);
        }
        Diligent::TextureDesc desc;
        desc.Name = "ArchViz material texture";
        desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
        desc.Width = image->width;
        desc.Height = image->height;
        desc.MipLevels = Diligent::Uint32 (mips.size ());
        desc.Format = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
        desc.Usage = Diligent::USAGE_IMMUTABLE;
        desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
        Diligent::TextureData data { subresources.data (), Diligent::Uint32 (subresources.size ()) };
        RefCntAutoPtr<Diligent::ITexture> texture;
        device->CreateTexture (desc, &data, &texture);
        if (texture == nullptr) {
            ArchVizLog ("Diligent textures: could not create '" + image->name + "' -- it draws its surface colour");
            continue;
        }
        impl_->albedoTextures.emplace (key, texture);
    }

    // #252 diagnostic: what the table names against what the GPU now holds. textured > 0 with
    // images = 0 is a table that lost its pixels on the way here.
    size_t textured = 0;
    for (const SurfaceMaterial& m : impl_->materials.All ())
        textured += m.textureKey != 0 ? 1 : 0;
    ArchVizLog ("Diligent textures: table materials=" + std::to_string (impl_->materials.Size ()) +
                " textured=" + std::to_string (textured) + " images=" + std::to_string (wanted.size ()) +
                " gpuImages=" + std::to_string (impl_->albedoTextures.size ()));
}

// #252: the texture lane for one draw range, into `constants`, and the view Draw binds with it.
// Textures show in the Realistic quality only (Rendered, Ray traced), with the HUD toggle on, and
// never under clay or the study model, whose whole point is to drop the surfaces' own colour.
Diligent::ITextureView* DiligentScene::Impl::AlbedoFor (const SurfaceMaterial& material, const ClayLook& look,
                                                        DiligentSceneConstants& constants) const
{
    constants.textureParams[0] = 0.0f;
    Diligent::ITextureView* fallback =
        albedoFallback != nullptr ? albedoFallback->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
    if (material.textureKey == 0)
        return fallback;
    if (!TexturesShown (look)) {
        ++textureLane[2];
        return fallback;
    }
    const auto found = albedoTextures.find (material.textureKey);
    if (found == albedoTextures.end ()) {
        ++textureLane[1];
        return fallback;
    }
    ++textureLane[0];
    constants.textureParams[0] = 1.0f;
    constants.textureParams[1] = 1.0f / (std::max) (material.textureSizeX, 1e-6f);
    constants.textureParams[2] = 1.0f / (std::max) (material.textureSizeY, 1e-6f);
    constants.textureParams[3] = float ((material.textureMirrorX ? 1 : 0) | (material.textureMirrorY ? 2 : 0));
    return found->second->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
}

// #252 diagnostic: closes the previous frame's count. Logged when it changes, so a live run
// says in one line whether textured ranges drew, named an image the GPU lost, or were gated.
void DiligentScene::Impl::NoteTextureLane ()
{
    if (!idleLastFrame)
        std::copy (std::begin (textureLane), std::end (textureLane), std::begin (textureLaneDrawn));
    std::fill (std::begin (textureLane), std::end (textureLane), 0u);
    if (std::equal (std::begin (textureLaneDrawn), std::end (textureLaneDrawn), std::begin (textureLaneLogged)))
        return;
    std::copy (std::begin (textureLaneDrawn), std::end (textureLaneDrawn), std::begin (textureLaneLogged));
    ArchVizLog ("Diligent textures: drawn ranges textured=" + std::to_string (textureLaneDrawn[0]) + " missingImage=" +
                std::to_string (textureLaneDrawn[1]) + " gated=" + std::to_string (textureLaneDrawn[2]) +
                " (gpuImages=" + std::to_string (albedoTextures.size ()) +
                " toggle=" + std::to_string (materialTexturesEnabled ? 1 : 0) +
                " realistic=" + std::to_string (renderQuality == RenderQuality::Realistic ? 1 : 0) + ")");
}

// #252 diagnostic: the coordinate range of the first textured elements, against the material's
// repeat size in the log's "read" line. Spans of a few units mean metres; spans near 0..1 mean
// the coordinates arrived already divided by the repeat.
void LogUvRange (const std::string& guid, const std::vector<float>& vertices, const std::vector<uint32_t>& indices,
                 const std::vector<float>& uvs, uint32_t& logged)
{
    if (uvs.size () < 2 || logged >= 24)
        return;
    ++logged;
    float lo[2] = { uvs[0], uvs[1] }, hi[2] = { uvs[0], uvs[1] };
    for (size_t i = 0; i + 1 < uvs.size (); i += 2)
        for (size_t k = 0; k < 2; ++k) {
            lo[k] = (std::min) (lo[k], uvs[i + k]);
            hi[k] = (std::max) (hi[k], uvs[i + k]);
        }
    ArchVizLog ("Diligent textures: uv " + guid + " vertices=" + std::to_string (uvs.size () / 2) + " u=[" +
                std::to_string (lo[0]) + ", " + std::to_string (hi[0]) + "] v=[" + std::to_string (lo[1]) + ", " +
                std::to_string (hi[1]) + "]");

    // Per triangle, as uploaded: Archicad maps a face evenly (Material Texture Check, stage 6),
    // so a squeezed triangle here means the coordinates were paired with the wrong corners on
    // the way. Triangles whose corners all sit at (0, 0) are untextured and skipped.
    const size_t vertexCount = (std::min) (vertices.size () / 3, uvs.size () / 2);
    size_t measured = 0, squeezed = 0;
    std::vector<std::pair<double, size_t>> worst;
    for (size_t t = 0; t + 2 < indices.size (); t += 3) {
        const uint32_t a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount)
            continue;
        const float* ta = &uvs[size_t (a) * 2];
        const float* tb = &uvs[size_t (b) * 2];
        const float* tc = &uvs[size_t (c) * 2];
        if (ta[0] == 0.0f && ta[1] == 0.0f && tb[0] == 0.0f && tb[1] == 0.0f && tc[0] == 0.0f && tc[1] == 0.0f)
            continue;
        const UvStretch s = TriangleUvStretch (&vertices[size_t (a) * 3], &vertices[size_t (b) * 3],
                                               &vertices[size_t (c) * 3], ta, tb, tc);
        if (s.maxPerUnit <= 0.0)
            continue;
        ++measured;
        const double ratio = s.maxPerUnit / (std::max) (s.minPerUnit, 1e-12);
        if (ratio > 1.5) {
            ++squeezed;
            worst.emplace_back (ratio, t);
        }
    }
    ArchVizLog ("Diligent textures: uv " + guid + " triangles=" + std::to_string (measured) +
                " squeezed=" + std::to_string (squeezed) + " (stretch > 1.5x)");
    std::sort (worst.begin (), worst.end (), [] (const auto& x, const auto& y) { return x.first > y.first; });
    for (size_t k = 0; k < worst.size () && k < 3; ++k) {
        std::string corners;
        for (size_t j = 0; j < 3; ++j) {
            const uint32_t v = indices[worst[k].second + j];
            corners += " #" + std::to_string (v) + " p=(" + std::to_string (vertices[v * 3]) + "," +
                       std::to_string (vertices[v * 3 + 1]) + "," + std::to_string (vertices[v * 3 + 2]) + ") uv=(" +
                       std::to_string (uvs[v * 2]) + "," + std::to_string (uvs[v * 2 + 1]) + ")";
        }
        ArchVizLog ("Diligent textures: uv " + guid + " squeezed " + std::to_string (worst[k].first) + "x:" + corners);
    }
}

void DiligentScene::SetMaterialTextures (bool enabled)
{
    if (impl_ != nullptr)
        impl_->materialTexturesEnabled = enabled;
}

// #252: every element gets a texture-coordinate buffer (slot 1), so the shaded pipelines can
// always declare it: Archicad's coordinates where the element has them, zeros otherwise.
bool CreateUvBuffer (Diligent::IRenderDevice* device, Entry& entry, const std::vector<float>& uvs, uint32_t vertexCount)
{
    entry.uvBuffer.Release ();
    std::vector<float> zeros;
    const bool have = uvs.size () == size_t (vertexCount) * 2;
    if (!have)
        zeros.assign (size_t (vertexCount) * 2, 0.0f);
    const std::vector<float>& data = have ? uvs : zeros;
    if (data.empty ())
        return false;
    Diligent::BufferDesc desc;
    desc.Name = "ArchViz texture coordinates";
    desc.Size = data.size () * sizeof (float);
    desc.BindFlags = Diligent::BIND_VERTEX_BUFFER;
    desc.Usage = Diligent::USAGE_IMMUTABLE;
    const Diligent::BufferData init { data.data (), desc.Size };
    device->CreateBuffer (desc, &init, &entry.uvBuffer);
    if (entry.uvBuffer != nullptr)
        entry.gpuBytes += desc.Size;
    return entry.uvBuffer != nullptr;
}

} // namespace archviz
} // namespace geomsrv
