#ifndef EVP_TESTS_SUNSTUDY_KERNEL_ORACLE_HPP
#define EVP_TESTS_SUNSTUDY_KERNEL_ORACLE_HPP

// What every SunStudyStore reader answered when the C++ study core ran the
// study, recomputed with the C++ kernels (atlas scatter, patch fan-out, step
// masks, summary, hover reading) from the Rust core's own hours and step bits,
// over the record's own grids and atlases. The store now answers from the
// Rust core; these say it still draws, summarises and reads exactly what the
// record's display atlas promises. The kernels stay C++ until P7b.

#include "SunStudy/SunStudyReading.hpp"
#include "SunStudy/SunStudyRustStudy.hpp"
#include "SunStudy/SunStudyStepAtlas.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace evp::sunstudy::fixture {

template <typename T> bool SameBytes (const std::vector<T>& a, const std::vector<T>& b)
{
    return a.size () == b.size () && (a.empty () || std::memcmp (a.data (), b.data (), a.size () * sizeof (T)) == 0);
}

inline bool SameBytes (double a, double b)
{
    return std::memcmp (&a, &b, sizeof a) == 0;
}

struct KernelImage {
    bool ok = false;
    uint32_t width = 0, height = 0;
    std::vector<float> image;
    std::string error;
};

struct KernelReads {
    std::vector<double> hours;
    std::vector<uint8_t> bits; // one byte per (sample, step), sample-major
    SunStudyResultSummary summary;
    KernelImage triangle, patch, display;
    std::vector<AtlasTile> tiles;
    std::vector<FaceLayout> layouts;
    bool masksOk = false;
    std::string masksError;
    StepMaskAtlas masks;
    std::vector<uint16_t> minutes;
    uint32_t noon = 0;
};

// The patch atlas image as the C++ store scattered it: span by span through
// the atlas's own routine, the sentinel everywhere else.
inline std::vector<float> ScatterPatchImage (const StudyRecord& record, const std::vector<double>& hours)
{
    std::vector<float> image (record.patchAtlas.TexelCount (), -1.0f);
    std::vector<double> forSpan;
    for (size_t spanIndex = 0; spanIndex < record.patchGrid.spans.size (); ++spanIndex) {
        const PatchSampleSpan& span = record.patchGrid.spans[spanIndex];
        if (span.first + span.count > hours.size ())
            continue;
        forSpan.assign (hours.begin () + span.first, hours.begin () + span.first + span.count);
        record.patchAtlas.ScatterPatch (record.patchGrid, spanIndex, forSpan, image);
    }
    return image;
}

