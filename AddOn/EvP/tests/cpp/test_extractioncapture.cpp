// B04: a recording of the extractor's SDK-free seam replays bit-exactly through the C++
// oracle, survives the file format unchanged, and a damaged file never half-loads.
#include "Geometry/ExtractionCapture.hpp"
#include "mesh_extraction_replay.hpp"

#include <gtest/gtest.h>
#include <bit>
#include <limits>

namespace replay = extraction_replay;

namespace {
// The recorder fed exactly as GeometryExtractor::ExtractElement feeds it: a body only when it
// has vertices and polygons, then each polygon, then each convex before the seam.
geomsrv::ExtractionCapture Record (const replay::Fixture& fixture)
{
    geomsrv::ExtractionRecorder recorder;
    geomsrv::ScopedExtractionRecording armed (recorder);
    auto* active = geomsrv::ActiveExtractionRecorder ();
    EXPECT_EQ (active, &recorder);
    active->BeginElement (fixture.guid, fixture.elemType);
    for (const auto& body : fixture.bodies) {
        if (body.positions.empty () || body.polygons.empty ())
            continue;
        active->BeginBody (body.index, body.positions);
        for (const auto& polygon : body.polygons) {
            active->BeginPolygon (polygon.index, polygon.material, polygon.surface, replay::VisibleEdges (polygon));
            for (const auto& convex : polygon.convexes)
                active->Convex (convex);
        }
    }
    EXPECT_FALSE (recorder.Truncated ());
    return recorder.Capture ();
}
replay::Result AsResult (const geomsrv::ExtractionReplay& r)
{
    return { r.mesh, r.counts, r.retained };
}
} // namespace

TEST (ExtractionCapture, ARecordingReplaysExactlyAsTheExtractorEmitted)
{
    for (const auto& fixture : { replay::Curated (), replay::Seeded (1), replay::Seeded (2), replay::Seeded (7) }) {
        const auto capture = Record (fixture);
        ASSERT_EQ (capture.elements.size (), 1u);
        replay::ExpectEqual (AsResult (geomsrv::ReplayExtraction (capture.elements[0])), replay::ReplayCpp (fixture));
    }
}

TEST (ExtractionCapture, TheFileKeepsEveryBitAndReplaysTheSame)
{
    auto capture = Record (replay::Curated ());
    auto seeded = Record (replay::Seeded (3));
    capture.elements.push_back (seeded.elements[0]);
    // Values the SDK can hand over and a text format would not keep: signed zero, a NaN payload.
    capture.elements[0].bodies[0].positions[0] = -0.0;
    capture.elements[0].bodies[0].polygons[0].convexes[0][0].nx = std::bit_cast<double> (uint64_t (0x7ff8000000000123));
    const auto bytes = geomsrv::WriteExtractionCapture (capture);
    geomsrv::ExtractionCapture loaded;
    std::string error;
    ASSERT_TRUE (geomsrv::ReadExtractionCapture (bytes, loaded, error)) << error;
    ASSERT_EQ (loaded.elements.size (), 2u);
    EXPECT_EQ (geomsrv::WriteExtractionCapture (loaded), bytes) << "a second write is byte-identical";
    EXPECT_EQ (std::bit_cast<uint64_t> (loaded.elements[0].bodies[0].positions[0]), std::bit_cast<uint64_t> (-0.0));
    EXPECT_EQ (std::bit_cast<uint64_t> (loaded.elements[0].bodies[0].polygons[0].convexes[0][0].nx),
               uint64_t (0x7ff8000000000123));
    for (size_t i = 0; i < 2; ++i)
        replay::ExpectEqual (AsResult (geomsrv::ReplayExtraction (loaded.elements[i])),
                             AsResult (geomsrv::ReplayExtraction (capture.elements[i])));
}

TEST (ExtractionCapture, ADamagedFileRefusesAndLeavesTheDestinationAlone)
{
    const auto bytes = geomsrv::WriteExtractionCapture (Record (replay::Curated ()));
    geomsrv::ExtractionCapture keep;
    keep.elements.push_back ({ "sentinel", 3, {} });
    std::string error;
    // Every proper prefix is a truncation.
    for (size_t n = 0; n < bytes.size (); ++n) {
        auto out = keep;
        EXPECT_FALSE (geomsrv::ReadExtractionCapture (std::string_view (bytes).substr (0, n), out, error)) << n;
        ASSERT_EQ (out.elements.size (), 1u);
        EXPECT_EQ (out.elements[0].guid, "sentinel");
    }
    auto out = keep;
    EXPECT_FALSE (geomsrv::ReadExtractionCapture (bytes + '\0', out, error));
    EXPECT_NE (error.find ("trailing"), std::string::npos) << error;
    auto wrongMagic = bytes;
    wrongMagic[0] = 'X';
    EXPECT_FALSE (geomsrv::ReadExtractionCapture (wrongMagic, out, error));
    auto wrongVersion = bytes;
    wrongVersion[8] = 2;
    EXPECT_FALSE (geomsrv::ReadExtractionCapture (wrongVersion, out, error));
    // A forged element count is refused against the bytes left, not allocated.
    auto forged = bytes;
    for (size_t i = 12; i < 16; ++i)
        forged[i] = char (0xff);
    EXPECT_FALSE (geomsrv::ReadExtractionCapture (forged, out, error));
    EXPECT_EQ (out.elements[0].guid, "sentinel");
}

TEST (ExtractionCapture, NothingRecordsUnlessArmedAndABudgetStopsGrowth)
{
    EXPECT_EQ (geomsrv::ActiveExtractionRecorder (), nullptr) << "production extraction records nothing";
    geomsrv::ExtractionRecorder outer, inner;
    {
        geomsrv::ScopedExtractionRecording a (outer);
        {
            geomsrv::ScopedExtractionRecording b (inner);
            EXPECT_EQ (geomsrv::ActiveExtractionRecorder (), &inner);
        }
        EXPECT_EQ (geomsrv::ActiveExtractionRecorder (), &outer);
    }
    EXPECT_EQ (geomsrv::ActiveExtractionRecorder (), nullptr);

    geomsrv::ExtractionRecorder small (9);
    small.BeginElement ("budget", 1);
    small.BeginBody (1, std::vector<double> (6, 1.0));
    small.BeginPolygon (1, 0, 0, {});
    small.Convex ({ { 0, 0, 0, 1 } }); // 3 values: 9 in all
    small.Convex ({ { 1, 0, 0, 1 } }); // past the budget
    small.BeginElement ("after", 1);   // nothing grows once truncated
    EXPECT_TRUE (small.Truncated ());
    ASSERT_EQ (small.Capture ().elements.size (), 1u);
    EXPECT_EQ (small.Capture ().elements[0].bodies[0].polygons[0].convexes.size (), 1u);
}
