#include "ArchViz/SunStudyDisplayAssembler.hpp"
#include "SunStudy/SunStudyDisplayData.hpp"
#include "SunStudy/SunStudyAtlasReuse.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

using namespace evp::sunstudy;
using geomsrv::archviz::SunStudyDisplayAssembler;
using geomsrv::archviz::SunStudyDisplayOptions;

namespace {
std::unique_ptr<StudyRecord> Record (bool patch = false)
{
    auto record = std::make_unique<StudyRecord> ();
    auto snapshot = std::make_shared<geomsrv::Snapshot> (evptest::MakeSnapshot ({ evptest::MakeBox ("box", 0, 0, 0) }));
    record->snapshot = snapshot;
    record->snapshotId = snapshot->id;
    record->gridSpacing = 0.3;
    record->timestepMinutes = 15;
    record->elementRoles = { 0 };
    record->domain = patch ? SamplingDomain::SurfacePatch : SamplingDomain::TriangleLegacy;
    SurfaceSamplingOptions options;
    options.domain = record->domain;
    options.spacing = record->gridSpacing;
    auto sampling = BuildSurfaceSampling (*snapshot, { 1 }, options);
    record->sampleGrid = std::move (sampling.triangles);
    record->patchGrid = std::move (sampling.patches);
    if (patch) {
        record->positions = record->patchGrid.positions;
        record->normals = record->patchGrid.normals;
        record->patchAtlas.Fit (record->patchGrid);
    }
    else {
        record->positions = record->sampleGrid.positions;
        record->normals = record->sampleGrid.normals;
        record->atlas = BuildStableTriangleAtlas (record->sampleGrid, *snapshot, record->triangleAllocations);
    }
    SunStep step;
    step.time = { 12, 0 };
    step.altitudeDegrees = 90.0;
    step.direction[2] = 1.0;
    record->series = SunSeries::FromSteps ({ step, step, step }, 15);
    record->traversal = std::make_shared<CpuTraversal> (std::make_shared<geomsrv::QueryEngine> (snapshot));
    record->session.Sync ({ 1, record->series.Version (), 1 }, record->series, record->Samples ());
    return record;
}
} // namespace

TEST (SunStudyDisplayPreparation, BothDomainsMatchLegacyHoursAndEveryStepBitWithoutCopyingSamples)
{
    auto& store = SunStudyStore::Get ();
    store.Clear ();
    for (bool patch : { false, true }) {
        auto record = Record (patch);
        const auto snapshot = record->snapshot;
        const auto positions = record->positions;
        const auto id = store.Insert (std::move (record));
        size_t advanced = 0;
        std::string error;
        ASSERT_TRUE (store.Advance (id, 3, 1, 0.001, 0.0, advanced, error));
        ASSERT_EQ (advanced, 3u);
        std::vector<AtlasTile> tiles;
        std::vector<FaceLayout> layouts;
        std::vector<float> image;
        uint32_t width = 0, height = 0;
        double spacing = 0, daylight = 0;
        bool converged = false;
        uint64_t generation = 0;
        ASSERT_TRUE (store.DisplayData (id, tiles, layouts, width, height, spacing, image, daylight, converged,
                                        generation, error));
        StepMaskAtlas steps;
        std::vector<uint16_t> minutes;
        uint32_t noon = 0;
        ASSERT_TRUE (store.StepMasks (id, steps, minutes, noon, error));
        SunStudyDisplayAssembler assembler;
        std::unique_ptr<geomsrv::archviz::SunStudyAtlasUpload> upload;
        ASSERT_TRUE (store.ReadDisplayRecord (
            id, store.Revision (id),
            [&] (const StudyRecord& source) {
                upload = assembler.Prepare (source, *snapshot, {}, error);
                EXPECT_EQ (source.positions, positions);
            },
            error));
        ASSERT_NE (upload, nullptr) << error;
        EXPECT_EQ (*upload->texels, image);
        EXPECT_EQ (*upload->stepMasks, steps.masks);
        EXPECT_EQ (upload->width, width);
        EXPECT_EQ (upload->stepMinutes, minutes);
        EXPECT_EQ (upload->noonStep, noon);
        EXPECT_EQ (upload->Elements ().size (), 1u);
        EXPECT_TRUE (upload->elements.empty ()); // the queue carries a shared immutable map
        store.Erase (id);
    }
    store.Clear ();
}

