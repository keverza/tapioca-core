#include "Edit/EditLandscape.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditSession.hpp"
#include "ArchViz/EditLayers.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <numbers>

namespace lg = tapioca::geometry;
namespace edit = geomsrv::edit;
namespace {
edit::Document Region ()
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (lg::StartLandscape (doc, { 1 }, error)) << error;
    const auto line = edit::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, true);
    EXPECT_TRUE (lg::SetLandscapeSourceRole (doc, line, lg::SourceRole::RegionBoundary, error)) << error;
    return doc;
}

std::string WithGrade (std::string json, const std::string& field)
{
    auto start = json.find ("\"grade\":");
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

TEST (EditRegionSurface, ContourGradeCodecReadsLegacyAndRefusesMissingAmbiguousOrMalformedInputAtomically)
{
    auto doc = Region ();
    auto surface = *doc.landscape->objects[0].surface;
    surface.elevation = lg::RegionElevationMode::Constant;
    surface.slope = lg::RegionSlopeMode::ContourLine;
    surface.grade = { lg::TerrainSlope::Unit::Percent, 5, 0 };
    std::string json, error;
    ASSERT_TRUE (lg::SetRegionSurface (doc, doc.landscape->objects[0].id, surface, {}, error)) << error;
    ASSERT_TRUE (lg::WriteLandscape (doc, lg::LandscapeStorage::Virtual, json, error)) << error;
    EXPECT_EQ (json.find ("gradePercent"), std::string::npos);
    edit::Document legacy;
    ASSERT_TRUE (lg::ReadLandscape (WithGrade (json, "\"gradePercent\":5"), legacy, error)) << error;
    EXPECT_EQ (legacy.landscape, doc.landscape);
    EXPECT_EQ (legacy.entities, doc.entities);
    for (const std::string field :
         { "", "\"gradePercent\":5,\"grade\":{\"unit\":2,\"a\":5,\"b\":0}", "\"grade\":{\"unit\":0,\"a\":5,\"b\":0}",
           "\"grade\":{\"unit\":5,\"a\":5,\"b\":0}", "\"grade\":{\"unit\":2,\"a\":-1,\"b\":0}",
           "\"grade\":{\"unit\":2,\"a\":1001,\"b\":0}", "\"grade\":{\"unit\":2,\"a\":5,\"b\":1}",
           "\"grade\":{\"unit\":3,\"a\":0,\"b\":0}", "\"grade\":{\"unit\":2,\"a\":5}",
           "\"grade\":{\"unit\":2,\"a\":5,\"b\":0,\"extra\":0}" }) {
        auto loaded = doc;
        EXPECT_FALSE (lg::ReadLandscape (WithGrade (json, field), loaded, error)) << field;
        if (field.empty ())
            EXPECT_NE (error.find ("exactly one of grade"), std::string::npos) << error;
        EXPECT_EQ (loaded.landscape, doc.landscape);
        EXPECT_EQ (loaded.entities, doc.entities);
    }
}

#ifdef TAPIOCA_GEOMETRY_RUST
TEST (EditRegionSurface, ContourGradeFormsPreserveAuthoredPairsHistoryAndAnalyticSurveyPreview)
{
    using Unit = lg::TerrainSlope::Unit;
    for (double offset : { 0.0, 1000000.0, -1000000.0 }) {
        auto doc = Region ();
        for (auto& entity : doc.entities)
            for (auto& vertex : entity.vertices) {
                vertex.p.x += offset;
                vertex.p.y += offset;
            }
        const auto object = doc.landscape->objects[0].id;
        for (auto side : { lg::RegionFallSide::Left, lg::RegionFallSide::Right })
            for (const lg::TerrainSlope grade : { lg::TerrainSlope { Unit::Percent, 5, 0 },
                                                  { Unit::Angle, std::atan (0.05), 0 },
                                                  { Unit::RiseRun, 1.5, 30 },
                                                  { Unit::Height, 2.5, 50 } }) {
                auto surface = *doc.landscape->objects[0].surface;
                surface.elevation = lg::RegionElevationMode::Constant;
                surface.slope = lg::RegionSlopeMode::ContourLine;
                surface.first = { offset, offset, 12 };
                surface.second = { offset + 10, offset, -100 };
                surface.fallsTo = side;
                surface.grade = grade;
                const double sign = side == lg::RegionFallSide::Left ? -1 : 1;
                const auto height = lg::RegionSurfaceElevation (surface, { offset + 3, offset + 10, 0 });
                ASSERT_TRUE (height);
                EXPECT_NEAR (*height, 12 + sign * 0.5, 1e-12);
                EXPECT_DOUBLE_EQ (*lg::RegionSurfaceElevation (surface, { offset + 30, offset, 0 }), 12);
                auto reversed = surface;
                reversed.second.x = offset - 10;
                reversed.fallsTo =
                    side == lg::RegionFallSide::Left ? lg::RegionFallSide::Right : lg::RegionFallSide::Left;
                EXPECT_NEAR (*lg::RegionSurfaceElevation (reversed, { offset + 3, offset + 10, 0 }), *height, 1e-12);
                edit::Session session (doc);
                ASSERT_TRUE (session.Apply ([object, surface] (auto& d, auto& e) {
                    return lg::SetRegionSurface (d, object, surface, {}, e);
                })) << session.Message ();
                EXPECT_EQ (session.Doc ().entities, doc.entities);
                EXPECT_EQ (session.Doc ().landscape->objects[0].terrain, doc.landscape->objects[0].terrain);
                std::string json, error;
                ASSERT_TRUE (lg::WriteLandscape (session.Doc (), lg::LandscapeStorage::Virtual, json, error)) << error;
                edit::Document loaded;
                ASSERT_TRUE (lg::ReadLandscape (json, loaded, error)) << error;
                EXPECT_EQ (loaded.landscape, session.Doc ().landscape);
                EXPECT_EQ (loaded.landscape->objects[0].surface->grade, grade);
                lg::LandscapeResult landscape;
                ASSERT_TRUE (lg::EvaluateLandscape (loaded, landscape)) << landscape.error;
                geomsrv::archviz::editlayers::Feedback feedback;
                feedback.landscape = &landscape;
                geomsrv::archviz::overlaylayers::Layer layer;
                geomsrv::archviz::editlayers::AddLandscape (loaded, feedback, {}, layer);
                ASSERT_EQ (layer.meshes.size (), 1u);
                const auto& points = layer.meshes[0].points;
                for (size_t i = 0; i < points.size (); i += 3)
                    EXPECT_NEAR (points[i + 2], 12 + sign * (points[i + 1] - offset) * 0.05, 1e-10);
                EXPECT_EQ (session.UndoDepth (), 1u);
                ASSERT_TRUE (session.Undo ());
                EXPECT_EQ (session.Doc ().landscape, doc.landscape);
                ASSERT_TRUE (session.Redo ());
                EXPECT_EQ (session.Doc ().landscape->objects[0].surface, surface);
            }
    }
}

TEST (EditRegionSurface, ContourGradeAllowsZeroInEveryFormButRefusesVerticalAndInvalidWithoutPublication)
{
    using Unit = lg::TerrainSlope::Unit;
    lg::RegionSurface surface;
    surface.elevation = lg::RegionElevationMode::Constant;
    surface.slope = lg::RegionSlopeMode::ContourLine;
    surface.first.z = 12;
    std::string error;
    for (const lg::TerrainSlope zero : { lg::TerrainSlope { Unit::Percent, 0, 0 },
                                         { Unit::Angle, 0, 0 },
                                         { Unit::RiseRun, 0, 3 },
                                         { Unit::Height, 0, 5 } }) {
        surface.grade = zero;
        lg::PreparedRegionSurface prepared;
        ASSERT_TRUE (lg::PrepareRegionSurface (surface, prepared, error)) << error;
        EXPECT_DOUBLE_EQ (*prepared.Elevation ({ 5, 10, 0 }), 12);
        EXPECT_EQ (surface.grade, zero);
        EXPECT_DOUBLE_EQ (*lg::RegionSurfaceElevation (surface, { 5, 10, 0 }), 12);
        auto zeroDoc = Region ();
        ASSERT_TRUE (lg::SetRegionSurface (zeroDoc, zeroDoc.landscape->objects[0].id, surface, {}, error)) << error;
        std::string json;
        ASSERT_TRUE (lg::WriteLandscape (zeroDoc, lg::LandscapeStorage::Virtual, json, error)) << error;
        edit::Document loaded;
        ASSERT_TRUE (lg::ReadLandscape (json, loaded, error)) << error;
        EXPECT_EQ (loaded.landscape, zeroDoc.landscape);
    }
    const auto doc = Region ();
    for (const lg::TerrainSlope bad : { lg::TerrainSlope { Unit::Angle, std::numbers::pi / 2, 0 },
                                        { Unit::RiseRun, 1, 0 },
                                        { Unit::Height, 0, 0 },
                                        { Unit::Percent, -0.1, 0 },
                                        { Unit::Percent, 1000.000001, 0 },
                                        { Unit::RiseRun, 11, 1 },
                                        { Unit::Percent, std::numeric_limits<double>::infinity (), 0 },
                                        { Unit::Height, 1, std::numeric_limits<double>::quiet_NaN () } }) {
        surface.grade = bad;
        lg::PreparedRegionSurface prepared;
        auto valid = surface;
        valid.grade = { Unit::Percent, 5, 0 };
        ASSERT_TRUE (lg::PrepareRegionSurface (valid, prepared, error)) << error;
        EXPECT_FALSE (lg::PrepareRegionSurface (surface, prepared, error));
        EXPECT_NEAR (*prepared.Elevation ({ 5, 10, 0 }), 12.5, 1e-12);
        auto changed = doc;
        EXPECT_FALSE (lg::SetRegionSurface (changed, doc.landscape->objects[0].id, surface, {}, error));
        EXPECT_EQ (changed.landscape, doc.landscape);
        EXPECT_EQ (changed.entities, doc.entities);
    }
    for (auto unit : { Unit::Percent, Unit::Angle, Unit::RiseRun, Unit::Height }) {
        ASSERT_TRUE (lg::ConvertTerrainSlope ({ Unit::Percent, 1000, 0 }, unit, 1, surface.grade, error)) << error;
        const auto authored = surface;
        EXPECT_TRUE (lg::ValidateRegionSurface (surface, error)) << error;
        EXPECT_EQ (surface, authored);
        lg::PreparedRegionSurface prepared;
        ASSERT_TRUE (lg::PrepareRegionSurface (surface, prepared, error)) << error;
        EXPECT_NEAR (*prepared.Elevation ({ 5, 10, 0 }), 112, 1e-10);
        const auto height = lg::RegionSurfaceElevation (surface, { 5, 10, 0 });
        ASSERT_TRUE (height);
        EXPECT_NEAR (*height, 112, 1e-10);
    }
}
#endif
