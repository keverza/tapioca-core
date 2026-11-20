#ifndef EVP_TESTS_SUNSTUDY_REFERENCE_KERNELS_HPP
#define EVP_TESTS_SUNSTUDY_REFERENCE_KERNELS_HPP

// tests/cpp/sunstudy_reference_kernels.hpp -- the C++ sun-study kernels the
// store's readers are recomputed against, kept for the tests ONLY after P7b
// moved every production kernel to the Rust core. They are the bodies the
// add-on ran until then (summary, atlas scatter, patch scatter, step masks,
// step minutes, solar noon, the hover reading), unchanged, so a store reader
// that drifts from them is caught by an independent implementation rather
// than compared with itself. Not for production code.

#include "SunStudy/SunSeries.hpp"
#include "SunStudy/SunStudyAtlas.hpp"
#include "SunStudy/SunStudyPatchAtlas.hpp"
#include "SunStudy/SunStudyReading.hpp"
#include "SunStudy/SunStudyResultSummary.hpp"
#include "SunStudy/SunStudyStepAtlas.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

namespace evp::sunstudy::reference {

template <typename HoursAt>
inline SunStudyResultSummary SummarizeSunHours (size_t count, double daylightHours, const HoursAt& hoursAt)
{
    SunStudyResultSummary summary;
    summary.count = count;
    summary.daylightHours = daylightHours;
    if (count == 0)
        return summary;
    summary.minHours = summary.maxHours = hoursAt (0);
    double sum = 0.0;
    for (size_t sample = 0; sample < count; ++sample) {
        const double value = hoursAt (sample);
        summary.minHours = std::min (summary.minHours, value);
        summary.maxHours = std::max (summary.maxHours, value);
        sum += value;
        summary.fullyLit += value >= daylightHours - 1e-9 ? 1 : 0;
        summary.fullyShaded += value <= 1e-9 ? 1 : 0;
    }
    summary.meanHours = sum / count;
    return summary;
}

inline SunStudyResultSummary SummarizeSunHours (const std::vector<double>& hours, double daylightHours)
{
    return SummarizeSunHours (hours.size (), daylightHours, [&hours] (size_t sample) { return hours[sample]; });
}

inline std::vector<float> ScatterToAtlas (const SunStudyAtlas& atlas, const std::vector<double>& perSample,
                                          float emptyValue = -1.0f)
{
    std::vector<float> image;
    if (!atlas.valid || atlas.TexelCount () == 0)
        return image;

    image.assign (atlas.TexelCount (), emptyValue);
    const size_t count = std::min (perSample.size (), atlas.texels.size ());
    for (size_t i = 0; i < count; ++i) {
        const uint32_t texel = atlas.texels[i];
        if (texel < image.size ())
            image[texel] = static_cast<float> (perSample[i]);
    }
    return image;
}

inline StepMaskAtlas PackStepMasks (size_t sampleCount, size_t stepCount,
                                    const std::function<bool (size_t, size_t)>& lit,
                                    const std::vector<int64_t>& texelOfSample, uint32_t width, uint32_t height,
                                    const std::function<bool ()>& isCancelled = {})
{
    if (isCancelled && isCancelled ())
        return {};
    StepMaskAtlas out;
    out.width = width;
    out.height = height;
    out.steps = static_cast<uint32_t> (stepCount);
    // ⚠️ AT LEAST ONE SLICE, even for a study of no steps: the shader always
    // binds the texture, and a zero-slice array is not a texture at all.
    out.words = static_cast<uint32_t> ((stepCount + 31) / 32);
    if (out.words == 0)
        out.words = 1;
    const size_t plane = static_cast<size_t> (width) * height;
    out.masks.assign (plane * out.words, 0u);
    if (!lit)
        return out;

    for (size_t sample = 0; sample < sampleCount && sample < texelOfSample.size (); ++sample) {
        // Cancellation discards the whole image, never a plausible partial day.
        if (sample % 1024 == 0 && isCancelled && isCancelled ())
            return {};
        const int64_t texel = texelOfSample[sample];
        if (texel < 0 || static_cast<size_t> (texel) >= plane)
            continue;
        for (size_t step = 0; step < stepCount; ++step) {
            if (lit (sample, step))
                out.masks[(step >> 5) * plane + static_cast<size_t> (texel)] |= 1u << (step & 31u);
        }
    }
    if (isCancelled && isCancelled ())
        return {};
    return out;
}

inline uint32_t SolarNoonStep (const SunSeries& series)
{
    uint32_t best = 0;
    for (size_t step = 1; step < series.StepCount (); ++step)
        if (series.Step (step).altitudeDegrees > series.Step (best).altitudeDegrees)
            best = static_cast<uint32_t> (step);
    return best;
}

inline std::vector<uint16_t> StepMinutes (const SunSeries& series)
{
    std::vector<uint16_t> out;
    out.reserve (series.StepCount ());
    for (size_t step = 0; step < series.StepCount (); ++step) {
        const TimeOfDay& time = series.Step (step).time;
        out.push_back (static_cast<uint16_t> (time.hour * 60 + time.minute));
    }
    return out;
}

inline bool ScatterPatch (const SunStudyPatchAtlas& atlas, const PatchSampleGrid& grid, size_t spanIndex,
                          const std::vector<double>& hoursForSpan, std::vector<float>& image)
{
    if (spanIndex >= grid.spans.size () || image.size () != atlas.TexelCount ())
        return false;
    const PatchSampleSpan& span = grid.spans[spanIndex];
    if (hoursForSpan.size () != span.count)
        return false;
    const PatchAtlasAllocation* allocation = atlas.Find (span.key);
    if (allocation == nullptr || !allocation->Placed ())
        return false;

    for (size_t i = 0; i < span.count; ++i) {
        const int64_t texel = atlas.TexelOf (grid, span.first + i);
        if (texel < 0 || static_cast<size_t> (texel) >= image.size ())
            continue;
        image[static_cast<size_t> (texel)] = static_cast<float> (hoursForSpan[i]);
    }
    return true;
}

namespace detail {

inline double Dot (const double a[3], const double b[3])
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
inline uint32_t Cell (const double point[3], const double origin[3], const double axis[3], double spacing, long start,
                      uint32_t count)
{
    const double relative[3] = { point[0] - static_cast<float> (origin[0]), point[1] - static_cast<float> (origin[1]),
                                 point[2] - static_cast<float> (origin[2]) };
    const double gpuAxis[3] = { static_cast<float> (axis[0]), static_cast<float> (axis[1]),
                                static_cast<float> (axis[2]) };
    const double invSpacing = static_cast<float> (1.0 / spacing);
    const double cell = std::floor (Dot (relative, gpuAxis) * invSpacing) - static_cast<double> (start);
    const double last = count > 0 ? static_cast<double> (count - 1) : 0.0;
    // std::clamp passes a NaN through, and a NaN cast to uint32_t is undefined
    // (#21): a non-finite hover point or a degenerate axis reads cell 0, as the
    // Rust twin's clamp_cell does.
    if (std::isnan (cell))
        return 0;
    return static_cast<uint32_t> (std::clamp (cell, 0.0, last));
}

inline double DistanceSquared (const std::vector<double>& positions, size_t sample, const double point[3])
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
inline SunStudyReading Choose (const Candidates& candidates, const std::vector<uint32_t>& columns,
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

} // namespace detail

using namespace detail;

inline SunStudyReading ReadTriangleStudyAt (const SampleGrid& grid, double spacing, const std::vector<double>& hours,
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

inline SunStudyReading ReadPatchStudyAt (const PatchSampleGrid& grid, double spacing, const std::vector<double>& hours,
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

} // namespace evp::sunstudy::reference

#endif
