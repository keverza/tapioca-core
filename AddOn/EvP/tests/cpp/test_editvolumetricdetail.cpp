#include "Edit/EditVolumetricDetail.hpp"

#include "Edit/EditLandscape.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditSession.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <set>

namespace lg = tapioca::geometry;
namespace edit = geomsrv::edit;

namespace {

lg::VolumetricDetail Detail ()
{
    lg::VolumetricDetail detail;
    detail.id = 100;
    detail.name = "Rock";
    detail.kind = lg::VolumetricDetailKind::Rock;
    detail.voxelSize = 0.2;
    detail.revision = 7;
    lg::VolumetricOperation operation;
    operation.id = 1;
    operation.primitive.kind = lg::VolumetricPrimitiveKind::Sphere;
    operation.primitive.centerOrA = { 1.0, 2.0, 3.0 };
    operation.primitive.radius = 2.0;
    operation.materialId = 4;
    detail.entries.emplace_back (operation);
    return detail;
}

edit::Document LandscapeDocument ()
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (lg::StartLandscape (doc, { 1 }, error)) << error;
    return doc;
}

lg::VolumetricDetail GroupedExtrusion ()
{
    lg::VolumetricDetail detail;
    detail.kind = lg::VolumetricDetailKind::Custom;
    detail.name = "Extrusion \"A\" / žolė";
    detail.voxelSize = 0.2;
    lg::VolumetricGroup group;
    group.isolated = true;
    group.profile = lg::VolumetricBlendProfile::Round;
    group.blendRadius = 0.1;
    detail.entries.emplace_back (group);
    lg::VolumetricOperation operation;
    operation.primitive.kind = lg::VolumetricPrimitiveKind::Extrude;
    operation.primitive.centerOrA = { 4, 5, 6 };
    operation.primitive.b = { 7, 8, 9 };
    operation.primitive.frame.origin = { 1, 2, 3 };
    operation.primitive.frame.axes = { lg::Vec3 { 0, 1, 0 }, lg::Vec3 { -1, 0, 0 }, lg::Vec3 { 0, 0, 1 } };
    operation.primitive.half = { 2, 3, 4 };
    operation.primitive.radius = 0.4;
    operation.primitive.halfHeight = 0.75;
    operation.primitive.profile = { { -1.5, -1 }, { 1.5, -1 }, { 1.5, 1 }, { -1.5, 1 } };
    operation.profile = lg::VolumetricBlendProfile::Round;
    operation.blendRadius = 0.2;
    operation.materialId = std::numeric_limits<int32_t>::max ();
    detail.entries.emplace_back (operation);
    return detail;
}

void ExpectSameDocument (const edit::Document& actual, const edit::Document& expected)
{
    EXPECT_EQ (actual.entities, expected.entities);
    EXPECT_EQ (actual.paths, expected.paths);
    EXPECT_EQ (actual.landscape, expected.landscape);
    EXPECT_EQ (actual.layer0, expected.layer0);
    EXPECT_EQ (actual.layers, expected.layers);
    EXPECT_EQ (actual.groups, expected.groups);
    EXPECT_EQ (actual.isolated, expected.isolated);
    EXPECT_EQ (actual.nextId, expected.nextId);
    EXPECT_EQ (actual.revision, expected.revision);
}

std::string Write (const edit::Document& doc, lg::LandscapeStorage mode)
{
    std::string text, error;
    EXPECT_TRUE (lg::WriteLandscape (doc, mode, text, error)) << error;
    return text;
}

} // namespace

TEST (EditVolumetricDetail, DefinitionHashIsDeterministicAndGeometrySensitive)
{
    auto a = Detail ();
    const auto original = lg::HashVolumetricDetail (a);
    EXPECT_EQ (original, lg::HashVolumetricDetail (Detail ()));

    auto b = a;
    auto& operation = std::get<lg::VolumetricOperation> (b.entries.front ());
    operation.primitive.radius += 0.1;
    EXPECT_NE (original, lg::HashVolumetricDetail (b));

    b = a;
    std::get<lg::VolumetricOperation> (b.entries.front ()).materialId = 8;
    EXPECT_NE (original, lg::HashVolumetricDetail (b));

    b = a;
    b.voxelSize = 0.1;
    EXPECT_NE (original, lg::HashVolumetricDetail (b));

    b = a;
    b.anchor = lg::VolumetricTerrainAnchor { 3, 4, 5.0, 6.0, 0.2 };
    EXPECT_NE (original, lg::HashVolumetricDetail (b));
}

