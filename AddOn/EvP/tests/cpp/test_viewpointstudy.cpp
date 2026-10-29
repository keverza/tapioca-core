#include "MeshFixtures.hpp"
#include "SunStudy/ViewpointStudy.hpp"
#include "ArchViz/ViewpointStudyController.hpp"
#include "ArchViz/HudGumball.hpp"
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
    settings.contextAll = false;
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
    hud.analysisFocus.tab = analysistabs::kViewpointTab;
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
    settings.contextAll = false;
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
    hud.analysisFocus.tab = analysistabs::kViewpointTab;
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

TEST (ViewpointWorker, WholeSceneContextStopsRaysWithoutAnExplicitList)
{
    view::Shutdown ();
    const auto scene = Scene ();
    view::Settings settings;
    settings.enabled = true;
    ASSERT_TRUE (settings.contextAll) << "the viewer's default Context is the whole scene";
    view::Configure (settings);
    view::Tick (scene);
    ASSERT_TRUE (Wait ());
    auto status = view::GetStatus ();
    ASSERT_NE (status.result, nullptr) << status.error;
    EXPECT_GT (status.result->collisionCount, 0u) << "the wall stops the rays toward it";
    settings.contextAll = false;
    view::Configure (settings);
    EXPECT_EQ (view::GetStatus ().result, nullptr) << "a Context change clears the fill";
    view::Tick (scene);
    ASSERT_TRUE (Wait ());
    status = view::GetStatus ();
    ASSERT_NE (status.result, nullptr) << status.error;
    EXPECT_EQ (status.result->collisionCount, 0u) << "empty explicit Context is the radius circle";
    view::Shutdown ();
}

TEST (ViewpointGumballInteraction, PointVisibilityDragsTheSharedPointEvenWithTheViewpointHidden)
{
    using namespace geomsrv::archviz;
    view::Shutdown ();
    view::Settings settings;
    settings.enabled = false; // "Show viewpoint" off: the isovist is not drawn
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
    float viewMatrix[16], projection[16], matrix[16];
    const float eye[3] = { 0, 0, 10 }, target[3] = { 0, 0, 0 }, up[3] = { 0, 1, 0 };
    LookAtRH (viewMatrix, eye, target, up);
    OrthographicRH (projection, -4, 4, -3, 3, 0.1f, 100);
    Multiply (matrix, viewMatrix, projection);
    const float start[3] = { 0, 0, 10 }, direction[3] = { 0, 0, -1 }, moved[3] = { 1, -2, 10 };
    hud.analysisFocus.tab = analysistabs::kViewpointTab;
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, start, direction);
    EXPECT_FALSE (hud.viewpointDragging) << "a hidden viewpoint has no gumball on its own tab";
    hud.analysisFocus.tab = analysistabs::kPointVisibilityTab;
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, start, direction);
    ASSERT_TRUE (hud.viewpointDragging);
    input.transitionCount = 0;
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, moved, direction);
    EXPECT_DOUBLE_EQ (view::GetSettings ()->options.point[0], 1);
    EXPECT_DOUBLE_EQ (view::GetSettings ()->options.point[1], -2);
    EXPECT_FALSE (view::GetSettings ()->enabled) << "moving the point must not switch the isovist on";
    view::Shutdown ();
}

TEST (ViewpointGumballInteraction, HoverLightsThePartWithoutTakingTheMouse)
{
    using namespace geomsrv::archviz;
    view::Shutdown ();
    view::Settings settings;
    settings.enabled = true;
    settings.options.point = { 0, 0, 0 };
    view::Configure (settings);
    HudState hud;
    hud.analysisFocus.tab = analysistabs::kViewpointTab;
    InputSnapshot input;
    input.inside = true;
    input.x = 400; // over the centre disc
    input.y = 300;
    float viewMatrix[16], projection[16], matrix[16];
    const float eye[3] = { 0, 0, 10 }, target[3] = { 0, 0, 0 }, up[3] = { 0, 1, 0 };
    LookAtRH (viewMatrix, eye, target, up);
    OrthographicRH (projection, -4, 4, -3, 3, 0.1f, 100);
    Multiply (matrix, viewMatrix, projection);
    const float origin[3] = { 0, 0, 10 }, direction[3] = { 0, 0, -1 };
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, origin, direction);
    EXPECT_EQ (hud.viewpointHotPart, 3);
    EXPECT_FALSE (hud.viewpointOwnsMouse) << "hover alone must not block navigation";
    input.x = 50;
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, origin, direction);
    EXPECT_EQ (hud.viewpointHotPart, -1);
    view::Shutdown ();
}

