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
    if (!materialTexturesEnabled || look.enabled || renderQuality != RenderQuality::Realistic ||
        material.textureKey == 0)
        return fallback;
    const auto found = albedoTextures.find (material.textureKey);
    if (found == albedoTextures.end ())
        return fallback;
    constants.textureParams[0] = 1.0f;
    constants.textureParams[1] = 1.0f / (std::max) (material.textureSizeX, 1e-6f);
    constants.textureParams[2] = 1.0f / (std::max) (material.textureSizeY, 1e-6f);
    constants.textureParams[3] = float ((material.textureMirrorX ? 1 : 0) | (material.textureMirrorY ? 2 : 0));
    return found->second->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
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
