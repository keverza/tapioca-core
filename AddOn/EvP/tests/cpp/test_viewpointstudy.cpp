#include "MeshFixtures.hpp"
#include "SunStudy/ViewpointStudy.hpp"
#include "ArchViz/ViewpointStudyController.hpp"
#include "ArchViz/PointGumball.hpp"
#include "ArchViz/ViewpointProjection.hpp"
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "Geometry/MeshStore.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <chrono>
#include <thread>

using namespace evp::sunstudy;
namespace view = geomsrv::archviz::viewpointstudy;
namespace {
std::shared_ptr<geomsrv::Snapshot> Scene ()
{
    auto scene = std::make_shared<geomsrv::Snapshot> (evptest::MakeSnapshot (
        { evptest::MakeBox ("wall", 2, -20, -10, 0.5, 40, 20) }, geomsrv::MeshStore::Get ().NextId ()));
    scene->completeModel = true;
    scene->captureStamp = geomsrv::MeshStore::Get ().CaptureStamp ();
    return scene;
}

bool Wait ()
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (view::GetStatus ().calculating) {
        if (std::chrono::steady_clock::now () > deadline)
            return false;
        std::this_thread::yield ();
    }
    return true;
}
} // namespace

TEST (ViewpointStudy, EmptyContextIsTheRadiusCircleAndUnassignedObjectsHaveNoEffect)
{
    ViewpointOptions options;
    const auto result = RunViewpointStudy (Scene (), {}, options);
    ASSERT_TRUE (result.valid) << result.error;
    EXPECT_EQ (result.collisionCount, 0u);
    ASSERT_EQ (result.perimeter.size (), options.rays);
    for (const auto& point : result.perimeter) {
        EXPECT_NEAR (std::hypot (point[0] - options.point[0], point[1] - options.point[1]), options.radius, 1e-8);
        EXPECT_DOUBLE_EQ (point[2], options.point[2]);
    }
    EXPECT_NEAR (result.area, 3.14159265358979323846 * options.radius * options.radius, 0.02);
}

TEST (ViewpointStudy, ContextStopsAtFirstCollisionAndMovingPointChangesThePerimeter)
{
    const auto scene = Scene ();
    ViewpointOptions options;
    const auto blocked = RunViewpointStudy (scene, { "WALL" }, options);
    ASSERT_TRUE (blocked.valid) << blocked.error;
    EXPECT_NEAR (blocked.perimeter.front ()[0], 2.0, 1e-9);
    EXPECT_GT (blocked.collisionCount, 0u);
    EXPECT_LT (blocked.area, 3.14159265358979323846 * options.radius * options.radius);
    options.point[0] = 4.0;
    const auto moved = RunViewpointStudy (scene, { "wall" }, options);
    ASSERT_TRUE (moved.valid) << moved.error;
    EXPECT_NEAR (moved.perimeter.front ()[0], 24.0, 1e-9);
    EXPECT_NEAR (moved.perimeter[options.rays / 2][0], 2.5, 1e-9);
    options.point[2] = 20.0;
    const auto above = RunViewpointStudy (scene, { "wall" }, options);
    ASSERT_TRUE (above.valid) << above.error;
    EXPECT_EQ (above.collisionCount, 0u);
}

TEST (ViewpointStudy, RejectsMissingContextAndNeverPublishesPartialCancellation)
{
    const auto missing = RunViewpointStudy (Scene (), { "deleted" }, {});
    EXPECT_FALSE (missing.valid);
    EXPECT_NE (missing.error.find ("absent"), std::string::npos);
    size_t checks = 0;
    const auto cancelled = RunViewpointStudy (Scene (), {}, {}, [&] { return ++checks == 20; });
    EXPECT_FALSE (cancelled.valid);
    EXPECT_TRUE (cancelled.perimeter.empty ());
    ViewpointOptions invalid;
    invalid.radius = 0;
    EXPECT_FALSE (RunViewpointStudy (Scene (), {}, invalid).valid);
}

