#include "ArchViz/ViewerEditor.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/EditLayers.hpp"
#include "Edit/EditShape.hpp"
#include "hud_fixture.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace av = geomsrv::archviz;
namespace ed = geomsrv::edit;

TEST (ViewerEditor, ProjectsWithTheViewerRowVectorCameraAndPhysicalExtent)
{
    av::vieweredit::Frame frame;
    frame.width = 800;
    frame.height = 600;
    for (int i : { 0, 5, 10, 15 })
        frame.view[i] = frame.projection[i] = 1;
    frame.view[12] = -.5f;
    float x = 0, y = 0;
    ASSERT_TRUE (frame.Project (.5, 0, .5, x, y));
    EXPECT_FLOAT_EQ (x, 400);
    EXPECT_FLOAT_EQ (y, 300);
    ASSERT_TRUE (frame.Project (1, .5, .5, x, y));
    EXPECT_FLOAT_EQ (x, 600);
    EXPECT_FLOAT_EQ (y, 150);
    EXPECT_FALSE (frame.Project (0, 0, -1, x, y));
    EXPECT_FALSE (frame.Project (0, 0, 2, x, y));
    EXPECT_FALSE (frame.Project (std::numeric_limits<double>::quiet_NaN (), 0, 0, x, y));
    frame.projection[15] = -1;
    EXPECT_FALSE (frame.Project (0, 0, .5, x, y));
    frame.width = 0;
    EXPECT_FALSE (frame.Project (0, 0, .5, x, y));
}

TEST (ViewerEditor, SurfaceHandoffRetainsTheSameSessionSelectionHistoryAndPlaneButCancelsPreview)
{
    namespace hud = av::overlayhud;
    hudtest::Watched view;
    ed::Document document;
    ed::AddPolyline (document, { { 0, 0, 3 }, { 5, 0, 3 } }, false, "survey");
    ASSERT_TRUE (hud::StartVirtualEditor (*view.state, std::move (document), "test", {}, 3, true));
    auto* original = hud::VirtualEditor (*view.state);
    original->owner = "VirtualEditor";
    original->selection.Set ({ { original->edits.Doc ().entities.front ().id, ed::Part::Entity, 0 } });
    ASSERT_TRUE (original->edits.Apply ([] (ed::Document& doc, std::string&) {
        ed::AddPolyline (doc, { { 0, 2, 3 }, { 5, 2, 3 } }, false, "added");
        return true;
    }));
    const auto revision = original->edits.Doc ().revision;
    const auto depth = original->edits.UndoDepth ();
    original->drawing.push_back ({ 2, 4, 3 });
    auto held = hud::TakeVirtualEditor (*view.state);
    ASSERT_EQ (held.get (), original);
    EXPECT_FALSE (hud::VirtualEditorActive (*view.state));
    av::overlaylayers::ClearEverything ();
    hud::ClearState (*view.state); // exactly what stopping the last overlay does
    hud::RestoreVirtualEditor (*view.state, std::move (held));
    ASSERT_EQ (hud::VirtualEditor (*view.state), original);
    EXPECT_EQ (original->edits.Doc ().revision, revision);
    EXPECT_EQ (original->edits.UndoDepth (), depth);
    EXPECT_EQ (original->selection.Items ().size (), 1);
    EXPECT_EQ (original->owner, "VirtualEditor");
    EXPECT_DOUBLE_EQ (original->plane.origin.z, 3);
    EXPECT_TRUE (original->plane.locked);
    EXPECT_TRUE (original->drawing.empty ());
    EXPECT_TRUE (hud::EditLocked (*view.state));
    av::overlaylayers::Layer layer;
    bool clear = false;
    hud::FollowVirtualEditor (*view.state, layer, clear);
    EXPECT_FALSE (clear);
    EXPECT_TRUE (hud::VirtualEditorActive (*view.state));
    hud::StopVirtualEditor (*view.state);
    av::overlaylayers::ClearEverything ();
}

TEST (ViewerEditor, SharedToolsDrawAndUndoThroughTheViewerProjectionProvider)
{
    namespace hud = av::overlayhud;
    hudtest::Watched host;
    ASSERT_TRUE (hud::StartVirtualEditor (*host.state, {}, "viewer", {}, 3, true));
    auto& session = *hud::VirtualEditor (*host.state);
    session.tool = av::edithud::Tool::Polyline;
    av::overlaylayers::Layer layer;
    ASSERT_TRUE (av::edithud::Follow (session, layer));
    av::overlaylayers::Set (layer);
    av::vieweredit::Frame frame;
    frame.width = 1200;
    frame.height = 800;
    for (int i : { 0, 5, 10, 15 })
        frame.view[i] = 1;
    frame.projection[0] = frame.projection[5] = .05f;
    frame.projection[10] = .01f;
    frame.projection[15] = 1;
    auto input = hudtest::At (600, 200);
    input.project = [frame] (double x, double y, double z, float& px, float& py) {
        return frame.Project (x, y, z, px, py);
    };
    const auto lay = [&] () {
        hud::Layout output;
        std::string error;
        EXPECT_TRUE (host.engine.Build ({ &layer.panels[0] }, { "virtual-editor#0" }, 1, input, {}, output, error))
            << error;
        input.buttons.clear ();
        input.keys.clear ();
        bool clear = false;
        if (hud::FollowVirtualEditor (*host.state, layer, clear) && !clear)
            av::overlaylayers::Set (layer);
    };
    for (float x : { 600.f, 750.f, 900.f }) {
        input.x = x;
        lay ();
        input.buttons = { { 0, true } };
        lay ();
        input.buttons = { { 0, false } };
        lay ();
    }
    input.keys = { { 0x0D, true } };
    lay ();
    ASSERT_EQ (session.edits.Doc ().entities.size (), 1);
    EXPECT_EQ (session.edits.Doc ().entities.front ().vertices.size (), 3);
    EXPECT_DOUBLE_EQ (session.edits.Doc ().entities.front ().vertices.front ().p.z, 3);
    EXPECT_EQ (session.edits.UndoDepth (), 1);
    input.editActions = { hud::Input::EditAction::Undo };
    lay ();
    EXPECT_TRUE (session.edits.Doc ().entities.empty ());
    hud::StopVirtualEditor (*host.state);
    av::overlaylayers::ClearEverything ();
}

TEST (ViewerEditor, ViewerDockRequestsTheOverlayWithoutHidingOrReplacingTheEditor)
{
    namespace hud = av::overlayhud;
    hudtest::Watched host;
    hud::OwnPages pages;
    pages.standalone = true;
    pages.viewerSurface = true;
    pages.viewer.active = true;
    host.engine.SetOwnPages (pages);
    const auto layout = host.Lay ({}, hudtest::At (600, 400));
    const auto& dock = layout.dock;
    const float x = 1200 + dock.offset[0] + dock.width * .5f;
    const float y = 400 + dock.offset[1] + dock.width * .5f;
    host.Click ({}, x, y);
    EXPECT_TRUE (hud::TakeViewerRequest (*host.state));
    EXPECT_FALSE (hud::TakeViewerRequest (*host.state));
    EXPECT_TRUE (hud::ContentShown (*host.state));
}