// Read under the record's session lock (the store's display lease), from the
// core's hours and bits as they stand.
inline KernelReads ReadThroughKernels (const std::string& id)
{
    KernelReads out;
    auto& store = SunStudyStore::Get ();
    std::string error;
    const bool leased = store.ReadDisplayRecord (
        id, store.Revision (id),
        [&] (const StudyRecord& record) {
            std::string coreError;
            EXPECT_NE (record.rustCore, nullptr);
            if (record.rustCore == nullptr || !record.rustCore->SunHours (out.hours, coreError) ||
                !record.rustCore->StepBits (out.bits, coreError)) {
                ADD_FAILURE () << coreError;
                return;
            }
            const size_t samples = record.positions.size () / 3;
            const size_t steps = record.series.StepCount ();
            const auto lit = [&] (size_t sample, size_t step) { return out.bits[sample * steps + step] != 0; };
            const double quantum = record.series.HoursPerStep ();
            out.summary = SummarizeSunHours (samples, record.series.DaylightHours (), [&] (size_t sample) {
                size_t count = 0;
                for (size_t step = 0; step < steps; ++step)
                    count += lit (sample, step) ? 1 : 0;
                return count * quantum;
            });

            if (record.atlas.valid) {
                out.triangle = {
                    true, record.atlas.width, record.atlas.height, ScatterToAtlas (record.atlas, out.hours), {}
                };
            }
            else {
                out.triangle.error =
                    record.IsPatchDomain ()
                        ? "study '" + id + "' is a patch-domain study - ask for its patch atlas, not the triangle one"
                        : "study '" + id + "' has no atlas - it was not sampled on model surfaces";
            }

            const bool packedPatch =
                record.IsPatchDomain () && record.patchGrid.valid && record.patchAtlas.Width () != 0;
            if (packedPatch)
                out.patch = { true,
                              record.patchAtlas.Width (),
                              record.patchAtlas.Height (),
                              ScatterPatchImage (record, out.hours),
                              {} };
            else
                out.patch.error = "study '" + id + "' is not a patch-domain study with a packed atlas";

            if (record.IsPatchDomain ()) {
                if (!packedPatch)
                    out.display.error = "study '" + id + "' is a patch-domain study with no packed patch atlas";
                else if (!PatchFaceArrays (record.patchGrid, record.patchAtlas, out.tiles, out.layouts))
                    out.display.error =
                        "study '" + id + "' has a patch the patch atlas did not place - the two are not one study";
                else
                    out.display = out.patch;
            }
            else if (record.atlas.valid) {
                out.display = out.triangle;
                out.tiles = record.atlas.tiles;
                out.layouts = record.sampleGrid.layouts;
            }
            else
                out.display.error = "study '" + id + "' has no atlas - it was not sampled on model surfaces";

            std::vector<int64_t> texelOf (samples, -1);
            uint32_t width = 0, height = 0;
            if (record.IsPatchDomain ()) {
                if (!packedPatch) {
                    out.masksError = "study '" + id + "' has no packed patch atlas";
                    return;
                }
                width = record.patchAtlas.Width ();
                height = record.patchAtlas.Height ();
                for (size_t sample = 0; sample < samples; ++sample)
                    texelOf[sample] = record.patchAtlas.TexelOf (record.patchGrid, sample);
            }
            else {
                if (!record.atlas.valid) {
                    out.masksError = "study '" + id + "' has no atlas - it was not sampled on model surfaces";
                    return;
                }
                width = record.atlas.width;
                height = record.atlas.height;
                for (size_t sample = 0; sample < samples && sample < record.atlas.texels.size (); ++sample)
                    texelOf[sample] = record.atlas.texels[sample];
            }
            out.masks = PackStepMasks (samples, steps, lit, texelOf, width, height);
            out.minutes = StepMinutes (record.series);
            out.noon = SolarNoonStep (record.series);
            out.masksOk = true;
        },
        error);
    EXPECT_TRUE (leased) << error;
    return out;
}

