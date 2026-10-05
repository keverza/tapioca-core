#ifndef EVP_ARCHVIZ_MASSINGSLICES_HPP
#define EVP_ARCHVIZ_MASSINGSLICES_HPP

#include "ArchViz/MassingCalculation.hpp"
#include "ArchViz/SlabSlices.hpp"
#include "ArchViz/HudSection.hpp"

namespace geomsrv::archviz::massingslices {
constexpr char kLayer[] = "tapioca.massing.storySlices";
struct Input {
    slabslices::Slab slab;
    metadata::EntityMetadata metadata;
    std::shared_ptr<const geomsrv::Mesh> body;
};
struct Row {
    std::string guid, function;
    int story = 0;
    double z = 0, floorHeight = 3, rawArea = 0, allowedArea = 0;
    bool clipped = false;
};
struct Result {
    overlaylayers::Layer layer;
    hudsection::Section section;
    std::vector<Row> rows;
    double rawArea = 0, allowedArea = 0;
    double firstFloorArea = 0, rawFirstFloorArea = 0, facadeArea = 0;
    bool hasFacade = false;
    std::vector<hudmeta::Page> heightControls;
    bool clipped = false;
    std::string note;
};
// Allowed chains/areas come from intersection with the union of current parcel
// shell cuts. Their complement is a red warning display, never an allowed area.
// Slice shared Python shells with the existing mesh slicer; never solve setbacks here.
bool Intersect (const std::vector<SliceChain>& slab, const overlaylayers::Mesh& envelope, double z,
                std::vector<SliceChain>& outlines, double& area, std::string& error);
bool Build (const std::vector<Input>& slabs, const ProjectStoreys& storeys, const massingcalculation::Result* envelope,
            Result& result, std::string& error, const storysliceoverlay::Controls& display = {});
} // namespace geomsrv::archviz::massingslices
#endif