TEST (SunStudyDisplayPreparation, PaletteDepthAndNonRoleDebugChangesShareImagesBitsAndSideMaps)
{
    auto record = Record ();
    ASSERT_EQ (record->session.Advance (*record->traversal, 3), 3u);
    SunStudyDisplayAssembler assembler;
    std::string error;
    auto initial = assembler.Prepare (*record, *record->snapshot, {}, error);
    ASSERT_NE (initial, nullptr) << error;
    SunStudyDisplayOptions options;
    options.hoursMax = 9.0;
    options.depth = 2;
    options.debug = 3;
    auto next = assembler.Prepare (*record, *record->snapshot, options, error);
    ASSERT_NE (next, nullptr);
    EXPECT_EQ (next->texels, initial->texels);
    EXPECT_EQ (next->stepMasks, initial->stepMasks);
    EXPECT_EQ (next->sharedElements, initial->sharedElements);
    EXPECT_TRUE (next->atlasRegions.empty ());
    EXPECT_TRUE (next->stepRegions.empty ());
    options.debug = 4;
    auto roles = assembler.Prepare (*record, *record->snapshot, options, error);
    ASSERT_NE (roles, nullptr);
    EXPECT_EQ (roles->texels, initial->texels);
    EXPECT_EQ (roles->stepMasks, initial->stepMasks);
    EXPECT_NE (roles->sharedElements, initial->sharedElements);
    EXPECT_EQ (record->displayCache->image, initial->texels);
}

TEST (SunStudyDisplayPreparation, PartialDayCacheInvalidatesOnResolvedStepsEvenWithoutANewSessionGeneration)
{
    auto record = Record (true);
    SunStudyDisplayAssembler assembler;
    std::string error;
    ASSERT_EQ (record->session.Advance (*record->traversal, 1), 1u);
    auto before = assembler.Prepare (*record, *record->snapshot, {}, error);
    ASSERT_NE (before, nullptr);
    const auto generation = record->session.Progress ().generation;
    ASSERT_EQ (record->session.Advance (*record->traversal, 1), 1u);
    auto after = assembler.Prepare (*record, *record->snapshot, {}, error);
    ASSERT_NE (after, nullptr);
    EXPECT_EQ (record->session.Progress ().generation, generation);
    EXPECT_NE (before->texels, after->texels);
    EXPECT_NE (*before->texels, *after->texels);
    EXPECT_NE (*before->stepMasks, *after->stepMasks);
    EXPECT_TRUE (geomsrv::archviz::CanApplySunAtlasRegions (*before, *after));
    EXPECT_FALSE (after->atlasRegions.empty ());
}

TEST (SunStudyDisplayPreparation, CancellationStaleSnapshotsAndIncompletePreviewsCannotProduceUploads)
{
    auto record = Record ();
    SunStudyDisplayAssembler assembler;
    std::string error;
    EXPECT_EQ (assembler.Prepare (*record, *record->snapshot, {}, error, [] { return true; }), nullptr);
    EXPECT_EQ (record->displayCache, nullptr);
    auto stale = *record->snapshot;
    ++stale.id;
    EXPECT_EQ (assembler.Prepare (*record, stale, {}, error), nullptr);
    SunStudyDisplayOptions preview;
    preview.preview = true;
    EXPECT_EQ (assembler.Prepare (*record, *record->snapshot, preview, error), nullptr);
    ASSERT_EQ (record->session.Advance (*record->traversal, 3), 3u);
    auto shown = assembler.Prepare (*record, *record->snapshot, preview, error);
    ASSERT_NE (shown, nullptr);
    const auto image = shown->texels;
    record.reset ();
    EXPECT_GT (image->size (), 0u); // queue ownership survives cancellation/erasure
}

TEST (SunStudyDisplayPreparation, SummaryAndOptionalCopiesPreserveExactLegacyNumerics)
{
    auto& store = SunStudyStore::Get ();
    store.Clear ();
    const auto id = store.Insert (Record ());
    size_t advanced = 0;
    std::string error;
    ASSERT_TRUE (store.Advance (id, 3, 1, 0.001, 0.0, advanced, error));
    ASSERT_EQ (advanced, 3u);
    std::vector<double> hours, positions { 7 }, normals { 8 };
    ASSERT_TRUE (store.Results (id, hours, positions, normals, nullptr, error, false));
    EXPECT_TRUE (positions.empty ());
    EXPECT_TRUE (normals.empty ());
    SunStudyResultSummary summary;
    ASSERT_TRUE (store.Summary (id, summary, error));
    const auto expected = SummarizeSunHours (hours, 0.75);
    EXPECT_EQ (summary.count, hours.size ());
    EXPECT_EQ (summary.fullyLit, expected.fullyLit);
    EXPECT_EQ (summary.fullyShaded, expected.fullyShaded);
    EXPECT_DOUBLE_EQ (summary.minHours, expected.minHours);
    EXPECT_DOUBLE_EQ (summary.meanHours, expected.meanHours);
    EXPECT_DOUBLE_EQ (summary.maxHours, expected.maxHours);
    ASSERT_TRUE (store.Results (id, hours, positions, normals, nullptr, error));
    EXPECT_EQ (positions.size (), hours.size () * 3);
    EXPECT_EQ (normals.size (), positions.size ());
    store.Clear ();
}

