#ifndef EVP_SUNSTUDY_TEXTUREDELTA_HPP
#define EVP_SUNSTUDY_TEXTUREDELTA_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>
#include <utility>
#include <functional>

namespace evp::sunstudy {

struct AtlasRegion {
    uint32_t x = 0, y = 0, width = 0, height = 0, layer = 0;
};

inline bool AtlasRegionsValid (const std::vector<AtlasRegion>& regions, uint32_t width, uint32_t height,
                               uint32_t layers)
{
    for (const auto& region : regions)
        if (region.width == 0 || region.height == 0 || region.x >= width || region.y >= height ||
            region.width > width - region.x || region.height > height - region.y || region.layer >= layers)
            return false;
    return true;
}

} // namespace evp::sunstudy
#endif