TEST (AdaptiveSlider, RangeIsTheNextOneTwoFiveStepAtLeastTwiceTheValue)
{
    using geomsrv::archviz::viewerhud::AdaptiveSliderRange;
    EXPECT_DOUBLE_EQ (AdaptiveSliderRange (20.0), 50.0);
    EXPECT_DOUBLE_EQ (AdaptiveSliderRange (50.0), 100.0);
    EXPECT_DOUBLE_EQ (AdaptiveSliderRange (100.0), 200.0);
    EXPECT_DOUBLE_EQ (AdaptiveSliderRange (240.0), 500.0);
    EXPECT_DOUBLE_EQ (AdaptiveSliderRange (3.0), 10.0);
    EXPECT_NEAR (AdaptiveSliderRange (0.3), 1.0, 1.0e-12);
}

TEST (ViewpointStudy, FieldOfViewIsAnOpenSectorAboutItsBearing)
{
    ViewpointOptions options;
    options.mode = ViewpointMode::FieldOfView;
    options.bearingDegrees = 90.0;
    options.fieldOfViewDegrees = 60.0;
    const auto result = RunViewpointStudy (Scene (), {}, options);
    ASSERT_TRUE (result.valid) << result.error;
    EXPECT_FALSE (result.Closed ());
    ASSERT_GE (result.perimeter.size (), 2u);
    // Both edges of the angle: 60 and 120 degrees.
    EXPECT_NEAR (
        std::atan2 (result.perimeter.front ()[1] - options.point[1], result.perimeter.front ()[0] - options.point[0]),
        60.0 * 3.14159265358979323846 / 180.0, 1e-9);
    EXPECT_NEAR (
        std::atan2 (result.perimeter.back ()[1] - options.point[1], result.perimeter.back ()[0] - options.point[0]),
        120.0 * 3.14159265358979323846 / 180.0, 1e-9);
    const double sector = 3.14159265358979323846 * options.radius * options.radius / 6.0;
    EXPECT_NEAR (result.area, sector, sector * 0.01);
    EXPECT_DOUBLE_EQ (result.openFraction, 1.0);
    options.fieldOfViewDegrees = 360.0;
    EXPECT_FALSE (RunViewpointStudy (Scene (), {}, options).valid) << "all round is the plan isovist";
}

TEST (ViewpointStudy, SphereMeasuresVolumeAndOpenSkyWithinItsBand)
{
    ViewpointOptions options;
    options.mode = ViewpointMode::Sphere;
    const auto open = RunViewpointStudy (Scene (), {}, options);
    ASSERT_TRUE (open.valid) << open.error;
    EXPECT_EQ (open.perimeter.size (), options.sphereRows * options.sphereColumns);
    const double ball = 4.0 / 3.0 * 3.14159265358979323846 * std::pow (options.radius, 3.0);
    EXPECT_NEAR (open.volume, ball, ball * 1e-9);
    EXPECT_DOUBLE_EQ (open.openFraction, 1.0);

    const auto walled = RunViewpointStudy (Scene (), { "wall" }, options);
    ASSERT_TRUE (walled.valid) << walled.error;
    EXPECT_GT (walled.collisionCount, 0u);
    EXPECT_LT (walled.openFraction, 1.0);
    EXPECT_LT (walled.volume, ball);

    // Only the sky above 80 degrees passes over the 10 m wall 2 m away.
    options.minElevationDegrees = 80.0;
    const auto sky = RunViewpointStudy (Scene (), { "wall" }, options);
    ASSERT_TRUE (sky.valid) << sky.error;
    EXPECT_EQ (sky.collisionCount, 0u);

    options.minElevationDegrees = 90.0;
    EXPECT_FALSE (RunViewpointStudy (Scene (), {}, options).valid) << "an empty band";
}

