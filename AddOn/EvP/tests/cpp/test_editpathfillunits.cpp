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

std::string WithSlope (std::string json, const char* face, const std::string& field)
{
    auto start = json.find (std::string ("\"") + face + "\":");
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

TEST (EditPathFillUnits, CodecReadsEachLegacyFaceIndependentlyAndRefusesMalformedFillAtomically)
{
    const auto doc = PathDocument ();
    std::string json, error;
    ASSERT_TRUE (lg::WriteLandscape (doc, lg::LandscapeStorage::Virtual, json, error)) << error;
    EXPECT_EQ (json.find ("fillSlope"), std::string::npos);
    for (bool oldCut : { false, true })
        for (bool oldFill : { false, true }) {
            auto text = oldCut ? WithSlope (json, "cut", "\"cutSlope\":50") : json;
            if (oldFill)
                text = WithSlope (text, "fill", "\"fillSlope\":33");
            edit::Document loaded;
            ASSERT_TRUE (lg::ReadLandscape (text, loaded, error)) << error;
            EXPECT_EQ (loaded.paths, doc.paths);
            EXPECT_EQ (loaded.landscape, doc.landscape);
        }
    for (const std::string field :
         { "", "\"fillSlope\":33,\"fill\":{\"unit\":2,\"a\":33,\"b\":0}", "\"fill\":{\"unit\":0,\"a\":33,\"b\":0}",
           "\"fill\":{\"unit\":5,\"a\":33,\"b\":0}", "\"fill\":{\"unit\":2,\"a\":0,\"b\":0}",
           "\"fill\":{\"unit\":2,\"a\":-1,\"b\":0}", "\"fill\":{\"unit\":2,\"a\":33,\"b\":1}",
           "\"fill\":{\"unit\":3,\"a\":0,\"b\":0}", "\"fill\":{\"unit\":2,\"a\":33}",
           "\"fill\":{\"unit\":2,\"a\":33,\"b\":0,\"extra\":0}" }) {
        auto loaded = doc;
        EXPECT_FALSE (lg::ReadLandscape (WithSlope (json, "fill", field), loaded, error)) << field;
        if (field.empty ())
            EXPECT_NE (error.find ("exactly one of fill"), std::string::npos) << error;
        EXPECT_EQ (loaded.paths, doc.paths);
        EXPECT_EQ (loaded.landscape, doc.landscape);
    }
}

#ifdef TAPIOCA_GEOMETRY_RUST
TEST (EditPathFillUnits, AuthoredFillPairsKeepTypedCutSurveyHistoryRangeCopiesAndIndependentSourceRevisions)
{
    using Unit = lg::TerrainSlope::Unit;
    for (double offset : { 0.0, 1000000.0, -1000000.0 }) {
        auto doc = PathDocument (offset);
        doc.paths[0].sections[0].cutSlope = { Unit::RiseRun, 1.5, 3 };
        doc.paths[0].sections[0].fillSlope = { Unit::Percent, 20, 0 };
        lg::LandscapeResult rebuilt;
        std::string error;
        ASSERT_TRUE (lg::RebuildLandscape (doc, rebuilt, error)) << error;
        const auto path = doc.paths[0].id;
        const auto before = doc.paths[0].sections[0];
        std::set<uint64_t> revisions;
        for (const lg::TerrainSlope fill : { lg::TerrainSlope { Unit::Percent, 25, 0 },
                                             { Unit::Angle, std::atan (0.25), 0 },
                                             { Unit::RiseRun, 1.5, 6 },
                                             { Unit::Height, 1.5, 6 },
                                             { Unit::Height, 2.5, 10 },
                                             { Unit::Height, 5, 20 },
                                             { Unit::Height, 5, 10 } }) {
            edit::Session session (doc);
            ASSERT_TRUE (session.Apply ([path, fill] (auto& d, auto& e) {
                return edit::EditPath (
                    d, path,
                    [fill] (auto& p, auto&) {
                        p.sections[0].fillSlope = fill;
                        return true;
                    },
                    e);
            })) << session.Message ();
            const auto& after = session.Doc ().paths[0];
            EXPECT_EQ (after.points, doc.paths[0].points);
            EXPECT_EQ (after.profile, doc.paths[0].profile);
            auto unchanged = after.sections[0];
            EXPECT_EQ (unchanged.fillSlope, fill);
            unchanged.fillSlope = before.fillSlope;
            EXPECT_EQ (unchanged, before);
            revisions.insert (session.Doc ().landscape->sources[0].revision);
            std::string json;
            ASSERT_TRUE (lg::WriteLandscape (session.Doc (), lg::LandscapeStorage::Virtual, json, error)) << error;
            edit::Document loaded;
            ASSERT_TRUE (lg::ReadLandscape (json, loaded, error)) << error;
            EXPECT_EQ (loaded.paths, session.Doc ().paths);
            auto ranges = after.sections;
            ASSERT_TRUE (edit::SetTaper (ranges, before.id, true, 3, 2, error)) << error;
            ASSERT_TRUE (edit::SplitSection (ranges, 20, 10, 999, error)) << error;
            edit::RestationSections (ranges, 20, 40);
            for (const auto& range : ranges) {
                EXPECT_EQ (range.fillSlope, fill);
                EXPECT_EQ (range.cutSlope, before.cutSlope);
            }
            EXPECT_EQ (edit::SectionIn (ranges, 40, 25).fillSlope, fill);
            EXPECT_EQ (session.UndoDepth (), 1u);
            ASSERT_TRUE (session.Undo ());
            EXPECT_EQ (session.Doc ().paths, doc.paths);
            ASSERT_TRUE (session.Redo ());
            EXPECT_EQ (session.Doc ().paths[0].sections[0].fillSlope, fill);
        }
        EXPECT_EQ (revisions.size (), 7u) << "unit-only and each-part edits invalidate provenance independently of Cut";
    }
}

TEST (EditPathFillUnits, FillPositiveFinitePolicyAllowsOver1000AndRefusesInvalidOrAggregateOverflow)
{
    using Unit = lg::TerrainSlope::Unit;
    auto doc = PathDocument ();
    doc.paths[0].sections[0].cutSlope = { Unit::Height, 2.5, 5 };
    for (const lg::TerrainSlope bad : { lg::TerrainSlope { Unit::Percent, 0, 0 },
                                        { Unit::Percent, -1, 0 },
                                        { Unit::Percent, 33, 1 },
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
                path.sections[0].fillSlope = bad;
                return true;
            },
            error));
        EXPECT_EQ (changed.paths, doc.paths);
    }
    std::string error;
    for (auto unit : { Unit::Percent, Unit::Angle, Unit::RiseRun, Unit::Height }) {
        lg::TerrainSlope fill;
        ASSERT_TRUE (lg::ConvertTerrainSlope ({ Unit::Percent, 1001, 0 }, unit, 1, fill, error)) << error;
        auto changed = doc;
        ASSERT_TRUE (edit::EditPath (
            changed, doc.paths[0].id,
            [fill] (auto& path, auto&) {
                path.sections[0].fillSlope = fill;
                return true;
            },
            error))
            << error;
        EXPECT_EQ (changed.paths[0].sections[0].fillSlope, fill);
        EXPECT_EQ (changed.paths[0].sections[0].cutSlope, doc.paths[0].sections[0].cutSlope);
    }
    auto changed = doc;
    EXPECT_FALSE (edit::EditPath (
        changed, doc.paths[0].id,
        [] (auto& path, auto&) {
            path.sections[0].cutSlope = { Unit::Percent, std::numeric_limits<double>::max (), 0 };
            path.sections[0].fillSlope = { Unit::Percent, std::numeric_limits<double>::max (), 0 };
            return true;
        },
        error));
    EXPECT_EQ (changed.paths, doc.paths);
}
#endif