TEST (ViewpointStudy, IgnoresNearerUnassignedGeometryAndStopsAtTheNearestContextSurface)
{
    auto scene = std::make_shared<const geomsrv::Snapshot> (evptest::MakeSnapshot (
        { evptest::MakeBox ("unassigned", 1, -20, -10, 0.2, 40, 20), evptest::MakeBox ("far", 5, -20, -10, 0.2, 40, 20),
          evptest::MakeBox ("near", 3, -20, -10, 0.2, 40, 20) },
        geomsrv::MeshStore::Get ().NextId ()));
    const auto result = RunViewpointStudy (scene, { "far", "near" }, {});
    ASSERT_TRUE (result.valid) << result.error;
    EXPECT_NEAR (result.perimeter.front ()[0], 3.0, 1e-9);
}

TEST (ViewpointWorker, CoalescesMovementAndClearsOnCaptureInvalidationAndShutdown)
{
    view::Shutdown ();
    const auto scene = Scene ();
    geomsrv::MeshStore::Get ().PublishShared (scene);
    view::Settings settings;
    settings.enabled = true;
    settings.context = { "wall" };
    for (int move = 0; move < 20; ++move) {
        settings.options.point[0] = 4.0 + move;
        view::Configure (settings);
        view::Tick (scene);
    }
    ASSERT_TRUE (Wait ());
    const auto status = view::GetStatus ();
    ASSERT_NE (status.result, nullptr) << status.error;
    EXPECT_DOUBLE_EQ (status.result->options.point[0], 23.0);
    geomsrv::MeshStore::Get ().BumpCaptureStamp ();
    view::Tick (nullptr);
    EXPECT_EQ (view::GetStatus ().result, nullptr);
    view::Shutdown ();
    EXPECT_FALSE (view::GetSettings ()->enabled);
    EXPECT_EQ (view::GetStatus ().result, nullptr);
    geomsrv::MeshStore::Get ().Release ();
}

TEST (PointGumball, AxisAndPlaneMovesAreMeasuredInWorldMetres)
{
    const double point[3] = { 0, 0, 0 }, origin[3] = { 2, 3, 10 }, direction[3] = { 0, 0, -1 };
    double parameter[3];
    ASSERT_TRUE (geomsrv::archviz::PointGumballParameter (point, 0, origin, direction, parameter));
    EXPECT_DOUBLE_EQ (parameter[0], 2);
    EXPECT_DOUBLE_EQ (parameter[1], 0);
    ASSERT_TRUE (geomsrv::archviz::PointGumballParameter (point, 3, origin, direction, parameter));
    EXPECT_DOUBLE_EQ (parameter[0], 2);
    EXPECT_DOUBLE_EQ (parameter[1], 3);
    EXPECT_DOUBLE_EQ (parameter[2], 0);
    EXPECT_FALSE (geomsrv::archviz::PointGumballParameter (point, 2, origin, direction, parameter));
    ASSERT_TRUE (geomsrv::archviz::PointGumballParameter (point, 1, origin, direction, parameter));
    EXPECT_DOUBLE_EQ (parameter[1], 3);
    const double sideOrigin[3] = { 10, 0, 4 }, sideDirection[3] = { -1, 0, 0 };
    ASSERT_TRUE (geomsrv::archviz::PointGumballParameter (point, 2, sideOrigin, sideDirection, parameter));
    EXPECT_DOUBLE_EQ (parameter[2], 4);
}

TEST (ViewpointGumballInteraction, HudMouseCapturePreventsDraggingAndHoverDoesNotBlockNavigation)
{
    using namespace geomsrv::archviz;
    view::Shutdown ();
    view::Settings settings;
    settings.enabled = true;
    settings.options.point = { 0, 0, 0.5 };
    view::Configure (settings);
    HudState hud;
    InputSnapshot input;
    input.inside = true;
    input.x = 400;
    input.y = 300;
    float matrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const float origin[3] = { 0, 0, 10 }, direction[3] = { 0, 0, -1 };
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, origin, direction);
    EXPECT_FALSE (hud.viewpointOwnsMouse);
    input.buttons = kMouseLeft;
    input.transitionCount = 1;
    input.transitions[0] = { kMouseLeft, true };
    hud.wantsMouse = true;
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, origin, direction);
    EXPECT_FALSE (hud.viewpointDragging);
    EXPECT_FALSE (hud.viewpointOwnsMouse);
    view::Shutdown ();
}