TEST (ViewpointStudy, SphereSeesOnlyTheNearFaceOfContextWithinItsRadius)
{
    // The wall is the box x 2..2.5; the point at x 0 sees its x = 2 face and
    // nothing of the face behind it.
    ViewpointOptions options;
    options.mode = ViewpointMode::Sphere;
    options.radius = 5.0;
    options.surfaceCellMetres = 0.5;
    const auto walled = RunViewpointStudy (Scene (), { "wall" }, options);
    ASSERT_TRUE (walled.valid) << walled.error;
    ASSERT_FALSE (walled.seenSurface.empty ());
    EXPECT_EQ (walled.seenSurface.size () % 3, 0u);
    for (const auto& corner : walled.seenSurface)
        EXPECT_LT (corner[0], 2.25) << "the far face is hidden behind the near one";
    // Within the radius: the disc the 5 m sphere cuts from the face 2 m away,
    // its border cut where the radius crosses each cell, not stepped by cells.
    const double disc = 3.14159265358979323846 * (25.0 - 4.0);
    EXPECT_GT (walled.seenArea, 0.98 * disc);
    EXPECT_LT (walled.seenArea, 1.01 * disc);
    EXPECT_NEAR (walled.surfaceCellMetres, 0.5, 1e-12);
    for (const auto& corner : walled.seenSurface)
        EXPECT_LT (std::hypot (corner[0], corner[1], corner[2] - options.point[2]), options.radius + 0.006)
            << "no cell sticks out past the radius";

    // Out of reach: nothing seen.
    options.radius = 1.5;
    const auto short_ = RunViewpointStudy (Scene (), { "wall" }, options);
    ASSERT_TRUE (short_.valid) << short_.error;
    EXPECT_TRUE (short_.seenSurface.empty ());
}

TEST (ViewpointStudy, ASpheresShadowBorderFollowsTheOccludersEdge)
{
    // A post x 1..1.2, y -0.5..0.5 between the point and the wall: its front
    // corners cast the shadow's edges onto the wall at y = +-1.
    const auto scene = std::make_shared<geomsrv::Snapshot> (evptest::MakeSnapshot (
        { evptest::MakeBox ("wall", 2, -20, -10, 0.5, 40, 20), evptest::MakeBox ("post", 1, -0.5, -10, 0.2, 1, 20) },
        geomsrv::MeshStore::Get ().NextId ()));
    ViewpointOptions options;
    options.mode = ViewpointMode::Sphere;
    options.point = { 0, 0, 0 };
    options.radius = 5.0;
    options.surfaceCellMetres = 0.5;
    const auto result = RunViewpointStudy (scene, { "wall", "post" }, options);
    ASSERT_TRUE (result.valid) << result.error;
    size_t onBorder = 0;
    for (const auto& corner : result.seenSurface) {
        if (std::abs (corner[0] - 2.0) > 1e-6)
            continue; // the wall's near face only
        EXPECT_GT (std::abs (corner[1]), 0.99) << "nothing seen in the post's shadow";
        onBorder += std::abs (corner[1]) < 1.01 ? 1u : 0u;
    }
    EXPECT_GT (onBorder, 8u) << "the border runs along the shadow's edge, not a cell away from it";
}

TEST (ViewpointStudy, ModesOtherThanPlanNeverReachAKernelWithoutThem)
{
    ViewpointOptions options;
    options.mode = ViewpointMode::FieldOfView;
    options.fieldOfViewDegrees = 30.0;
    const auto result = RunSelectedViewpointStudy (Scene (), {}, options);
    ASSERT_TRUE (result.valid) << result.error;
    EXPECT_LT (result.perimeter.size (), options.rays) << "a sector, not the full plan circle";
}

