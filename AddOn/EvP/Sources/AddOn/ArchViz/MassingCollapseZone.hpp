#ifndef EVP_ARCHVIZ_MASSINGCOLLAPSEZONE_HPP
#define EVP_ARCHVIZ_MASSINGCOLLAPSEZONE_HPP
#include "ArchViz/MassingSlices.hpp"

namespace geomsrv::archviz::massingcollapse {
constexpr char kLayer[] = "tapioca.massing.collapseZone";
constexpr double kHeightFactor = 0.3333;
struct Result {
    overlaylayers::Layer layer;
    std::vector<SliceChain> chains;
    double area = 0;
};
// Requires current operated bodies for EVERY input. Height is the local vertical
// span at each XY surface patch, not the body's bounding-box height.
bool Build (const std::vector<massingslices::Input>& inputs, double drawingZ, Result& result, std::string& error);
} // namespace geomsrv::archviz::massingcollapse
#endif
