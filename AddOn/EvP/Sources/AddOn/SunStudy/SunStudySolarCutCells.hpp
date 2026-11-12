#ifndef EVP_SUNSTUDY_SUNSTUDYSOLARCUTCELLS_HPP
#define EVP_SUNSTUDY_SUNSTUDYSOLARCUTCELLS_HPP

// SunStudy/SunStudySolarCutCells -- the study's own cells, re-measured after a
// solar cut, as geometry to draw over the study (#168).
//
// The won-lit surface says where the cut brings sun during its slice. This
// says what the STUDY would read after the cut: each study cell whose day
// gains sun gets its whole-day hours re-counted (the study's stored lit steps
// plus the steps whose ray now runs clear past what remains), and is drawn as
// its own cell in those hours. The cells of one surface are merged, and the
// merged region's perimeter is returned on its own, so the view can mark
// "this part came from the cut" with one outline instead of a grid of squares.
//
// Pure CPU geometry; no store, no host. The caller says which surface each
// cell lies on and gives that surface's triangles: a cell's square is clipped
// to them, so a cell on a surface edge stops where the surface does.

#include "SunStudy/SunStudySolarCut.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace evp::sunstudy {

// One study cell the cut changes.
struct SolarCutCell {
    uint64_t surface = 0;      // the cells of one surface merge
    double corners[4][3] = {}; // the cell's square, in order around it, world
    uint32_t sample = 0;       // the study sample it measures
    float before = 0.0f;       // whole-day direct hours the study read
    float after = 0.0f;        // with the cut
};

// The cells sharing one after-cut value, clipped to their surfaces.
struct SolarCutCellGroup {
    float after = 0.0f;
    std::vector<SolarCutTriangle> triangles;
};

struct SolarCutCellsResult {
    std::vector<SolarCutCellGroup> groups;      // ascending `after`
    std::vector<std::array<double, 6>> outline; // the merged region's perimeter
    // Per changed sample, ascending: what the hover reads.
    std::vector<uint32_t> samples;
    std::vector<float> before, after;
    size_t cells = 0, unplaced = 0; // drawn; changed but with no surface to draw on
    double area = 0.0;              // m2 of the merged region
    float mostGained = 0.0f;        // hours, the largest after - before
    std::string error;

    // The hours a changed sample reads before and after; false when the cut
    // does not change it.
    bool Read (uint32_t sample, float& beforeHours, float& afterHours) const;
};

bool BuildSolarCutCells (const std::vector<SolarCutCell>& cells,
                         const std::map<uint64_t, std::vector<SolarCutTriangle>>& surfaces, SolarCutCellsResult& out,
                         const std::function<bool ()>& cancelled = {});

} // namespace evp::sunstudy

#endif
