#include "Edit/EditLandscape.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPathCrossing.hpp"
#include "Edit/EditPathNetwork.hpp"
#include "Edit/EditSession.hpp"
#include "Edit/EditTracker.hpp"
#include "NodeGraph/Json.hpp"
#include "NativeCommands/VirtualEditorProjection.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <numbers>
#include <regex>
#include <set>

namespace edit = geomsrv::edit;
namespace lg = tapioca::geometry;
namespace {

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
            section.cutSlope = { lg::TerrainSlope::Unit::RiseRun, 1.5, 3 };
            section.fillSlope = { lg::TerrainSlope::Unit::Height, 2.1, 5 };
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

TEST (EditPathCrossfallUnits, FourFormsKeepExactPairsSignFallSurveyHistoryAndIndependentSourceRevisions)
{
    using Unit = lg::TerrainSlope::Unit;
    for (double offset : { 0.0, 1000000.0, -1000000.0 }) {
        auto doc = PathDocument (offset);
        const auto path = doc.paths[0].id, range = doc.paths[0].sections[0].id;
        const auto before = doc.paths[0].sections[0];
        std::set<uint64_t> revisions;
        for (const edit::PathCrossfall crossfall : { edit::PathCrossfall { { Unit::Percent, 2, 0 }, false },
                                                     { { Unit::Angle, std::atan (.02), 0 }, false },
                                                     { { Unit::RiseRun, 1, 50 }, false },
                                                     { { Unit::Height, 1, 50 }, false },
                                                     { { Unit::RiseRun, 1, 50 }, true } }) {
            edit::Session session (doc);
            ASSERT_TRUE (session.Apply ([=] (auto& d, auto& e) {
                return edit::SetPathCrossfall (d, path, range, crossfall, edit::Fall::Right, e);
            })) << session.Message ();
            const auto& after = session.Doc ().paths[0].sections[0];
            EXPECT_EQ (after.crossfall, crossfall);
            EXPECT_EQ (after.fall, edit::Fall::Right);
            auto unchanged = after;
            unchanged.crossfall = before.crossfall;
            unchanged.fall = before.fall;
            EXPECT_EQ (unchanged, before);
            double percent = 0;
            std::string error;
            ASSERT_TRUE (edit::PathCrossfallPercent (crossfall, percent, error)) << error;
            EXPECT_NEAR (percent, crossfall.negative ? -2 : 2, 1e-12);
            const edit::Dimension dimension { { path, edit::Part::Range, range }, edit::Quantity::Slope };
            double projected = 0;
            ASSERT_TRUE (edit::ReadDimension (session.Doc (), dimension, projected));
            EXPECT_EQ (projected, percent);
            revisions.insert (session.Doc ().landscape->sources[0].revision);

            edit::AlignmentGeometry geometry;
            edit::PathPreparation preparation;
            ASSERT_TRUE (edit::PreparePath (session.Doc ().paths[0], geometry, preparation, error)) << error;
            edit::StationFrame frame;
            ASSERT_TRUE (edit::Evaluate (session.Doc ().paths[0], geometry, preparation, 10, frame));
            EXPECT_NEAR (frame.leftEdge.z - frame.centre.z, percent * .015, 1e-12);
            EXPECT_NEAR (frame.rightEdge.z - frame.centre.z, -percent * .025, 1e-12);
            edit::PathOutline outline;
            ASSERT_TRUE (edit::Outline (session.Doc ().paths[0], 2, outline, error)) << error;
            edit::PathOutlineCache cache;
            const edit::PathPreparation* cachedPreparation = nullptr;
            const auto* cached = cache.Get (session.Doc ().paths[0], 2, cachedPreparation);
            ASSERT_NE (cached, nullptr);
            ASSERT_NE (cachedPreparation, nullptr);
            EXPECT_EQ (cachedPreparation->sections, preparation.sections);
            EXPECT_EQ (cachedPreparation->crossfallPercent, preparation.crossfallPercent);
            const auto cachedBuilds = cache.builds;
            ASSERT_NE (cache.Get (session.Doc ().paths[0], 2, cachedPreparation), nullptr);
            EXPECT_EQ (cache.builds, cachedBuilds);

            const auto paving = edit::Pave (session.Doc ().paths, .5, true);
            const auto cachedPaving = edit::Pave (session.Doc ().paths, .5, true, &cache);
            EXPECT_EQ (cachedPaving.vertices, paving.vertices);
            double left = 0, right = 0;
            ASSERT_TRUE (edit::PavedHeight (paving, { offset + 10, offset + 1, 0 }, left));
            ASSERT_TRUE (edit::PavedHeight (paving, { offset + 10, offset - 1, 0 }, right));
            EXPECT_NEAR (left, 10.5 + percent * .01, 1e-9);
            EXPECT_NEAR (right, 10.5 - percent * .01, 1e-9);

            std::string json;
            ASSERT_TRUE (lg::WriteLandscape (session.Doc (), lg::LandscapeStorage::Virtual, json, error)) << error;
            edit::Document loaded;
            ASSERT_TRUE (lg::ReadLandscape (json, loaded, error)) << error;
            EXPECT_EQ (loaded.paths, session.Doc ().paths);
            auto ranges = loaded.paths[0].sections;
            ASSERT_TRUE (edit::SetTaper (ranges, range, true, 3, 2, error)) << error;
            ASSERT_TRUE (edit::SplitSection (ranges, 20, 10, 999, error)) << error;
            edit::RestationSections (ranges, 20, 40);
            for (const auto& copy : ranges) {
                EXPECT_EQ (copy.crossfall, crossfall);
                EXPECT_EQ (copy.cutSlope, before.cutSlope);
                EXPECT_EQ (copy.fillSlope, before.fillSlope);
            }
            ASSERT_TRUE (session.Undo ());
            EXPECT_EQ (session.Doc ().paths, doc.paths);
            ASSERT_TRUE (session.Redo ());
            EXPECT_EQ (session.Doc ().paths, loaded.paths);
        }
        EXPECT_EQ (revisions.size (), 5u) << "unit/parts and sign independently invalidate the live source";
    }
}

TEST (EditPathCrossfallUnits, PreparedMismatchAndInvalidOrBoundaryTypedInputsRefuseWithoutPublishing)
{
    using Unit = lg::TerrainSlope::Unit;
    const auto doc = PathDocument ();
    const auto path = doc.paths[0].id, range = doc.paths[0].sections[0].id;
    edit::AlignmentGeometry geometry;
    edit::PathPreparation preparation;
    std::string error;
    ASSERT_TRUE (edit::PreparePath (doc.paths[0], geometry, preparation, error)) << error;
    auto stale = preparation;
    stale.ready = false;
    edit::StationFrame unchanged;
    EXPECT_FALSE (edit::Evaluate (doc.paths[0], geometry, stale, 10, unchanged));
    stale = preparation;
    stale.sections[0].crossfall.negative = true;
    EXPECT_FALSE (edit::Evaluate (doc.paths[0], geometry, stale, 10, unchanged));
    auto changed = doc.paths[0];
    changed.sections[0].crossfall.negative = true;
    EXPECT_FALSE (edit::Evaluate (changed, geometry, preparation, 10, unchanged));
    stale.sections[0].widthLeft = 9;
    EXPECT_FALSE (edit::Evaluate (doc.paths[0], geometry, stale, 10, unchanged));
    lg::TerrainSlope exactAngle;
    ASSERT_TRUE (lg::ConvertTerrainSlope ({ Unit::Angle, std::atan (1.0), 0 }, Unit::Percent, 1, exactAngle, error))
        << error;
    EXPECT_NEAR (exactAngle.a, 100, 1e-12);
    EXPECT_LT (exactAngle.a, 100);
    auto exactAngleDocument = doc;
    ASSERT_TRUE (edit::SetPathCrossfall (exactAngleDocument, path, range,
                                         edit::PathCrossfall { { Unit::Angle, std::atan (1.0), 0 }, true },
                                         edit::Fall::Crown, error))
        << error;

    const double below = std::nextafter (100.0, 0.0);
    for (double valid : { 0.0, -0.0, below, -below }) {
        auto changed = doc;
        ASSERT_TRUE (edit::SetPathCrossfall (changed, path, range, valid, edit::Fall::Crown, error)) << error;
        EXPECT_EQ (changed.paths[0].sections[0].crossfall, edit::PercentCrossfall (valid));
        EXPECT_EQ (edit::PathCrossfallPercent (changed.paths[0].sections[0].crossfall), valid);
    }
    for (const lg::TerrainSlope bad : { lg::TerrainSlope { Unit::Percent, 100, 0 },
                                        { Unit::Percent, -2, 0 },
                                        { Unit::Percent, std::numeric_limits<double>::infinity (), 0 },
                                        { Unit::Angle, std::numbers::pi / 2, 0 },
                                        { Unit::RiseRun, 1, 0 },
                                        { Unit::Height, 1, 1 },
                                        { Unit::RiseRun, 0, 0 },
                                        { Unit::Height, 0, 0 },
                                        { Unit::Height, 1, std::numeric_limits<double>::quiet_NaN () } }) {
        auto changed = doc;
        const bool accepted =
            edit::SetPathCrossfall (changed, path, range, edit::PathCrossfall { bad, true }, edit::Fall::Crown, error);
        double projected = edit::PathCrossfallPercent ({ bad, true });
        EXPECT_FALSE (accepted) << int (bad.unit) << " parts " << bad.a << ", " << bad.b << " projected "
                                << std::hexfloat << projected << std::defaultfloat << " error " << error;
        EXPECT_EQ (changed.paths, doc.paths);
        EXPECT_EQ (changed.landscape, doc.landscape);
        EXPECT_EQ (changed.revision, doc.revision);
    }
}

TEST (EditPathCrossfallUnits, JsonSignedZeroSurvivesNumericHydration)
{
    const auto parsed = evp::nodegraph::json::Parse ("-0");
    ASSERT_TRUE (parsed.ok) << parsed.error;
    double value = 1;
    ASSERT_TRUE (parsed.value.AsDouble (value));
    EXPECT_EQ (value, 0);
    EXPECT_TRUE (std::signbit (value));
    const auto zero = evp::nodegraph::json::Parse ("0");
    ASSERT_TRUE (zero.ok) << zero.error;
    ASSERT_TRUE (zero.value.AsDouble (value));
    EXPECT_FALSE (std::signbit (value));
}

TEST (EditPathCrossfallUnits, CodecLoadsLegacySignedZeroAndRejectsAmbiguousOrInconsistentTypedProjections)
{
    using Unit = lg::TerrainSlope::Unit;
    auto doc = PathDocument ();
    std::string json, error;
    ASSERT_TRUE (
        edit::SetPathCrossfall (doc, doc.paths[0].id, doc.paths[0].sections[0].id, -0.0, edit::Fall::Crown, error))
        << error;
    ASSERT_TRUE (lg::WriteLandscape (doc, lg::LandscapeStorage::Virtual, json, error)) << error;
    const auto legacy =
        std::regex_replace (std::regex_replace (json, std::regex ("\"crossfallNegative\":(?:true|false),"), ""),
                            std::regex ("\"crossfallSlope\":\\{[^}]+\\},"), "");
    ASSERT_NE (legacy, json);
    edit::Document loaded;
    ASSERT_TRUE (lg::ReadLandscape (legacy, loaded, error)) << error;
    EXPECT_EQ (loaded.paths, doc.paths);
    EXPECT_TRUE (loaded.paths[0].sections[0].crossfall.negative);
    EXPECT_EQ (loaded.paths[0].sections[0].crossfall.slope.a, 0);
    const auto missingSlope = std::regex_replace (json, std::regex ("\"crossfallSlope\":\\{[^}]+\\},"), "");
    const auto missingNegative = std::regex_replace (json, std::regex ("\"crossfallNegative\":(?:true|false),"), "");
    for (std::string text :
         { missingSlope, missingNegative,
           std::regex_replace (json, std::regex ("\"crossfallNegative\":true"), "\"crossfallNegative\":false"),
           std::regex_replace (json, std::regex ("\"crossfall\":-0"), "\"crossfall\":0"),
           std::regex_replace (json, std::regex ("\"crossfallSlope\":\\{[^}]+\\}"),
                               "\"crossfallSlope\":{\"unit\":2,\"a\":-2,\"b\":0}"),
           std::regex_replace (json, std::regex ("\"crossfallSlope\":\\{[^}]+\\}"),
                               "\"crossfallSlope\":{\"unit\":4,\"a\":1,\"b\":1}"),
           std::regex_replace (json, std::regex ("\"crossfallNegative\":true"), "\"crossfallNegative\":1") }) {
        auto candidate = doc;
        EXPECT_FALSE (lg::ReadLandscape (text, candidate, error));
        EXPECT_EQ (candidate.paths, doc.paths);
    }
}

TEST (EditPathCrossfallUnits, TypedCrossfallDrivesATJunctionWithThePreparedSection)
{
    edit::Document doc;
    const auto through = edit::AddPath (doc, { { 0, 0, 10 }, { 20, 0, 10 } }, 3);
    std::string error;
    ASSERT_TRUE (edit::SetPathCrossfall (doc, through, doc.paths[0].sections[0].id,
                                         edit::PathCrossfall { { lg::TerrainSlope::Unit::Height, 1, 50 }, false },
                                         edit::Fall::Right, error))
        << error;
    const auto junction = edit::FindJunctions (doc.paths);
    EXPECT_TRUE (junction.empty ()); // no second path yet: the typed path is still valid
    edit::AddPath (doc, { { 10, 10, 5 }, { 10, 1, 5 } }, 3);
    const auto found = edit::FindJunctions (doc.paths);
    ASSERT_EQ (found.size (), 1u);
    EXPECT_NEAR (found[0].rise, .02, 1e-12); // Right fall raises the left side
}

TEST (EditPathCrossfallUnits, PercentProjectionPreservesOutputOnRefusal)
{
    const edit::PathCrossfall invalid { { lg::TerrainSlope::Unit::Percent, 100, 0 }, true };
    double value = 7;
    std::string error;
    EXPECT_FALSE (edit::PathCrossfallPercent (invalid, value, error));
    EXPECT_EQ (value, 7);
    EXPECT_FALSE (geomsrv::nativecommands::VirtualEditorCrossfallPercent (invalid, value, error));
    EXPECT_EQ (value, 7);
    EXPECT_NE (error.find ("VirtualEditorCrossfall:"), std::string::npos);
    const edit::PathCrossfall typed { { lg::TerrainSlope::Unit::RiseRun, 1, 50 }, true };
    ASSERT_TRUE (geomsrv::nativecommands::VirtualEditorCrossfallPercent (typed, value, error)) << error;
    EXPECT_NEAR (value, -2, 1e-12);
}
