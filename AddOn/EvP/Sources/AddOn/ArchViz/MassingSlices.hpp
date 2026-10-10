#ifndef EVP_ARCHVIZ_MASSINGSLICES_HPP
#define EVP_ARCHVIZ_MASSINGSLICES_HPP

#include "ArchViz/MassingCalculation.hpp"
#include "ArchViz/SlabSlices.hpp"
#include "ArchViz/HudSection.hpp"
#include "ArchViz/MassingBuildings.hpp"
#include "ArchViz/HudBuildingPlan.hpp"

namespace geomsrv::archviz::massingslices {
constexpr char kLayer[] = "tapioca.massing.storySlices";
struct Input {
    slabslices::Slab slab;
    metadata::EntityMetadata metadata;
    std::shared_ptr<const geomsrv::Mesh> body;
    // Current extracted surface; native defined sources also use it for body slicing.
    std::shared_ptr<const geomsrv::Mesh> facadeBody;
};
struct Row {
    std::string guid, function;
    int story = 0;
    double z = 0, floorHeight = 3, rawArea = 0, allowedArea = 0;
    double excludedArea = 0, rawVolume = 0, allowedVolume = 0;
    bool clipped = false;
    uint32_t rgba = 0x9AA0A6FF;
    uint32_t fillRgba = 0x9AA0A659;
    float fillOpacity = 1, wireWidthPixels = 2;
    std::vector<SliceChain> rawChains, chains;
    std::vector<SliceChain> footprintChains, lowChains,
        highlightChains; // Physical coverage vs counted/gray floor regions.
};
struct CoveragePatch {
    std::vector<SliceChain> built, unbuilt;
    double z = 0;
    bool hasElevation = false;
};
struct Result {
    bool complete = true; // False when automatic sources were skipped; baking must never create a partial building.
    overlaylayers::Layer layer;
    hudsection::Section section;
    std::vector<Row> rows;
    double rawArea = 0, allowedArea = 0;
    double excludedArea = 0;
    double firstFloorArea = 0, rawFirstFloorArea = 0, facadeArea = 0;
    bool hasFacade = false;
    std::vector<hudmeta::Page> heightControls;
    std::vector<massingbuildings::Surface> buildingSurfaces;
    std::vector<buildingplan::Source> planSources;
    bool clipped = false;
    double rawVolume = 0, allowedVolume = 0;
    double builtArea = 0, unbuiltArea = 0, parcelArea = 0;
    bool hasCoverage = false;
    std::vector<CoveragePatch> coverage;
    std::string note;
};
constexpr char kHighlightLayer[] = "tapioca.massing.functionVolumes";
constexpr char kUnbuiltProjectedLayer[] = "tapioca.massing.unbuilt.terrain";
constexpr char kLargeFloorsLayer[] = "tapioca.massing.largeFloorHighlight";
constexpr char kLowHeadroomLayer[] = "tapioca.massing.lowHeadroomHighlight";
constexpr char kBuiltHover[] = "coverage.built";
constexpr char kUnbuiltHover[] = "coverage.unbuilt";
struct Usage {
    std::string function, label;
    uint32_t rgba = 0;
    double area = 0, percent = 0, volume = 0;
};
std::vector<Usage> UsageMix (const Result& result);
bool Coverage (Result& result, const massingcalculation::Preview& parcels, std::string& error);
bool Highlight (const Result& result, const std::string& function, overlaylayers::Layer& layer, std::string& error);
// Closed `chains` (holes kept, even-odd) as a prism from `bottom` to `top`, caps and sides, appended
// to `mesh`'s points and indices in world coordinates; false when they do not triangulate.
bool ExtrudeChains (const std::vector<SliceChain>& chains, double bottom, double top, overlaylayers::Mesh& mesh,
                    std::string& error, bool topCap = true);
// Closed `chains` moved `distance` inward (holes grow), mitred; empty where nothing is left. What a
// floor is inside its facade wall (user, 2026-10-10: floorprogramme::kFacade inside the massing).
std::vector<SliceChain> Inset (const std::vector<SliceChain>& chains, double distance);
bool FloorHighlight (const Result& result, const std::string& building, const hudsection::Run& run,
                     overlaylayers::Layer& layer, std::string& error);
// Strictly >500 m2 gross, using the displayed section area basis and shared coefficient.
// Combine same-ID parts at one physical elevation; missing IDs remain separate slabs.
bool LargeFloorHighlight (const Result& result, const std::vector<massingbuildings::Record>& records,
                          const massingareas::Coefficients& coefficients, overlaylayers::Layer& layer,
                          std::string& error);
bool LowHeadroomHighlight (const Result& result, overlaylayers::Layer& layer, std::string& error);
overlaylayers::Layer RowDisplay (const Row& row, const storysliceoverlay::Slice& outside, const std::string& name,
                                 const storysliceoverlay::Controls& display);
bool UnbuiltHighlight (const Result& result, const Mesh* terrain, overlaylayers::Layer& plan,
                       overlaylayers::Layer& projected, std::string& error);
// Exposed union surface area, with 70-90 degree inclination from horizontal.
bool Facade (const std::vector<Input>& inputs, double& area, std::string& error);
// Allowed chains/areas come from intersection with the union of current parcel
// shell cuts. Their complement is a red warning display, never an allowed area.
// Slice shared Python shells with the existing mesh slicer; never solve setbacks here.
bool Intersect (const std::vector<SliceChain>& slab, const overlaylayers::Mesh& envelope, double z,
                std::vector<SliceChain>& outlines, double& area, std::string& error);
bool Build (const std::vector<Input>& slabs, const ProjectStoreys& storeys, const massingcalculation::Result* envelope,
            Result& result, std::string& error, const storysliceoverlay::Controls& display = {});
} // namespace geomsrv::archviz::massingslices
#endif
