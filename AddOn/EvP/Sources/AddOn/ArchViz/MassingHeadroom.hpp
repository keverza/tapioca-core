#ifndef EVP_ARCHVIZ_MASSINGHEADROOM_HPP
#define EVP_ARCHVIZ_MASSINGHEADROOM_HPP
#include "ArchViz/StorySliceGeometry.hpp"
#include <string>

namespace geomsrv::archviz::massingheadroom {
constexpr double kMinimum = 1.6; // Vertical metres above the physical floor, not perpendicular roof distance.
struct Split {
    std::vector<SliceChain> counted, excluded;
    double countedArea = 0, excludedArea = 0;
};
// Closed, outward-wound operated solid. Project every upward exit surface in
// [floorZ, floorZ+1.6): a higher roof cannot hide a lower SEO ceiling/cavity.
// Source contours already describe the solid at floorZ; holes are preserved.
// Shared triangle-work budget across one automatic massing build. Atomic output.
bool Partition (const std::vector<SliceChain>& source, double floorZ, const std::vector<double>& vertices,
                const std::vector<uint32_t>& indices, Split& result, size_t& work, std::string& error);
} // namespace geomsrv::archviz::massingheadroom
#endif
