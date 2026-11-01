#include "Edit/EditLandscape.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditSession.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>

namespace edit = geomsrv::edit;
namespace lg = tapioca::geometry;
namespace {
edit::Document PathDocument (double offset = 0)
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (lg::StartLandscape (doc, { 1 }, error)) << error;
    const auto path = edit::AddPath (doc, { { offset, offset, 10 }, { offset + 20, offset, 11 } }, 2);
    lg::LandscapeSource source;
    source.pathId = path;
    source.role = lg::SourceRole::PathCenterline;
    lg::Id id = 0;
    EXPECT_TRUE (lg::AddLandscapeSource (doc, source, id, error)) << error;
    lg::LandscapeResult result;
    EXPECT_TRUE (lg::RebuildLandscape (doc, result, error)) << error;
    return doc;
}

std::string WithCut (std::string json, const std::string& field)
{
    auto start = json.find ("\"cut\":");
    const auto end = json.find ('}', start);
    EXPECT_NE (start, std::string::npos);
    EXPECT_NE (end, std::string::npos);
    if (start == std::string::npos || end == std::string::npos)
        return json;
    size_t length = end - start + 1;
    if (field.empty ()) {
        if (end + 1 < json.size () && json[end + 1] == ',')
            ++length;
        else if (start > 0 && json[start - 1] == ',') {
            --start;
            ++length;
        }
    }
    json.replace (start, length, field);
    return json;
}
} // namespace

TEST (EditPathCutUnits, CodecReadsLegacyAndRefusesMissingAmbiguousOrMalformedCutAtomically)
{
    const auto doc = PathDocument ();
    std::string json, error;
    ASSERT_TRUE (lg::WriteLandscape (doc, lg::LandscapeStorage::Virtual, json, error)) << error;
    EXPECT_EQ (json.find ("cutSlope"), std::string::npos);
    edit::Document legacy;
    ASSERT_TRUE (lg::ReadLandscape (WithCut (json, "\"cutSlope\":50"), legacy, error)) << error;
    EXPECT_EQ (legacy.paths, doc.paths);
    EXPECT_EQ (legacy.landscape, doc.landscape);
    for (const std::string field :
         { "", "\"cutSlope\":50,\"cut\":{\"unit\":2,\"a\":50,\"b\":0}", "\"cut\":{\"unit\":0,\"a\":50,\"b\":0}",
           "\"cut\":{\"unit\":5,\"a\":50,\"b\":0}", "\"cut\":{\"unit\":2,\"a\":0,\"b\":0}",
           "\"cut\":{\"unit\":2,\"a\":-1,\"b\":0}", "\"cut\":{\"unit\":2,\"a\":50,\"b\":1}",
           "\"cut\":{\"unit\":2,\"a\":50}", "\"cut\":{\"unit\":2,\"a\":50,\"b\":0,\"extra\":0}" }) {
        auto loaded = doc;
        EXPECT_FALSE (lg::ReadLandscape (WithCut (json, field), loaded, error)) << field;
        if (field.empty ())
            EXPECT_NE (error.find ("exactly one of cut"), std::string::npos) << error;
        EXPECT_EQ (loaded.paths, doc.paths);
        EXPECT_EQ (loaded.landscape, doc.landscape);
    }
}

