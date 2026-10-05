#ifndef EVP_ARCHVIZ_MASSINGCOLLAPSEZONE_HPP
#define EVP_ARCHVIZ_MASSINGCOLLAPSEZONE_HPP
#include "ArchViz/MassingSlices.hpp"

namespace geomsrv::archviz::massingcollapse {
constexpr char kLayer[] = "tapioca.massing.collapseZone";
constexpr char kProjectedLayer[] = "tapioca.massing.collapseZone.terrain";
constexpr double kHeightFactor = 0.3333;
struct Result {
    overlaylayers::Layer layer;
    std::vector<SliceChain> chains;
    double area = 0;
    double hatchOriginSum = 0;
};
// Requires current operated bodies for EVERY input. Height is the local vertical
// span at each XY surface patch, not the body's bounding-box height.
bool Build (const std::vector<massingslices::Input>& inputs, double drawingZ, Result& result, std::string& error);
// Vertical projection onto the upward-facing topography triangles. Outside its
// extent remains undrawn; no mean-Z plane or underside substitute in 3D.
bool Project (const Result& zone, const Mesh& terrain, overlaylayers::Layer& layer, std::string& error);
} // namespace geomsrv::archviz::massingcollapse
#endif