TEST (EditVolumetricDetail, IdentityAndPresentationDoNotChangeDefinitionHash)
{
    auto a = Detail ();
    auto b = a;
    b.id = 999;
    b.name = "Different name";
    b.kind = lg::VolumetricDetailKind::Custom;
    b.revision = 99;
    EXPECT_EQ (lg::HashVolumetricDetail (a), lg::HashVolumetricDetail (b));
}

TEST (EditVolumetricDetail, StackOrderAndGroupIsolationAreHashVisible)
{
    auto a = Detail ();
    lg::VolumetricOperation second;
    second.id = 2;
    second.primitive.kind = lg::VolumetricPrimitiveKind::Capsule;
    second.primitive.centerOrA = { -1, 0, 0 };
    second.primitive.b = { 1, 0, 0 };
    second.primitive.radius = 0.5;
    second.operation = lg::VolumetricBoolean::Subtract;
    second.materialId = 5;
    a.entries.emplace_back (second);
    auto b = a;
    std::swap (b.entries[0], b.entries[1]);
    EXPECT_NE (lg::HashVolumetricDetail (a), lg::HashVolumetricDetail (b));

    lg::VolumetricGroup group;
    group.id = 10;
    group.isolated = true;
    a.entries.insert (a.entries.begin (), group);
    b = a;
    std::get<lg::VolumetricGroup> (b.entries.front ()).isolated = false;
    EXPECT_NE (lg::HashVolumetricDetail (a), lg::HashVolumetricDetail (b));
}

TEST (EditVolumetricDetail, RockAndCavePresetsValidateAfterDocumentPlacement)
{
    for (auto preset : { lg::MakeVolumetricRockPreset (), lg::MakeVolumetricCavePreset () }) {
        auto doc = LandscapeDocument ();
        lg::Id id = 0;
        std::string error;
        ASSERT_TRUE (lg::AddVolumetricDetail (doc, preset, id, error)) << error;
        ASSERT_EQ (doc.landscape->volumetricDetails.size (), 1);
        const auto& placed = doc.landscape->volumetricDetails.front ();
        EXPECT_EQ (placed.id, id);
        EXPECT_EQ (placed.revision, 1);
        EXPECT_TRUE (lg::ValidateVolumetricDetail (placed, error)) << error;
        std::set<lg::Id> ids { placed.id };
        for (const auto& entry : placed.entries) {
            const auto entryId = std::get_if<lg::VolumetricOperation> (&entry)
                                     ? std::get<lg::VolumetricOperation> (entry).id
                                     : std::get<lg::VolumetricGroup> (entry).id;
            EXPECT_NE (entryId, lg::kNoId);
            EXPECT_LT (entryId, doc.nextId);
            EXPECT_TRUE (ids.insert (entryId).second);
        }
    }
}

TEST (EditVolumetricDetail, StructuralRefusalsCoverFieldsStackAndProfiles)
{
    auto valid = Detail ();
    std::string error;
    ASSERT_TRUE (lg::ValidateVolumetricDetail (valid, error)) << error;

    auto invalid = valid;
    invalid.voxelSize = std::numeric_limits<double>::infinity ();
    EXPECT_FALSE (lg::ValidateVolumetricDetail (invalid, error));
    invalid = valid;
    std::get<lg::VolumetricOperation> (invalid.entries.front ()).operation = lg::VolumetricBoolean::Subtract;
    EXPECT_FALSE (lg::ValidateVolumetricDetail (invalid, error));
    invalid = valid;
    std::get<lg::VolumetricOperation> (invalid.entries.front ()).materialId = 0;
    EXPECT_FALSE (lg::ValidateVolumetricDetail (invalid, error));
    invalid = valid;
    invalid.entries.push_back (invalid.entries.front ());
    EXPECT_FALSE (lg::ValidateVolumetricDetail (invalid, error));
    invalid = valid;
    invalid.entries.clear ();
    EXPECT_FALSE (lg::ValidateVolumetricDetail (invalid, error));
    invalid = valid;
    invalid.anchor = lg::VolumetricTerrainAnchor {};
    EXPECT_FALSE (lg::ValidateVolumetricDetail (invalid, error));

    auto extrusion = GroupedExtrusion ();
    extrusion.id = 200;
    extrusion.revision = 1;
    std::get<lg::VolumetricGroup> (extrusion.entries.front ()).id = 201;
    auto& operation = std::get<lg::VolumetricOperation> (extrusion.entries.back ());
    operation.id = 202;
    ASSERT_TRUE (lg::ValidateVolumetricDetail (extrusion, error)) << error;
    operation.primitive.profile = { { 0, 0 }, { 2, 2 }, { 0, 2 }, { 2, 0 } };
    EXPECT_FALSE (lg::ValidateVolumetricDetail (extrusion, error));
}

