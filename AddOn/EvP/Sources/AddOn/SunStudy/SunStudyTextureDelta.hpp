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

// Exact row-run deltas, including retired texels/sentinels. Rectangles merge only
// vertically with identical extents. Fragmented layers use one full-plane update
// to bound API calls; missing/incompatible bases must use a complete upload.
template <typename T>
std::vector<AtlasRegion> AtlasChangedRegions (uint32_t width, uint32_t height, uint32_t layers,
                                              const std::vector<T>& before, const std::vector<T>& after,
                                              size_t maxRegionsPerLayer = 512,
                                              const std::function<bool ()>& isCancelled = {})
{
    if (width == 0 || height == 0 || layers == 0 ||
        static_cast<size_t> (width) * height > std::numeric_limits<size_t>::max () / layers ||
        before.size () != static_cast<size_t> (width) * height * layers || before.size () != after.size ())
        return {};
    std::vector<AtlasRegion> regions;
    const size_t plane = static_cast<size_t> (width) * height;
    for (uint32_t layer = 0; layer < layers; ++layer) {
        const size_t firstRegion = regions.size ();
        std::vector<size_t> previous;
        for (uint32_t y = 0; y < height; ++y) {
            if (isCancelled && isCancelled ())
                return {}; // caller must discard, never publish a partial delta
            std::vector<size_t> current;
            for (uint32_t x = 0; x < width;) {
                const auto changed = [&] (uint32_t column) {
                    const size_t i = layer * plane + y * static_cast<size_t> (width) + column;
                    return std::memcmp (&before[i], &after[i], sizeof (T)) != 0;
                };
                if (!changed (x)) {
                    ++x;
                    continue;
                }
                const uint32_t start = x++;
                while (x < width && changed (x))
                    ++x;
                size_t extend = regions.size ();
                for (size_t index : previous)
                    if (regions[index].x == start && regions[index].width == x - start) {
                        extend = index;
                        break;
                    }
                if (extend < regions.size ())
                    ++regions[extend].height;
                else
                    regions.push_back ({ start, y, x - start, 1, layer });
                current.push_back (extend);
            }
            previous = std::move (current);
            if (regions.size () - firstRegion > maxRegionsPerLayer) {
                regions.resize (firstRegion);
                regions.push_back ({ 0, 0, width, height, layer });
                break;
            }
        }
    }
    return regions;
}

} // namespace evp::sunstudy
#endif
