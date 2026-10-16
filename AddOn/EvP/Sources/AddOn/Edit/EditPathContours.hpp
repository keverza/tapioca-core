#ifndef EVP_EDIT_EDITPATHCONTOURS_HPP
#define EVP_EDIT_EDITPATHCONTOURS_HPP

// The paths' grading read as contour lines: where their walking surface crosses each level
// of a round interval, a visual guide to how the ground falls. Each path's surface is its
// outline -- left edge, centreline, right edge at every sample -- with the crossfall
// carried straight from the centre to each edge; where paths cross, the ground belongs to
// the path it lies deepest in, as the paved surface's heights do (Pave).
#include "Edit/EditPath.hpp"
#include <vector>

namespace geomsrv::edit {

struct Contour {
    Id path = kNoId;
    double z = 0.0;
    std::vector<Vec3> points; // in plan at height `z`, in the order the line runs
};

// Every `interval` metres of height (levels at whole multiples of it), traced on samples
// `step` metres apart. Paths that do not validate are left out; an interval that would
// cut more than `maxLevels` levels draws none.
std::vector<Contour> PathContours (const std::vector<Path>& paths, double interval, double step = 0.5,
                                   size_t maxLevels = 400);

// The paths' lowest and highest surface heights; false when there are none.
bool PathHeightRange (const std::vector<Path>& paths, double& lo, double& hi, double step = 0.5);

// A round interval -- 1, 2, 2.5 or 5 times a power of ten -- cutting `range` into about
// `divisions` parts; and the whole multiples of `step` from `lo` to `hi`.
double NiceStep (double range, int divisions);
std::vector<double> Levels (double lo, double hi, double step);
// The decimals a multiple of `step` needs to be written exactly (0.25 needs 2, 0.5 needs 1).
int StepDecimals (double step);

} // namespace geomsrv::edit
#endif
