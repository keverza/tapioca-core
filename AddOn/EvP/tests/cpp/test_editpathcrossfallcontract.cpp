#include "Edit/EditLandscape.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditSession.hpp"
#include "Edit/EditTracker.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>

namespace edit = geomsrv::edit;
namespace lg = tapioca::geometry;
namespace {
struct FallCase {
    edit::Fall fall;
    double percent, left, right;
};
constexpr FallCase s_cases[] { { edit::Fall::Left, 2, -0.03, 0.05 },   { edit::Fall::Right, 2, 0.03, -0.05 },
                               { edit::Fall::Crown, 2, -0.03, -0.05 }, { edit::Fall::Left, -2, 0.03, -0.05 },
                               { edit::Fall::Right, -2, -0.03, 0.05 }, { edit::Fall::Crown, -2, 0.03, 0.05 },
                               { edit::Fall::Crown, 0, 0, 0 } };

edit::Document PathDocument (double offset = 0)
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (lg::StartLandscape (doc, { 1 }, error)) << error;
    const auto path = edit::AddPath (doc, { { offset, offset, 10 }, { offset + 20, offset, 11 } }, 4);
    EXPECT_TRUE (edit::EditPath (
        doc, path,
        [] (auto& p, auto&) {
            auto& section = p.sections[0];
            section.widthLeft = section.endLeft = 1.5;
            section.widthRight = section.endRight = 2.5;
            section.crossfall = edit::PercentCrossfall (1.25);
            section.fall = edit::Fall::Right;
            section.cutSlope = { lg::TerrainSlope::Unit::Percent, 65, 0 };
            section.fillSlope = { lg::TerrainSlope::Unit::Percent, 42, 0 };
            return true;
        },
        error))
        << error;
    lg::LandscapeSource source;
    source.pathId = path;
    source.role = lg::SourceRole::PathCenterline;
    lg::Id id = 0;
    EXPECT_TRUE (lg::AddLandscapeSource (doc, source, id, error)) << error;
    lg::LandscapeResult result;
    EXPECT_TRUE (lg::RebuildLandscape (doc, result, error)) << error;
    return doc;
}
} // namespace

TEST (EditPathCrossfallContract, SignedHandsAndCrownMatchAnalyticEdgesFramesAndSurveyOutlines)
{
    for (double offset : { 0.0, 1000000.0, -1000000.0 })
        for (const auto& sample : s_cases) {
            auto doc = PathDocument (offset);
            auto& path = doc.paths[0];
            const auto before = path.sections[0];
            std::string error;
            ASSERT_TRUE (edit::SetPathCrossfall (doc, path.id, before.id, sample.percent, sample.fall, error)) << error;
            const auto& section = path.sections[0];
            EXPECT_EQ (section.crossfall, edit::PercentCrossfall (sample.percent));
            EXPECT_EQ (section.fall, sample.fall);
            auto unchanged = section;
            unchanged.crossfall = before.crossfall;
            unchanged.fall = before.fall;
            EXPECT_EQ (unchanged, before);
            double left = 0, right = 0;
            edit::EdgeDrops (section, left, right);
            EXPECT_NEAR (left, sample.left, 1e-12);
            EXPECT_NEAR (right, sample.right, 1e-12);
            edit::AlignmentGeometry geometry;
            ASSERT_TRUE (edit::ValidatePath (path, geometry, error)) << error;
            edit::StationFrame frame;
            ASSERT_TRUE (edit::Evaluate (path, geometry, 10, frame));
            EXPECT_NEAR (frame.centre.z, 10.5, 1e-12);
            EXPECT_NEAR (frame.leftEdge.z, 10.5 + sample.left, 1e-12);
            EXPECT_NEAR (frame.rightEdge.z, 10.5 + sample.right, 1e-12);
            edit::PathOutline outline;
            ASSERT_TRUE (edit::Outline (path, 2, outline, error)) << error;
            ASSERT_FALSE (outline.stations.empty ());
            for (size_t i = 0; i < outline.stations.size (); ++i) {
                const double centre = 10 + 0.05 * outline.stations[i];
                EXPECT_NEAR (outline.left[i].z, centre + sample.left, 1e-12);
                EXPECT_NEAR (outline.right[i].z, centre + sample.right, 1e-12);
            }
        }
}

