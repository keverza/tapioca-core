#include "Edit/EditVolumetricDetail.hpp"

#include <gtest/gtest.h>

namespace lg = tapioca::geometry;

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
