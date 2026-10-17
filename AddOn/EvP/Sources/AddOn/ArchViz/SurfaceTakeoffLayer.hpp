#ifndef EVP_SURFACETAKEOFFLAYER_HPP
#define EVP_SURFACETAKEOFFLAYER_HPP

// OVERLAY-INVARIANTS.md: a client of existing layers, never a camera/renderer.
#include "ArchViz/OverlayLayers.hpp"
#include "Geometry/SurfaceAttributeMap.hpp"

namespace geomsrv::archviz::surfacetakeoff {
constexpr const char* kLayer = "tapioca.exposedSurfaces";
struct SurfaceRow {
    int32_t index = 0;
    std::string name;
    uint32_t rgba = 0;
    double areaM2 = 0;
};
struct Presentation {
    overlaylayers::Layer layer;
    std::vector<SurfaceRow> rows;
    size_t triangles = 0;
    double areaM2 = 0;
};
// Pure: the exact selected fragment triangles are copied to the layer and summed
// independently for its table. Whole-element native quantities are a comparison,
// never a replacement for highlighted area or schedule-equivalence evidence.
bool Build (const Snapshot& snapshot, const exposed::Result& result, const exposed::SurfaceSelection& selection,
            const std::vector<int32_t>& surfaces, const std::map<int32_t, double>& nativeAreas,
            const std::string& comparisonNote, Presentation& output, std::string& error,
            const std::vector<uint32_t>& colors = { 0xE5484DFFu, 0xF6C945FFu });
} // namespace geomsrv::archviz::surfacetakeoff
#endif
