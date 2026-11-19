#include "ArchViz/MaterialTexture.hpp"

namespace geomsrv {
namespace archviz {

void FitTexturePixels (TexturePixels& pixels, uint32_t maxSide)
{
    if (!pixels.Valid () || maxSide == 0)
        return;
    while (pixels.width > maxSide || pixels.height > maxSide) {
        const uint32_t w = pixels.width > 1 ? pixels.width / 2 : 1;
        const uint32_t h = pixels.height > 1 ? pixels.height / 2 : 1;
        std::vector<uint8_t> out (size_t (w) * h * 4);
        for (uint32_t y = 0; y < h; ++y) {
            const uint32_t y0 = pixels.height > 1 ? y * 2 : 0;
            const uint32_t y1 = pixels.height > 1 ? y0 + 1 : 0;
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t x0 = pixels.width > 1 ? x * 2 : 0;
                const uint32_t x1 = pixels.width > 1 ? x0 + 1 : 0;
                for (int c = 0; c < 4; ++c) {
                    const auto at = [&] (uint32_t px, uint32_t py) {
                        return uint32_t (pixels.rgba[(size_t (py) * pixels.width + px) * 4 + c]);
                    };
                    out[(size_t (y) * w + x) * 4 + c] =
                        uint8_t ((at (x0, y0) + at (x1, y0) + at (x0, y1) + at (x1, y1) + 2) / 4);
                }
            }
        }
        pixels.width = w;
        pixels.height = h;
        pixels.rgba = std::move (out);
    }
}

uint64_t TextureKeyOf (const std::string& identity)
{
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : identity) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
}

} // namespace archviz
} // namespace geomsrv