TEST (ViewpointGumballInteraction, TheConeTargetHasItsOwnGumballAndStopsFollowingFocusWhenDragged)
{
    using namespace geomsrv::archviz;
    view::Shutdown ();
    view::Settings settings;
    settings.options.point = { 0, 0, 0 };
    view::Configure (settings);
    HudState hud;
    hud.analysisFocus.tab = analysistabs::kPointVisibilityTab;
    hud.visibilityView = 0;
    hud.visibilityTarget[0] = 2.0; // 600 px across an 8 m wide, 800 px view
    hud.visibilityTargetSet = true;
    InputSnapshot input;
    input.inside = true;
    input.x = 600;
    input.y = 300;
    input.buttons = kMouseLeft;
    input.transitions[0] = { kMouseLeft, true };
    input.transitionCount = 1;
    float viewMatrix[16], projection[16], matrix[16];
    const float eye[3] = { 0, 0, 10 }, target[3] = { 0, 0, 0 }, up[3] = { 0, 1, 0 };
    LookAtRH (viewMatrix, eye, target, up);
    OrthographicRH (projection, -4, 4, -3, 3, 0.1f, 100);
    Multiply (matrix, viewMatrix, projection);
    const float start[3] = { 2, 0, 10 }, direction[3] = { 0, 0, -1 }, moved[3] = { 3, 1, 10 };
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, start, direction);
    ASSERT_TRUE (hud.viewpointDragging) << "pressing the target's marker selects it and drags it in plan";
    EXPECT_EQ (hud.viewpointDragHandle, 1);
    EXPECT_EQ (hud.viewpointActiveHandle, 1) << "the target now shows its gumball, the point a marker";
    input.transitionCount = 0;
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, moved, direction);
    EXPECT_DOUBLE_EQ (hud.visibilityTarget[0], 3.0);
    EXPECT_DOUBLE_EQ (hud.visibilityTarget[1], 1.0);
    EXPECT_TRUE (hud.visibilityTargetEdited);
    EXPECT_DOUBLE_EQ (view::GetSettings ()->options.point[0], 0.0) << "the point keeps still";
    view::Shutdown ();
}

TEST (ViewpointGumballInteraction, ATargetFacingFieldOfViewTurnsToItsTarget)
{
    using namespace geomsrv::archviz;
    view::Shutdown ();
    view::Settings settings;
    settings.enabled = true;
    settings.options.point = { 0, 0, 0 };
    settings.options.mode = ViewpointMode::FieldOfView;
    view::Configure (settings);
    HudState hud;
    hud.analysisFocus.tab = analysistabs::kViewpointTab;
    hud.viewpointFaceTarget = true;
    hud.visibilityTargetSet = true;
    hud.visibilityTarget[0] = 0.0;
    hud.visibilityTarget[1] = 5.0;
    InputSnapshot input;
    float matrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const float origin[3] = { 0, 0, 10 }, direction[3] = { 0, 0, -1 };
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, origin, direction);
    EXPECT_NEAR (view::GetSettings ()->options.bearingDegrees, 90.0, 1e-9);
    EXPECT_EQ (hud.viewpointActiveHandle, 0) << "the view point stays selected until the target is clicked";
    hud.viewpointFaceTarget = false;
    hud.visibilityTarget[0] = 5.0;
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, origin, direction);
    EXPECT_NEAR (view::GetSettings ()->options.bearingDegrees, 90.0, 1e-9) << "a free bearing ignores the target";
    view::Shutdown ();
}

namespace {
// Top-down, 8 m x 6 m across an 800 x 600 view: 100 px a metre, origin at the centre.
void TopDown (float matrix[16])
{
    float viewMatrix[16], projection[16];
    const float eye[3] = { 0, 0, 10 }, target[3] = { 0, 0, 0 }, up[3] = { 0, 1, 0 };
    geomsrv::archviz::LookAtRH (viewMatrix, eye, target, up);
    geomsrv::archviz::OrthographicRH (projection, -4, 4, -3, 3, 0.1f, 100);
    geomsrv::archviz::Multiply (matrix, viewMatrix, projection);
}

void Press (geomsrv::archviz::InputSnapshot& input, float x, float y, bool down)
{
    using geomsrv::archviz::kMouseLeft;
    input.inside = true;
    input.x = x;
    input.y = y;
    input.buttons = down ? kMouseLeft : 0;
    input.transitions[0] = { kMouseLeft, down };
    input.transitionCount = 1;
}
} // namespace

