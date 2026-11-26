#ifndef TAPIOCA_ARCHVIZ_PLANTINGSCHEMESCENE_HPP
#define TAPIOCA_ARCHVIZ_PLANTINGSCHEMESCENE_HPP

// ArchViz/PlantingSchemeScene -- a placed planting scheme as depth-tested
// scene geometry (#278): one cylinder per plant, its mature crown as the radius,
// and lines joining the plants of each species, centre to centre.
//
// Pure: world-space GhPreview buckets, no device, camera or ImGui. The HUD
// builds them when the scheme changes; DiligentScene draws them through its
// planting-scheme channel (DiligentScenePlantingSchemeOverlay.cpp), like the
// sun path, so the model hides what is behind them.
//
// ⚠️ A MARKER, NOT A PLANT MODEL. The cylinder says where a plant's mature crown
// would reach in plan. "Mature" draws it to its catalogue height, which is
// coarse; "Marker" keeps it a low disc so the plan reads.

#include "ArchViz/GhPreviewGeometry.hpp"
#include "ArchViz/PlantingScheme.hpp"

#include <cstdint>

namespace geomsrv::archviz::planting {

constexpr double kSchemeDiscHeight = 0.2;
constexpr float kSchemeLinkPixels = 2.0f;

struct PlantingSchemeGeometry {
    GhPreviewDrawables crowns; // translucent cylinders and their top rims
    GhPreviewDrawables links;  // same-species lines and centre ticks
    size_t plants = 0;
};

// `highlight` >= 0 dims every other species.
void BuildPlantingSchemeScene (const SchemeResult& scheme, SchemeMarker marker, int highlight,
                               PlantingSchemeGeometry& out);

} // namespace geomsrv::archviz::planting

#endif