TEST (SunStudyDisplayPreparation, MultiwordDaysKeepEveryBinaryStepAndCancelledPackingDiscardsTheWholeImage)
{
    for (bool patch : { false, true }) {
        auto record = Record (patch);
        std::vector<SunStep> day (65);
        for (size_t step = 0; step < day.size (); ++step) {
            day[step].time = { static_cast<int> (step / 4 + 4), static_cast<int> (step % 4 * 15) };
            day[step].altitudeDegrees = 45.0;
            day[step].direction[0] = step % 2 == 0 ? 1.0 : -1.0;
            day[step].direction[2] = 1.0;
        }
        record->series = SunSeries::FromSteps (day, 15);
        record->session.Sync ({ 1, record->series.Version (), 1 }, record->series, record->Samples ());
        ASSERT_EQ (record->session.Advance (*record->traversal, 65), 65u);
        SunStudyDisplayAssembler assembler;
        std::string error;
        auto upload = assembler.Prepare (*record, *record->snapshot, {}, error);
        ASSERT_NE (upload, nullptr) << error;
        EXPECT_EQ (upload->stepWords, 3u);
        const auto hours = record->session.SunHours ();
        const auto& acc = record->session.Accumulator ();
        const size_t plane = static_cast<size_t> (upload->width) * upload->height;
        for (size_t sample = 0; sample < hours.size (); ++sample) {
            const int64_t texel =
                patch ? record->patchAtlas.TexelOf (record->patchGrid, sample) : record->atlas.texels[sample];
            if (texel < 0)
                continue;
            EXPECT_EQ ((*upload->texels)[static_cast<size_t> (texel)], static_cast<float> (hours[sample]));
            for (size_t step = 0; step < 65; ++step)
                EXPECT_EQ (((*upload->stepMasks)[(step >> 5) * plane + static_cast<size_t> (texel)] >> (step & 31u)) &
                               1u,
                           acc.Lit (sample, step) ? 1u : 0u);
        }
    }
    std::vector<int64_t> texels (5000);
    for (size_t sample = 0; sample < texels.size (); ++sample)
        texels[sample] = static_cast<int64_t> (sample);
    size_t polls = 0, reads = 0;
    const auto cancelled = PackStepMasks (
        5000, 65,
        [&] (size_t, size_t) {
            ++reads;
            return true;
        },
        texels, 128, 64, [&] { return ++polls >= 3; });
    EXPECT_TRUE (cancelled.masks.empty ());
    EXPECT_EQ (cancelled.width, 0u);
    EXPECT_EQ (reads, 1024u * 65u);
}

TEST (SunStudyDisplayPreparation, CancelledPartialDayRefreshNeverReplacesThePriorCache)
{
    auto record = Record ();
    SunStudyDisplayAssembler assembler;
    std::string error;
    ASSERT_EQ (record->session.Advance (*record->traversal, 1), 1u);
    auto before = assembler.Prepare (*record, *record->snapshot, {}, error);
    ASSERT_NE (before, nullptr);
    const auto previous = record->displayCache;
    ASSERT_EQ (record->session.Advance (*record->traversal, 1), 1u);
    size_t checks = 0;
    EXPECT_EQ (assembler.Prepare (*record, *record->snapshot, {}, error, [&] { return ++checks >= 3; }), nullptr);
    EXPECT_EQ (record->displayCache, previous);
    auto after = assembler.Prepare (*record, *record->snapshot, {}, error);
    ASSERT_NE (after, nullptr);
    EXPECT_EQ (after->baseTexels, before->texels);
    EXPECT_NE (after->texels, before->texels);
}

TEST (SunStudyDisplayPreparation, ReadyPacketsCannotPublishAfterCancellationOrSameIdReplacement)
{
    auto& store = SunStudyStore::Get ();
    store.Clear ();
    auto source = Record ();
    source->id = "display-record";
    const auto id = store.Insert (std::move (source));
    const auto revision = store.Revision (id);
    size_t enqueued = 0;
    EXPECT_TRUE (store.PublishDisplayRecord (id, revision, [&] { ++enqueued; }));
    ASSERT_TRUE (store.Erase (id));
    EXPECT_FALSE (store.PublishDisplayRecord (id, revision, [&] { ++enqueued; }));
    auto replacement = Record ();
    replacement->id = id;
    ASSERT_EQ (store.Insert (std::move (replacement)), id);
    EXPECT_FALSE (store.PublishDisplayRecord (id, revision, [&] { ++enqueued; }));
    EXPECT_EQ (enqueued, 1u);
    EXPECT_TRUE (store.PublishDisplayRecord (id, store.Revision (id), [&] { ++enqueued; }));
    EXPECT_EQ (enqueued, 2u);
    store.Clear ();
}