#ifdef TAPIOCA_GEOMETRY_RUST
TEST (EditPathCutUnits, AuthoredPairsPersistThroughHistoryRangeCopiesAndSourceRevisionsAtSurveyCoordinates)
{
    using Unit = lg::TerrainSlope::Unit;
    for (double offset : { 0.0, 1000000.0, -1000000.0 }) {
        const auto doc = PathDocument (offset);
        const auto path = doc.paths[0].id;
        const auto before = doc.paths[0].sections[0];
        std::set<uint64_t> revisions;
        for (const lg::TerrainSlope cut : { lg::TerrainSlope { Unit::Percent, 50, 0 },
                                            { Unit::Angle, std::atan (0.5), 0 },
                                            { Unit::RiseRun, 1.5, 3 },
                                            { Unit::Height, 2.5, 5 },
                                            { Unit::Height, 5, 10 } }) {
            auto initial = doc;
            // The first Percent form must also be an edit rather than the default no-op.
            initial.paths[0].sections[0].cutSlope = { Unit::Percent, 33, 0 };
            std::string initialError;
            lg::LandscapeResult rebuilt;
            ASSERT_TRUE (lg::RebuildLandscape (initial, rebuilt, initialError)) << initialError;
            edit::Session session (initial);
            ASSERT_TRUE (session.Apply ([path, cut] (auto& d, auto& error) {
                return edit::EditPath (
                    d, path,
                    [cut] (auto& p, auto&) {
                        p.sections[0].cutSlope = cut;
                        return true;
                    },
                    error);
            })) << session.Message ();
            const auto& after = session.Doc ().paths[0];
            EXPECT_EQ (after.points, doc.paths[0].points);
            EXPECT_EQ (after.profile, doc.paths[0].profile);
            auto unchanged = after.sections[0];
            EXPECT_EQ (unchanged.cutSlope, cut);
            unchanged.cutSlope = before.cutSlope;
            EXPECT_EQ (unchanged, before);
            revisions.insert (session.Doc ().landscape->sources[0].revision);
            std::string json, error;
            ASSERT_TRUE (lg::WriteLandscape (session.Doc (), lg::LandscapeStorage::Virtual, json, error)) << error;
            edit::Document loaded;
            ASSERT_TRUE (lg::ReadLandscape (json, loaded, error)) << error;
            EXPECT_EQ (loaded.paths, session.Doc ().paths);
            auto ranges = after.sections;
            ASSERT_TRUE (edit::SetTaper (ranges, before.id, true, 3, 2, error)) << error;
            ASSERT_TRUE (edit::SplitSection (ranges, 20, 10, 999, error)) << error;
            edit::RestationSections (ranges, 20, 40);
            for (const auto& range : ranges)
                EXPECT_EQ (range.cutSlope, cut);
            EXPECT_EQ (edit::SectionIn (ranges, 40, 25).cutSlope, cut);
            EXPECT_EQ (session.UndoDepth (), 1u);
            ASSERT_TRUE (session.Undo ());
            EXPECT_EQ (session.Doc ().paths, initial.paths);
            ASSERT_TRUE (session.Redo ());
            EXPECT_EQ (session.Doc ().paths[0].sections[0].cutSlope, cut);
        }
        EXPECT_EQ (revisions.size (), 5u) << "unit and both authored parts invalidate source provenance";
    }
}

TEST (EditPathCutUnits, PositiveFinitePolicyAllowsOver1000ButRefusesVerticalInvalidAndOverflowAtomically)
{
    using Unit = lg::TerrainSlope::Unit;
    const auto doc = PathDocument ();
    for (const lg::TerrainSlope bad : { lg::TerrainSlope { Unit::Percent, 0, 0 },
                                        { Unit::Percent, -1, 0 },
                                        { Unit::Percent, 50, 1 },
                                        { Unit::Angle, std::numbers::pi / 2, 0 },
                                        { Unit::RiseRun, 1, 0 },
                                        { Unit::Height, 0, 0 },
                                        { Unit::Percent, std::numeric_limits<double>::infinity (), 0 },
                                        { Unit::Height, 1, std::numeric_limits<double>::quiet_NaN () } }) {
        auto changed = doc;
        std::string error;
        EXPECT_FALSE (edit::EditPath (
            changed, doc.paths[0].id,
            [bad] (auto& path, auto&) {
                path.sections[0].cutSlope = bad;
                return true;
            },
            error));
        EXPECT_EQ (changed.paths, doc.paths);
    }
    std::string error;
    for (auto unit : { Unit::Percent, Unit::Angle, Unit::RiseRun, Unit::Height }) {
        lg::TerrainSlope cut;
        ASSERT_TRUE (lg::ConvertTerrainSlope ({ Unit::Percent, 1001, 0 }, unit, 1, cut, error)) << error;
        auto changed = doc;
        ASSERT_TRUE (edit::EditPath (
            changed, doc.paths[0].id,
            [cut] (auto& path, auto&) {
                path.sections[0].cutSlope = cut;
                return true;
            },
            error))
            << error;
        EXPECT_EQ (changed.paths[0].sections[0].cutSlope, cut);
    }
    auto changed = doc;
    EXPECT_FALSE (edit::EditPath (
        changed, doc.paths[0].id,
        [] (auto& path, auto&) {
            path.sections[0].cutSlope = { Unit::Percent, std::numeric_limits<double>::max (), 0 };
            path.sections[0].fillSlope = std::numeric_limits<double>::max ();
            return true;
        },
        error));
    EXPECT_EQ (changed.paths, doc.paths);
}
#endif
