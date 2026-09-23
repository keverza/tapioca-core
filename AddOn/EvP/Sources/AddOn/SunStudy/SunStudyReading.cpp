#include "SunStudy/SunStudyReading.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace evp::sunstudy {

namespace {

double Dot (const double a[3], const double b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// The shader's cell arithmetic: floor of the lattice coordinate, minus the
// start cell, clamped to the tile. ⚠️ KEEP IN STEP WITH kArchVizSunTintPS.
//
// ⚠️ WITH THE GPU RECORD'S QUANTITIES, NOT THE EXACT ONES. The shader sees the
// origin and axes rounded to float and MULTIPLIES by a float 1/spacing; exact
// double division picks the other cell at every boundary that is a whole
// multiple of the spacing (the offline test found x = 5.6 at 0.7 m), and the
// inspector must describe the colour on screen, not a cleaner number.
uint32_t Cell (const double point[3], const double origin[3], const double axis[3], double spacing, long start,
               uint32_t count)
{
    const double relative[3] = { point[0] - static_cast<float> (origin[0]), point[1] - static_cast<float> (origin[1]),
                                 point[2] - static_cast<float> (origin[2]) };
    const double gpuAxis[3] = { static_cast<float> (axis[0]), static_cast<float> (axis[1]),
                                static_cast<float> (axis[2]) };
    const double invSpacing = static_cast<float> (1.0 / spacing);
    const double cell = std::floor (Dot (relative, gpuAxis) * invSpacing) - static_cast<double> (start);
    const double last = count > 0 ? static_cast<double> (count - 1) : 0.0;
    return static_cast<uint32_t> (std::clamp (cell, 0.0, last));
}

double DistanceSquared (const std::vector<double>& positions, size_t sample, const double point[3])
{
    double sum = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        const double d = positions[sample * 3 + axis] - point[axis];
        sum += d * d;
    }
    return sum;
}

// Among `candidates`, the sample in (column, row); failing that the nearest.
template <typename Candidates>
SunStudyReading Choose (const Candidates& candidates, const std::vector<uint32_t>& columns,
                        const std::vector<uint32_t>& rows, const std::vector<double>& positions,
                        const std::vector<double>& hours, uint32_t column, uint32_t row, const double point[3])
{
    SunStudyReading out;
    out.column = column;
    out.row = row;
    bool found = false;
    double best = std::numeric_limits<double>::max ();
    candidates ([&] (size_t sample) {
        if (sample >= hours.size () || sample >= columns.size () || sample >= rows.size ())
            return;
        if (columns[sample] == column && rows[sample] == row) {
            if (!found || out.nearest) {
                out.sample = sample;
                out.nearest = false;
                found = true;
            }
            return;
        }
        if (found && !out.nearest)
            return;
        const double d = DistanceSquared (positions, sample, point);
        if (d < best) {
            best = d;
            out.sample = sample;
            out.nearest = true;
            found = true;
        }
    });
    if (!found)
        return SunStudyReading ();
    out.measured = true;
    out.hours = hours[out.sample];
    return out;
}

} // namespace

SunStudyReading ReadTriangleStudyAt (const SampleGrid& grid, double spacing, const std::vector<double>& hours,
                                     size_t face, const double point[3])
{
    if (point == nullptr || !(spacing > 0.0) || face >= grid.layouts.size ())
        return SunStudyReading ();
    const FaceLayout& layout = grid.layouts[face];
    // A face below the grid carries one centroid sample in cell (0,0).
    const uint32_t column =
        layout.gridded ? Cell (point, layout.origin, layout.uAxis, spacing, layout.uStart, layout.columns) : 0u;
    const uint32_t row =
        layout.gridded ? Cell (point, layout.origin, layout.vAxis, spacing, layout.vStart, layout.rows) : 0u;

    // The triangle grid is not grouped by face, so the face's samples are
    // found by scanning. A hover asks a few times a second, which is nothing.
    auto candidates = [&] (auto&& visit) {
        for (size_t i = 0; i < grid.faces.size (); ++i)
            if (grid.faces[i] == face)
                visit (i);
    };
    return Choose (candidates, grid.cellColumns, grid.cellRows, grid.positions, hours, column, row, point);
}

SunStudyReading ReadPatchStudyAt (const PatchSampleGrid& grid, double spacing, const std::vector<double>& hours,
                                  size_t face, const double point[3])
{
    if (point == nullptr || !(spacing > 0.0) || face >= grid.patchOfTriangle.size ())
        return SunStudyReading ();
    const uint32_t spanIndex = grid.patchOfTriangle[face];
    if (spanIndex == PatchSampleGrid::kNoPatch || spanIndex >= grid.spans.size ())
        return SunStudyReading ();
    const PatchSampleSpan& span = grid.spans[spanIndex];
    const uint32_t column = Cell (point, span.origin, span.uAxis, spacing, 0, span.columns);
    const uint32_t row = Cell (point, span.origin, span.vAxis, spacing, 0, span.rows);

    // A patch's samples are one contiguous span -- no scan of the study.
    auto candidates = [&] (auto&& visit) {
        for (size_t i = span.first; i < span.first + span.count; ++i)
            visit (i);
    };
    return Choose (candidates, grid.cellColumns, grid.cellRows, grid.positions, hours, column, row, point);
}

} // namespace evp::sunstudy