TEST (EditVolumetricDetail, PlaceEditRemoveUseOneExistingSessionHistoryAndStableIds)
{
    edit::Session session (LandscapeDocument ());
    lg::Id id = 0;
    std::string error;
    ASSERT_TRUE (session.Apply ([&] (edit::Document& doc, std::string& e) {
        return lg::AddVolumetricDetail (doc, lg::MakeVolumetricRockPreset (), id, e);
    })) << error;
    const auto placed = session.Doc ().landscape->volumetricDetails.front ();
    const auto placedNextId = session.Doc ().nextId;

    auto edited = placed;
    edited.voxelSize = 0.1;
    lg::VolumetricOperation added;
    added.primitive.kind = lg::VolumetricPrimitiveKind::Sphere;
    added.primitive.centerOrA = { 2, 0, 0 };
    added.primitive.radius = 0.5;
    added.materialId = 1;
    edited.entries.emplace_back (added);
    ASSERT_TRUE (session.Apply ([&] (edit::Document& doc, std::string& e) {
        return lg::SetVolumetricDetail (doc, edited, e);
    })) << error;
    const auto afterEdit = session.Doc ().landscape->volumetricDetails.front ();
    EXPECT_EQ (afterEdit.id, placed.id);
    EXPECT_EQ (afterEdit.revision, placed.revision + 1);
    EXPECT_EQ (std::get<lg::VolumetricOperation> (afterEdit.entries[0]).id,
               std::get<lg::VolumetricOperation> (placed.entries[0]).id);
    EXPECT_NE (std::get<lg::VolumetricOperation> (afterEdit.entries.back ()).id, lg::kNoId);
    EXPECT_GT (session.Doc ().nextId, placedNextId);
    const auto editNextId = session.Doc ().nextId;

    ASSERT_TRUE (session.Undo ());
    ASSERT_EQ (session.Doc ().landscape->volumetricDetails.size (), 1);
    EXPECT_EQ (session.Doc ().landscape->volumetricDetails.front (), placed);
    EXPECT_EQ (session.Doc ().nextId, editNextId);
    ASSERT_TRUE (session.Redo ());
    EXPECT_EQ (session.Doc ().landscape->volumetricDetails.front (), afterEdit);
    EXPECT_EQ (session.Doc ().nextId, editNextId);

    ASSERT_TRUE (session.Apply ([&] (edit::Document& doc, std::string& e) {
        return lg::RemoveVolumetricDetail (doc, id, e);
    })) << error;
    EXPECT_TRUE (session.Doc ().landscape->volumetricDetails.empty ());
    ASSERT_TRUE (session.Undo ());
    EXPECT_EQ (session.Doc ().landscape->volumetricDetails.front (), afterEdit);
    ASSERT_TRUE (session.Redo ());
    EXPECT_TRUE (session.Doc ().landscape->volumetricDetails.empty ());
}

TEST (EditVolumetricDetail, NoOpSetLeavesDocumentAndDetailRevisionsUnchanged)
{
    auto doc = LandscapeDocument ();
    lg::Id id = 0;
    std::string error;
    ASSERT_TRUE (lg::AddVolumetricDetail (doc, lg::MakeVolumetricRockPreset (), id, error)) << error;
    const auto before = doc;
    EXPECT_TRUE (lg::SetVolumetricDetail (doc, doc.landscape->volumetricDetails.front (), error)) << error;
    ExpectSameDocument (doc, before);
}

