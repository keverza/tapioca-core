#include "ArchViz/EnvironmentMap.hpp"

#include "ArchViz/EnvironmentLighting.hpp"

#include <windows.h>
#include <d3d11.h>   // Must precede any Diligent D3D11 interop header (Probe 1a).
#include <DeviceContext.h>
#include <GraphicsTypes.h>
#include <Image.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDevice.h>
#include <Texture.h>
#include <TextureView.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace geomsrv {
namespace archviz {

using Diligent::RefCntAutoPtr;

struct EnvironmentMap::Impl {
    RefCntAutoPtr<Diligent::ITexture> texture;
    Diligent::ITextureView* srv = nullptr;   // owned by `texture`
    uint32_t mipLevels = 0;
    bool ready = false;

    bool loaded = false;
    std::string loadedPath;
    ShIrradiance sh;
    float average[3] = {0.0f, 0.0f, 0.0f};
};

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
    impl_->ready = true;
    return true;
}

void EnvironmentMap::Shutdown ()
{
    if (impl_ == nullptr)
        return;
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
    context->UpdateTexture (impl_->texture, 0, 0, box, sub,
                            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->GenerateMips (impl_->srv);

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
