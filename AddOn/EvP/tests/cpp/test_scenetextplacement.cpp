#include "ArchViz/SceneTextPlacement.hpp"

#include <gtest/gtest.h>

using geomsrv::archviz::ResolveSceneTextPlacement;
using geomsrv::archviz::SceneTextBounds;

TEST (SceneTextPlacement, LeavesInteriorLabelAtItsAnchor)
{
    const auto placement = ResolveSceneTextPlacement ({ 20.0f, 30.0f, 80.0f, 50.0f }, 100.0f, 100.0f, 4.0f, 2.0f, {});
    ASSERT_TRUE (placement.accepted);
    EXPECT_FLOAT_EQ (placement.offsetX, 0.0f);
    EXPECT_FLOAT_EQ (placement.offsetY, 0.0f);
}

TEST (SceneTextPlacement, MovesPartiallyVisibleLabelInsideTargetInset)
{
    const auto topLeft = ResolveSceneTextPlacement ({ -10.0f, -5.0f, 40.0f, 20.0f }, 100.0f, 80.0f, 4.0f, 2.0f, {});
    ASSERT_TRUE (topLeft.accepted);
    EXPECT_FLOAT_EQ (topLeft.bounds.left, 4.0f);
    EXPECT_FLOAT_EQ (topLeft.bounds.top, 4.0f);

    const auto bottomRight = ResolveSceneTextPlacement ({ 70.0f, 65.0f, 110.0f, 90.0f }, 100.0f, 80.0f, 4.0f, 2.0f, {});
    ASSERT_TRUE (bottomRight.accepted);
    EXPECT_FLOAT_EQ (bottomRight.bounds.right, 96.0f);
    EXPECT_FLOAT_EQ (bottomRight.bounds.bottom, 76.0f);
}

TEST (SceneTextPlacement, RejectsOffscreenAndLaterOverlappingLabels)
{
    EXPECT_FALSE (ResolveSceneTextPlacement ({ -80.0f, 10.0f, -20.0f, 30.0f }, 100.0f, 80.0f, 4.0f, 2.0f, {}).accepted);

    const std::vector<SceneTextBounds> occupied = { { 20.0f, 20.0f, 60.0f, 40.0f } };
    EXPECT_FALSE (
        ResolveSceneTextPlacement ({ 61.0f, 20.0f, 90.0f, 40.0f }, 100.0f, 80.0f, 4.0f, 2.0f, occupied).accepted);
    EXPECT_TRUE (
        ResolveSceneTextPlacement ({ 62.0f, 20.0f, 90.0f, 40.0f }, 100.0f, 80.0f, 4.0f, 2.0f, occupied).accepted);
}

TEST (SceneTextPlacement, LeavesOversizedVisibleLabelForTargetClipping)
{
    const auto placement = ResolveSceneTextPlacement ({ -10.0f, 20.0f, 120.0f, 40.0f }, 100.0f, 80.0f, 4.0f, 2.0f, {});
    ASSERT_TRUE (placement.accepted);
    EXPECT_FLOAT_EQ (placement.offsetX, 0.0f);
    EXPECT_FLOAT_EQ (placement.bounds.left, -10.0f);
    EXPECT_FLOAT_EQ (placement.bounds.right, 120.0f);
}