TEST (EditPathCrossfallContract, StrictSignedSurfaceBoundaryAndNonfiniteEditsRefuseAtomically)
{
    const auto doc = PathDocument ();
    const auto path = doc.paths[0].id, range = doc.paths[0].sections[0].id;
    const double below = std::nextafter (100.0, 0.0);
    std::string error;
    for (double valid : { 0.0, -0.0, below, -below }) {
        auto changed = doc;
        ASSERT_TRUE (edit::SetPathCrossfall (changed, path, range, valid, edit::Fall::Crown, error)) << error;
        EXPECT_EQ (changed.paths[0].sections[0].crossfall, edit::PercentCrossfall (valid));
        EXPECT_EQ (changed.paths[0].sections[0].crossfall.negative, std::signbit (valid));
    }
    for (double invalid : { 100.0, -100.0, std::nextafter (100.0, 200.0), std::nextafter (-100.0, -200.0),
                            std::numeric_limits<double>::infinity (), -std::numeric_limits<double>::infinity (),
                            std::numeric_limits<double>::quiet_NaN () }) {
        auto changed = doc;
        EXPECT_FALSE (edit::SetPathCrossfall (changed, path, range, invalid, edit::Fall::Left, error));
        EXPECT_EQ (error, "a crossfall must be below 100 %");
        EXPECT_EQ (changed.paths, doc.paths);
        EXPECT_EQ (changed.landscape, doc.landscape);
        EXPECT_EQ (changed.revision, doc.revision);
    }
}

TEST (EditPathCrossfallContract, SignedPercentCodecTrackerHistoryRangeCopiesAndFallEditsKeepTheirDistinctIntent)
{
    const auto doc = PathDocument (1000000);
    const auto path = doc.paths[0].id, range = doc.paths[0].sections[0].id;
    const edit::Dimension dimension { { path, edit::Part::Range, range }, edit::Quantity::Slope };
    for (const auto& sample : s_cases) {
        edit::Session session (doc);
        ASSERT_TRUE (session.Apply ([=] (auto& d, auto& e) {
            return edit::SetPathCrossfall (d, path, range, sample.percent, sample.fall, e);
        })) << session.Message ();
        double value = 0;
        ASSERT_TRUE (edit::ReadDimension (session.Doc (), dimension, value));
        EXPECT_EQ (value, sample.percent);
        std::string json, error;
        ASSERT_TRUE (lg::WriteLandscape (session.Doc (), lg::LandscapeStorage::Virtual, json, error)) << error;
        edit::Document loaded;
        ASSERT_TRUE (lg::ReadLandscape (json, loaded, error)) << error;
        EXPECT_EQ (loaded.paths, session.Doc ().paths);
        auto ranges = loaded.paths[0].sections;
        ASSERT_TRUE (edit::SetTaper (ranges, range, true, 3, 2, error)) << error;
        ASSERT_TRUE (edit::SplitSection (ranges, 20, 10, 999, error)) << error;
        edit::RestationSections (ranges, 20, 40);
        for (const auto& copy : ranges) {
            EXPECT_EQ (copy.crossfall, edit::PercentCrossfall (sample.percent));
            EXPECT_EQ (copy.fall, sample.fall);
            EXPECT_EQ (copy.cutSlope, doc.paths[0].sections[0].cutSlope);
            EXPECT_EQ (copy.fillSlope, doc.paths[0].sections[0].fillSlope);
        }
        EXPECT_EQ (session.UndoDepth (), 1u);
        ASSERT_TRUE (session.Undo ());
        EXPECT_EQ (session.Doc ().paths, doc.paths);
        ASSERT_TRUE (session.Redo ());
        EXPECT_EQ (session.Doc ().paths, loaded.paths);
    }
    edit::Session tracker (doc);
    ASSERT_TRUE (tracker.Apply (edit::WriteDimension (dimension, -3))) << tracker.Message ();
    EXPECT_EQ (tracker.Doc ().paths[0].sections[0].crossfall, edit::PercentCrossfall (-3));
    EXPECT_EQ (tracker.Doc ().paths[0].sections[0].fall, edit::Fall::Right)
        << "tracker/raw APIs preserve signed percent, unlike the on-view label's negative-input shortcut";
    auto changed = tracker.Doc ();
    std::string error;
    ASSERT_TRUE (edit::FlipPathFall (changed, path, range, error)) << error;
    EXPECT_EQ (changed.paths[0].sections[0].crossfall, edit::PercentCrossfall (-3));
    EXPECT_EQ (changed.paths[0].sections[0].fall, edit::Fall::Left);
    ASSERT_TRUE (edit::SetPathFall (changed, path, range, edit::Fall::Crown, error)) << error;
    const auto before = changed.paths;
    EXPECT_FALSE (edit::FlipPathFall (changed, path, range, error));
    EXPECT_EQ (changed.paths, before);
    EXPECT_EQ (changed.paths[0].sections[0].crossfall, edit::PercentCrossfall (-3));
}
