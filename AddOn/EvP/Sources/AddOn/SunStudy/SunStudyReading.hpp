#ifndef EVP_SUNSTUDY_SUNSTUDYREADING_HPP
#define EVP_SUNSTUDY_SUNSTUDYREADING_HPP

// SunStudy/SunStudyReading -- the study's value at one point on one face: what
// the hover inspector reports under the cursor.
//
// ⚠️ IT READS THE SAME CELL THE TINT SHADER DRAWS. The shader maps a world point
// through the face's (or the patch's) lattice to a cell and reads that cell's
// texel; this maps the same point through the same lattice to the same cell and
// returns the sample that was scattered there. A reader that picked the NEAREST
// sample instead would disagree with the colour under the cursor at every cell
// boundary -- the one place a person hovering to check a value looks.
//
// Nearest is the FALLBACK only: a cell the lattice holds but no sample covers
// (a partial boundary cell, float rounding at an edge) still reports the
// surface's closest measurement rather than nothing.

#include "SunStudy/SunStudyPatchSampler.hpp"
#include "SunStudy/SunStudySampler.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace evp::sunstudy {

struct SunStudyReading {
    // False when the face carries no measurement: a context or ignored
    // element, a degenerate face, a triangle no patch claimed.
    bool measured = false;
    double hours = 0.0;
    size_t sample = 0;
    uint32_t column = 0;
    uint32_t row = 0;
    // True when the cell under the point held no sample and the nearest sample
    // of the same surface was reported instead.
    bool nearest = false;
};

// Triangle domain. `grid` must have been built with `wantLayouts`.
SunStudyReading ReadTriangleStudyAt (const SampleGrid& grid, double spacing, const std::vector<double>& hours,
                                     size_t face, const double point[3]);

// Patch domain.
SunStudyReading ReadPatchStudyAt (const PatchSampleGrid& grid, double spacing, const std::vector<double>& hours,
                                  size_t face, const double point[3]);

} // namespace evp::sunstudy

#endif // EVP_SUNSTUDY_SUNSTUDYREADING_HPP
