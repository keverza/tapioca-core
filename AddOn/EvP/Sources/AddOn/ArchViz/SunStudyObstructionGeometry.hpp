#ifndef EVP_ARCHVIZ_SUNSTUDYOBSTRUCTIONGEOMETRY_HPP
#define EVP_ARCHVIZ_SUNSTUDYOBSTRUCTIONGEOMETRY_HPP

// Pure world-space geometry for single-day exact obstruction attribution.
//
// #168: what is filled is each selected cell's SHADOW FOOTPRINT on its blocker
// -- sub-cells cast along the sun onto the face their rays hit -- not the whole
// hit triangle, so a large coarse wall triangle no longer paints a region the
// cell never saw. Each blocked cell also gets its receiver-to-hit ray.
// #167: the whole blocking element is not drawn here; the scene highlights it
// as a selection is highlighted, in the obstruction colour
// (DiligentScene::SetObstructionHighlight). Every fill and ray is coloured by
// its step's time of day (SunStudyTimeOfDayRgba), the diagram's time row.

#include "ArchViz/GhPreviewGeometry.hpp"
#include "Geometry/Mesh.hpp"
#include "SunStudy/SunStudyObstruction.hpp"
#include "SunStudy/SunStudySolarCut.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace geomsrv::archviz {

constexpr uint32_t kSunObstructionPink = 0xF240BFFFu;

uint32_t SunObstructionTimeColour (int minute);

// One blocking element over the attributed steps. Elapsed time is a union over
// the selected cells -- a step counts once however many cells it blocks -- so
// several cells never add up to more hours than the day had.
struct SunObstructionBlocker {
    std::string guid; // as the snapshot names it, for the scene highlight
    int32_t elemType = 0;
    bool context = false;
    std::vector<std::pair<int, int>> intervals; // [from, to) clock minutes, merged across adjacent steps
    double hours = 0.0;                         // elapsed hours it blocks at least one selected cell
    double meanCellShare = 0.0;                 // over those steps, the mean share of the cells it blocks
};

struct SunObstructionGeometry {
    GhPreviewDrawables drawables;
    size_t blockerObjects = 0;
    size_t footprints = 0;                       // sub-cell footprint fills
    std::vector<SunObstructionBlocker> blockers; // most hours first
    // Cell-steps by state: what was lit, blocked, behind the surface, or could
    // not be attributed (a sample outside the record) -- never folded together.
    size_t lit = 0, blocked = 0, backFacing = 0, unavailable = 0;
    std::string error;
};

// `timestepMinutes` is the study's step, for interval ends and elapsed hours.
// Multiple steps (whole day) draw the ray of the first, snapped cell only;
// one step draws every blocked cell's ray.
bool BuildSunObstructionGeometry (const geomsrv::Snapshot& snapshot,
                                  const std::vector<evp::sunstudy::SunObstructionResult>& steps, int timestepMinutes,
                                  SunObstructionGeometry& result);

// The solar cut's preview: the merged cutting surface (faint), the blockers'
// surfaces above it (what goes) and the cut faces (strong), and the envelope's
// edge rays from the point. Cells whose shadow the cut removes are drawn by
// AppendSolarCutWonCells, in their own colour.
constexpr uint32_t kSolarCutEnvelopeRgba = 0xF240BF38u;
constexpr uint32_t kSolarCutRemovedRgba = 0xF240BF90u;
constexpr uint32_t kSolarCutCapRgba = 0xF240BFD8u;
constexpr uint32_t kSolarCutWonRgba = 0xFFE27AC0u; // sun the cut wins: a warm yellow, apart from the map's colours
void BuildSolarCutGeometry (const evp::sunstudy::SolarCutResult& cut, const double point[3],
                            GhPreviewDrawables& drawables);
// One square per won cell, `size` across, lifted off its surface along its normal.
struct SolarCutWonCell {
    double position[3] = {};
    double normal[3] = { 0.0, 0.0, 1.0 };
};
void AppendSolarCutWonCells (const std::vector<SolarCutWonCell>& cells, double size, GhPreviewDrawables& drawables);

} // namespace geomsrv::archviz

#endif