// Every store reader against the kernels. Hours must also be the lit steps
// times the step length, so the bits and the hours are one answer.
inline void ExpectStoreMatchesKernels (const std::string& id, const std::string& what)
{
    auto& store = SunStudyStore::Get ();
    const KernelReads k = ReadThroughKernels (id);
    if (::testing::Test::HasFatalFailure ())
        return;
    std::string error;

    std::vector<double> hours, positions, normals;
    std::vector<uint8_t> bits;
    ASSERT_TRUE (store.Results (id, hours, positions, normals, &bits, error)) << what << ": " << error;
    EXPECT_TRUE (SameBytes (hours, k.hours) && SameBytes (bits, k.bits)) << what << " results";

    SunStudyResultSummary summary;
    ASSERT_TRUE (store.Summary (id, summary, error)) << what << ": " << error;
    EXPECT_TRUE (summary.count == k.summary.count && summary.fullyLit == k.summary.fullyLit &&
                 summary.fullyShaded == k.summary.fullyShaded && SameBytes (summary.minHours, k.summary.minHours) &&
                 SameBytes (summary.meanHours, k.summary.meanHours) &&
                 SameBytes (summary.maxHours, k.summary.maxHours) &&
                 SameBytes (summary.daylightHours, k.summary.daylightHours))
        << what << " summary";

    const auto expectImage = [&] (bool ok, uint32_t w, uint32_t h, const std::vector<float>& image,
                                  const std::string& refusal, const KernelImage& kernel, const char* which) {
        EXPECT_EQ (ok, kernel.ok) << what << " " << which << ": " << refusal;
        if (ok && kernel.ok)
            EXPECT_TRUE (w == kernel.width && h == kernel.height && SameBytes (image, kernel.image))
                << what << " " << which;
        else
            EXPECT_EQ (refusal, kernel.error) << what << " " << which;
    };
    uint32_t w = 0, h = 0;
    std::vector<float> image;
    error.clear ();
    bool ok = store.AtlasImage (id, w, h, image, error);
    expectImage (ok, w, h, image, error, k.triangle, "triangle image");
    error.clear ();
    ok = store.PatchAtlasImage (id, w, h, image, error);
    expectImage (ok, w, h, image, error, k.patch, "patch image");

    std::vector<AtlasTile> tiles;
    std::vector<FaceLayout> layouts;
    double spacing = 0, daylight = 0;
    bool converged = false;
    uint64_t generation = 0;
    error.clear ();
    ok = store.DisplayData (id, tiles, layouts, w, h, spacing, image, daylight, converged, generation, error);
    expectImage (ok, w, h, image, error, k.display, "display image");
    if (ok && k.display.ok) {
        ASSERT_EQ (tiles.size (), k.tiles.size ()) << what;
        ASSERT_EQ (layouts.size (), k.layouts.size ()) << what;
        for (size_t f = 0; f < tiles.size (); ++f)
            ASSERT_TRUE (tiles[f].x == k.tiles[f].x && tiles[f].y == k.tiles[f].y &&
                         tiles[f].width == k.tiles[f].width && tiles[f].height == k.tiles[f].height)
                << what << " tile " << f;
        for (size_t f = 0; f < layouts.size (); ++f) {
            const FaceLayout &x = layouts[f], &y = k.layouts[f];
            ASSERT_TRUE (std::memcmp (x.origin, y.origin, sizeof x.origin) == 0 &&
                         std::memcmp (x.uAxis, y.uAxis, sizeof x.uAxis) == 0 &&
                         std::memcmp (x.vAxis, y.vAxis, sizeof x.vAxis) == 0 && x.uStart == y.uStart &&
                         x.vStart == y.vStart && x.columns == y.columns && x.rows == y.rows && x.gridded == y.gridded)
                << what << " layout " << f;
        }
    }

    StepMaskAtlas masks;
    std::vector<uint16_t> minutes;
    uint32_t noon = 0;
    error.clear ();
    ok = store.StepMasks (id, masks, minutes, noon, error);
    EXPECT_EQ (ok, k.masksOk) << what << " masks: " << error;
    if (ok && k.masksOk)
        EXPECT_TRUE (masks.width == k.masks.width && masks.height == k.masks.height && masks.words == k.masks.words &&
                     masks.steps == k.masks.steps && SameBytes (masks.masks, k.masks.masks) && minutes == k.minutes &&
                     noon == k.noon)
            << what << " masks";
    else
        EXPECT_EQ (error, k.masksError) << what << " masks";
}

// The hover reading through the store against the C++ reading kernels.
inline void ExpectReadingMatchesKernels (const std::string& id, uint64_t snapshotId, size_t face, const double point[3],
                                         const std::string& what)
{
    auto& store = SunStudyStore::Get ();
    SunStudyReading expected;
    std::string error;
    ASSERT_TRUE (store.ReadDisplayRecord (
        id, store.Revision (id),
        [&] (const StudyRecord& record) {
            std::vector<double> hours;
            std::string coreError;
            ASSERT_TRUE (record.rustCore != nullptr && record.rustCore->SunHours (hours, coreError)) << coreError;
            expected = record.IsPatchDomain ()
                           ? ReadPatchStudyAt (record.patchGrid, record.gridSpacing, hours, face, point)
                           : ReadTriangleStudyAt (record.sampleGrid, record.gridSpacing, hours, face, point);
        },
        error))
        << error;
    SunStudyReading actual;
    uint8_t role = 0;
    double daylight = 0;
    ASSERT_TRUE (store.ReadAt (id, snapshotId, face, 0, point, actual, role, daylight, error)) << error;
    ASSERT_TRUE (actual.measured == expected.measured && SameBytes (actual.hours, expected.hours) &&
                 actual.sample == expected.sample && actual.column == expected.column && actual.row == expected.row &&
                 actual.nearest == expected.nearest)
        << what << " face " << face;
}

} // namespace evp::sunstudy::fixture

#endif
