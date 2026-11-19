#ifndef EVP_ARCHVIZ_MATERIALTEXTURE_HPP
#define EVP_ARCHVIZ_MATERIALTEXTURE_HPP

// ArchViz/MaterialTexture -- an Archicad surface's texture image, as the viewer holds it (#252).
//
// The producer (ExtractionEnvironment, main thread) reads ModelerAPI::Texture::GetPixelMap once per
// image, keyed by Archicad's fingerprint, converts it to RGBA8 and fits it to kMaxTextureSide; the
// MaterialTable carries the result to the render thread by shared pointer, so a live sync that
// re-reads the materials never re-reads or copies an image it already holds.
//
// ⚠️ THE BYTES ARE sRGB-ENCODED, AS THE IMAGE FILE HAS THEM. The GPU texture is created with an
// sRGB format and the hardware decodes on sampling; decoding here would band the darks in 8 bits.
//
// PURE: no ACAPI, no Diligent -- the offline suite tests the fit and the key directly.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {

// The largest side a texture is uploaded at. Archicad's library images reach 4K; at viewer
// distances a 2K image with mips is indistinguishable and a quarter of the memory.
constexpr uint32_t kMaxTextureSide = 2048;

struct TexturePixels {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba; // width * height * 4, row 0 first as the producer stores it
    bool hasAlpha = false;     // any pixel below 255 alpha
    std::string name;

    size_t Bytes () const
    {
        return sizeof (TexturePixels) + rgba.capacity () + name.capacity ();
    }
    bool Valid () const
    {
        return width > 0 && height > 0 && rgba.size () == size_t (width) * height * 4;
    }
};

using TexturePixelsPtr = std::shared_ptr<const TexturePixels>;

// Halves the image with a 2x2 box filter until both sides fit `maxSide` (an odd side keeps its last
// row or column). Alpha-weighted is not needed: the viewer's textures are opaque colour.
void FitTexturePixels (TexturePixels& pixels, uint32_t maxSide);

// The cache and GPU key of an image: FNV-1a of Archicad's fingerprint (or of the file path, for
// RenderBench). Never 0, which SurfaceMaterial::textureKey reserves for "no texture".
uint64_t TextureKeyOf (const std::string& identity);

} // namespace archviz
} // namespace geomsrv

#endif