TEST (ViewpointWorker, MovingRetainsTheLastCompleteFillButRoleChangesAndDisableClearIt)
{
    view::Shutdown ();
    const auto scene = Scene ();
    view::Settings settings;
    settings.enabled = true;
    view::Configure (settings);
    view::Tick (scene);
    ASSERT_TRUE (Wait ());
    const auto completed = view::GetStatus ().result;
    ASSERT_NE (completed, nullptr);
    settings.options.point[0] = 5;
    view::Configure (settings);
    EXPECT_EQ (view::GetStatus ().result, completed) << "dragging must not blank the fill every frame";
    view::Tick (scene);
    ASSERT_TRUE (Wait ());
    ASSERT_NE (view::GetStatus ().result, nullptr);
    EXPECT_DOUBLE_EQ (view::GetStatus ().result->options.point[0], 5);
    settings.context = { "wall" };
    view::Configure (settings);
    EXPECT_EQ (view::GetStatus ().result, nullptr);
    settings.enabled = false;
    view::Configure (settings);
    EXPECT_EQ (view::GetStatus ().result, nullptr);
    view::Shutdown ();
}

TEST (ViewpointProjection, CloseZoomClipsTrianglesEvenWhenCentreIsOffScreen)
{
    float matrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const double a[3] = { -2, 0, -0.2 }, b[3] = { 0.8, -0.8, 0.5 }, c[3] = { 0.8, 0.8, 0.5 };
    const auto polygon = geomsrv::archviz::ProjectViewpointTriangle (a, b, c, matrix, 800, 600);
    ASSERT_GE (polygon.count, 3u);
    for (size_t i = 0; i < polygon.count; ++i) {
        EXPECT_GE (polygon.points[i][0], 0);
        EXPECT_LE (polygon.points[i][0], 800);
        EXPECT_GE (polygon.points[i][1], 0);
        EXPECT_LE (polygon.points[i][1], 600);
    }
}

TEST (ViewpointGumballInteraction, DragOwnsTheMouseMovesPointAndReleasesWithoutSelectingGeometry)
{
    using namespace geomsrv::archviz;
    view::Shutdown ();
    view::Settings settings;
    settings.enabled = true;
    settings.options.point = { 0, 0, 0 };
    view::Configure (settings);
    HudState hud;
    InputSnapshot input;
    input.inside = true;
    input.x = 400;
    input.y = 300;
    input.buttons = kMouseLeft;
    input.transitions[0] = { kMouseLeft, true };
    input.transitionCount = 1;
    float view[16], projection[16], matrix[16];
    const float eye[3] = { 0, 0, 10 }, target[3] = { 0, 0, 0 }, up[3] = { 0, 1, 0 };
    LookAtRH (view, eye, target, up);
    OrthographicRH (projection, -4, 4, -3, 3, 0.1f, 100);
    Multiply (matrix, view, projection);
    const float start[3] = { 0, 0, 10 }, direction[3] = { 0, 0, -1 }, moved[3] = { 2, 3, 10 };
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, start, direction);
    ASSERT_TRUE (hud.viewpointDragging);
    EXPECT_TRUE (hud.viewpointOwnsMouse);
    input.transitionCount = 0;
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, moved, direction);
    EXPECT_DOUBLE_EQ (view::GetSettings ()->options.point[0], 2);
    EXPECT_DOUBLE_EQ (view::GetSettings ()->options.point[1], 3);
    input.buttons = 0;
    input.transitions[0] = { kMouseLeft, false };
    input.transitionCount = 1;
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, moved, direction);
    EXPECT_FALSE (hud.viewpointDragging);
    EXPECT_TRUE (hud.viewpointOwnsMouse) << "release belongs to the gumball, not a background selection";
    view::Shutdown ();
}