TEST (ViewpointGumballInteraction, AClickOffBothHandlesDeselectsThemAndADragDoesNot)
{
    using namespace geomsrv::archviz;
    view::Shutdown ();
    view::Settings settings;
    settings.enabled = true;
    settings.options.point = { 0, 0, 0 };
    view::Configure (settings);
    HudState hud;
    hud.analysisFocus.tab = analysistabs::kViewpointTab;
    float matrix[16];
    TopDown (matrix);
    const float origin[3] = { 0, 0, 10 }, direction[3] = { 0, 0, -1 };
    InputSnapshot input;
    Press (input, 100, 100, true);
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, origin, direction);
    EXPECT_FALSE (hud.viewpointOwnsMouse) << "the press belongs to navigation";
    Press (input, 160, 100, false);
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, origin, direction);
    EXPECT_EQ (hud.viewpointActiveHandle, 0) << "an orbit drag keeps the selection";
    Press (input, 100, 100, true);
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, origin, direction);
    Press (input, 101, 100, false);
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, origin, direction);
    EXPECT_EQ (hud.viewpointActiveHandle, -1) << "a click in place selects nothing";
    // The point is now a marker: pressing it selects it and drags it in plan.
    Press (input, 400, 300, true);
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, origin, direction);
    EXPECT_TRUE (hud.viewpointDragging);
    EXPECT_EQ (hud.viewpointActiveHandle, 0);
    EXPECT_EQ (hud.viewpointDragAxis, kGumballPlane);
    view::Shutdown ();
}

TEST (ViewpointGumballInteraction, TheSquareMovesInPlanAndTheCentreSnapsTheTargetOntoTheModel)
{
    using namespace geomsrv::archviz;
    view::Shutdown ();
    const auto scene = Scene (); // the wall: x 2..2.5, z -10..10
    geomsrv::MeshStore::Get ().PublishShared (scene);
    view::Settings settings;
    settings.options.point = { -2, 0, 0 };
    view::Configure (settings);
    HudState hud;
    hud.analysisFocus.tab = analysistabs::kPointVisibilityTab;
    hud.visibilityView = 0;
    hud.visibilityTargetSet = true;
    hud.viewpointActiveHandle = 1;
    float matrix[16];
    TopDown (matrix);
    const float direction[3] = { 0, 0, -1 };
    const auto gizmo = MakeGumball (hud.visibilityTarget, matrix, 800, 600, true);
    ASSERT_TRUE (gizmo.plane);
    const ImVec2 square ((gizmo.square[0].x + gizmo.square[2].x) * 0.5f,
                         (gizmo.square[0].y + gizmo.square[2].y) * 0.5f);
    EXPECT_EQ (HitGumball (gizmo, square.x, square.y), kGumballPlane);

    // The square: in plan, at the target's own height, whatever is under the cursor.
    InputSnapshot input;
    Press (input, square.x, square.y, true);
    const float startRay[3] = { (square.x - 400.0f) / 100.0f, (300.0f - square.y) / 100.0f, 50 };
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, startRay, direction);
    ASSERT_TRUE (hud.viewpointDragging);
    input.transitionCount = 0;
    const float overWall[3] = { startRay[0] + 2.2f, startRay[1], 50 };
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, overWall, direction);
    EXPECT_NEAR (hud.visibilityTarget[0], 2.2, 1e-5);
    EXPECT_DOUBLE_EQ (hud.visibilityTarget[2], 0.0) << "the square keeps the height";
    Press (input, 0, 0, false);
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, overWall, direction);

    // The centre disc: onto the surface under the cursor -- the wall's top.
    hud.visibilityTarget[0] = 0.0;
    Press (input, 400, 300, true);
    const float atTarget[3] = { 0, 0, 50 }, onWall[3] = { 2.2f, 0, 50 };
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, atTarget, direction);
    ASSERT_TRUE (hud.viewpointDragging);
    EXPECT_EQ (hud.viewpointDragAxis, kGumballCentre);
    input.transitionCount = 0;
    viewerhud::ServiceViewpointInteractions (hud, input, matrix, 800, 600, onWall, direction);
    EXPECT_NEAR (hud.visibilityTarget[0], 2.2, 1e-5);
    EXPECT_NEAR (hud.visibilityTarget[2], 10.0, 1e-6) << "on the wall's top face, not at its old height";
    view::Shutdown ();
    geomsrv::MeshStore::Get ().Release ();
}