TEST (EditVolumetricDetail, StaleAndForeignIdentityEditsAreAtomic)
{
    auto doc = LandscapeDocument ();
    lg::Id id = 0;
    std::string error;
    ASSERT_TRUE (lg::AddVolumetricDetail (doc, lg::MakeVolumetricCavePreset (), id, error)) << error;
    const auto before = doc;
    auto edited = doc.landscape->volumetricDetails.front ();
    edited.revision = 99;
    edited.voxelSize = 0.05;
    EXPECT_FALSE (lg::SetVolumetricDetail (doc, edited, error));
    ExpectSameDocument (doc, before);

    edited = before.landscape->volumetricDetails.front ();
    std::get<lg::VolumetricOperation> (edited.entries.front ()).id = 1;
    EXPECT_FALSE (lg::SetVolumetricDetail (doc, edited, error));
    ExpectSameDocument (doc, before);

    edited = before.landscape->volumetricDetails.front ();
    std::get<lg::VolumetricOperation> (edited.entries.front ()).primitive.profile = {
        { std::numeric_limits<double>::quiet_NaN (), 0 }
    };
    EXPECT_FALSE (lg::SetVolumetricDetail (doc, edited, error));
    ExpectSameDocument (doc, before);

    auto malformed = lg::MakeVolumetricRockPreset ();
    std::get<lg::VolumetricOperation> (malformed.entries.front ()).primitive.profile = {
        { std::numeric_limits<double>::infinity (), 0 }
    };
    EXPECT_FALSE (lg::AddVolumetricDetail (doc, malformed, id, error));
    ExpectSameDocument (doc, before);
}

TEST (EditVolumetricDetail, StrictRoundTripPreservesEveryFieldHashAndUnsignedAnchor)
{
    auto doc = LandscapeDocument ();
    lg::Id rockId = 0, extrusionId = 0;
    std::string error;
    ASSERT_TRUE (lg::AddVolumetricDetail (doc, lg::MakeVolumetricRockPreset (), rockId, error)) << error;
    ASSERT_TRUE (lg::AddVolumetricDetail (doc, GroupedExtrusion (), extrusionId, error)) << error;
    auto rock = doc.landscape->volumetricDetails.front ();
    rock.anchor = lg::VolumetricTerrainAnchor { std::numeric_limits<uint64_t>::max () - 1,
                                                 std::numeric_limits<uint64_t>::max (), 1234567.25, -7654321.5, 0.125 };
    ASSERT_TRUE (lg::SetVolumetricDetail (doc, rock, error)) << error;

    for (const auto mode : { lg::LandscapeStorage::Virtual, lg::LandscapeStorage::Native }) {
        const auto text = Write (doc, mode);
        edit::Document loaded;
        ASSERT_TRUE (lg::ReadLandscape (text, loaded, error)) << error;
        ExpectSameDocument (loaded, doc);
        EXPECT_EQ (Write (loaded, mode), text);
        ASSERT_EQ (loaded.landscape->volumetricDetails.size (), doc.landscape->volumetricDetails.size ());
        for (size_t index = 0; index < doc.landscape->volumetricDetails.size (); ++index)
            EXPECT_EQ (lg::HashVolumetricDetail (loaded.landscape->volumetricDetails[index]),
                       lg::HashVolumetricDetail (doc.landscape->volumetricDetails[index]));
    }
}

TEST (EditVolumetricDetail, MissingLegacyArrayReadsEmptyAndMalformedInputNeverMutatesDestination)
{
    auto empty = LandscapeDocument ();
    const auto legacy = [] (std::string text) {
        const std::string field = ",\"volumetricDetails\":[]";
        const auto at = text.find (field);
        EXPECT_NE (at, std::string::npos);
        if (at != std::string::npos)
            text.erase (at, field.size ());
        return text;
    }(Write (empty, lg::LandscapeStorage::Virtual));
    edit::Document loaded;
    std::string error;
    ASSERT_TRUE (lg::ReadLandscape (legacy, loaded, error)) << error;
    ExpectSameDocument (loaded, empty);

    auto doc = LandscapeDocument ();
    lg::Id id = 0;
    ASSERT_TRUE (lg::AddVolumetricDetail (doc, lg::MakeVolumetricRockPreset (), id, error)) << error;
    const auto valid = Write (doc, lg::LandscapeStorage::Virtual);
    const auto destination = loaded;
    const auto replace = [] (std::string text, const std::string& from, const std::string& to) {
        const auto at = text.find (from);
        EXPECT_NE (at, std::string::npos);
        if (at != std::string::npos)
            text.replace (at, from.size (), to);
        return text;
    };
    for (const auto& malformed : {
             replace (valid, "\"volumetricDetails\"", "\"volumetricDetailz\""),
             replace (valid, "\"type\":1", "\"type\":9"),
             replace (valid, "\"kind\":1", "\"kind\":9"),
             replace (valid, "\"voxelSize\":0.15", "\"voxelSize\":0.15,\"extra\":1"),
             replace (valid, "\"revision\":\"1\"", "\"revision\":\"0\""),
         }) {
        auto candidate = destination;
        EXPECT_FALSE (lg::ReadLandscape (malformed, candidate, error)) << malformed;
        ExpectSameDocument (candidate, destination);
    }
}
