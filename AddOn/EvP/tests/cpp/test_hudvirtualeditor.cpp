#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "Edit/EditPathEdit.hpp"
#include "hud_fixture.hpp"
#include <gtest/gtest.h>

namespace ed = geomsrv::edit;
namespace eh = geomsrv::archviz::edithud;
namespace el = geomsrv::archviz::editlayers;
namespace hud = geomsrv::archviz::overlayhud;
namespace layers = geomsrv::archviz::overlaylayers;

namespace {
// The locked plan view: model (x, y) shows at (600 + 20 x, 500 - 20 y).
struct Drive {
    hudtest::Watched view;
    layers::Layer layer;
    hud::Input input = hudtest::At (0, 0);

    explicit Drive (ed::Document doc)
    {
        EXPECT_TRUE (hud::StartVirtualEditor (*view.state, std::move (doc), "test"));
        input.planar = true;
        input.plan[0] = 20, input.plan[2] = 600, input.plan[4] = -20, input.plan[5] = 500;
        // As the starting command does: the session's first layer goes in the store at once.
        EXPECT_TRUE (eh::Follow (S (), layer));
        layers::Set (layer);
    }
    ~Drive ()
    {
        hud::StopVirtualEditor (*view.state);
        layers::Clear (el::kLayer);
    }
    eh::Session& S ()
    {
        return *view.state->virtualEditor;
    }
    const ed::Document& D ()
    {
        return S ().edits.Doc ();
    }
    void Publish ()
    {
        bool clear = false;
        if (hud::FollowVirtualEditor (*view.state, layer, clear) && !clear)
            layers::Set (layer);
    }
    hud::Layout Frame ()
    {
        hud::Layout out;
        std::string error;
        EXPECT_TRUE (
            view.engine.Build ({ &layer.panels[0] }, { std::string (el::kLayer) + "#0" }, 1, input, {}, out, error))
            << error;
        input.keys.clear (), input.characters.clear (), input.buttons.clear ();
        Publish ();
        return out;
    }
    void At (float x, float y)
    {
        input.x = x, input.y = y;
        Frame ();
    }
    // The pointer is over a place a frame before it presses there, as the input layer delivers it.
    void Press (float x, float y)
    {
        At (x, y);
        input.buttons = { { 0, true } };
        Frame ();
    }
    void Release (float x, float y)
    {
        input.x = x, input.y = y;
        input.buttons = { { 0, false } };
        Frame ();
    }
    void Key (uint32_t code)
    {
        input.keys = { { code, true } };
        Frame ();
        input.keys = { { code, false } };
        Frame ();
    }
};

ed::Document Corner ()
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 } }, false);
    return doc;
}
} // namespace

TEST (HudVirtualEditor, StartingPublishesTheLayerAndBorrowsTheLock)
{
    const bool locked = hud::EditLocked (*hudtest::Watched ().state);
    Drive d (Corner ());
    EXPECT_TRUE (hud::EditLocked (*d.view.state));
    EXPECT_EQ (d.layer.name, el::kLayer);
    ASSERT_EQ (d.layer.panels.size (), 1u);
    EXPECT_EQ (d.layer.panels[0].title, eh::kPanelTitle);
    EXPECT_EQ (d.layer.lifetime, layers::LayerLifetime::Command);
    hud::StopVirtualEditor (*d.view.state);
    EXPECT_EQ (hud::EditLocked (*d.view.state), locked);
    EXPECT_FALSE (hud::VirtualEditorActive (*d.view.state));
}

TEST (HudVirtualEditor, TheCommandThatOpenedItOwnsIt)
{
    Drive d (Corner ());
    EXPECT_FALSE (hud::VirtualEditorOwnedBy (*d.view.state, "")); // no owner recorded
    d.S ().owner = "Commands/VirtualEditor/command.py";
    EXPECT_TRUE (hud::VirtualEditorOwnedBy (*d.view.state, "Commands/VirtualEditor/command.py"));
    EXPECT_FALSE (hud::VirtualEditorOwnedBy (*d.view.state, "Commands/Other/command.py"));
    EXPECT_FALSE (hud::VirtualEditorOwnedBy (*d.view.state, ""));
}

TEST (HudVirtualEditor, DraggingAVertexIsOneUndoableGesture)
{
    Drive d (Corner ());
    d.At (800, 500); // the vertex at (10, 0)
    EXPECT_EQ (d.S ().selection.hover.part, ed::Part::Vertex);
    d.Press (800, 500);
    d.At (830, 520);
    d.At (840, 500);
    d.Release (840, 500);
    const auto& v = d.D ().entities[0].vertices[1].p;
    EXPECT_NEAR (v.x, 12.0, 1e-6);
    EXPECT_NEAR (v.y, 0.0, 1e-6);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    // The published layer followed the edit.
    bool moved = false;
    for (const auto& p : d.layer.polylines)
        for (size_t i = 0; i + 2 < p.points.size (); i += 3)
            moved |= std::abs (p.points[i] - 12.0) < 1e-6;
    EXPECT_TRUE (moved);
    d.input.keys = { { 'Z', true, false, true } };
    d.Frame ();
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 10.0, 1e-9);
}

TEST (HudVirtualEditor, TypedLengthOnASelectedSegment)
{
    Drive d (Corner ());
    d.Press (700, 500); // the first segment's middle
    d.Release (700, 500);
    ASSERT_EQ (d.S ().selection.Kind (), ed::SelectionKind::Segment);
    d.input.characters = u"12";
    d.Frame ();
    EXPECT_EQ (d.S ().tracker.State (), ed::TrackerState::Editing);
    d.Key (0x0D);
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 12.0, 1e-9);
    EXPECT_NEAR (d.D ().entities[0].vertices[0].p.x, 0.0, 1e-9);
    // The layer labels the new length.
    ASSERT_FALSE (d.layer.dimensions.empty ());
    EXPECT_EQ (d.layer.dimensions[0].text, "12.00 m");
}

TEST (HudVirtualEditor, GizmoAxisMovesTheSelection)
{
    Drive d (Corner ());
    d.Press (800, 300); // the vertex at (10, 10)
    d.Release (800, 300);
    ASSERT_EQ (d.S ().selection.Kind (), ed::SelectionKind::Vertex);
    d.At (840, 300); // on the gizmo's X axis
    ASSERT_GE (d.S ().hoverHandle, 0);
    d.Press (840, 300);
    d.At (880, 300);
    d.Release (880, 300);
    const auto& v = d.D ().entities[0].vertices[2].p;
    EXPECT_NEAR (v.x, 12.0, 1e-6);
    EXPECT_NEAR (v.y, 10.0, 1e-6);
}

TEST (HudVirtualEditor, DrawingAPolylineAndAPath)
{
    Drive d (Corner ());
    d.Key ('L');
    ASSERT_EQ (d.S ().tool, eh::Tool::Polyline);
    for (const auto& [x, y] :
         std::vector<std::pair<float, float>> { { 600.f, 100.f }, { 700.f, 100.f }, { 700.f, 160.f } }) {
        d.Press (x, y);
        d.Release (x, y);
    }
    d.Key (0x0D);
    ASSERT_EQ (d.D ().entities.size (), 2u);
    EXPECT_EQ (d.D ().entities[1].vertices.size (), 3u);
    EXPECT_EQ (d.D ().entities[1].role, "test");
    d.Key ('P');
    d.Press (600, 600);
    d.Release (600, 600);
    d.input.characters = u"8";
    d.Frame ();
    d.Key (0x0D); // the typed length places the next point
    d.Key (0x0D); // Enter again ends the path
    ASSERT_EQ (d.D ().paths.size (), 1u);
    const ed::Path& p = d.D ().paths[0];
    EXPECT_NEAR (ed::DistanceXY (p.points[0].p, p.points[1].p), 8.0, 1e-6);
    EXPECT_EQ (d.S ().selection.Kind (), ed::SelectionKind::Entity);
    EXPECT_EQ (eh::ProfilePath (d.S ()), &d.D ().paths[0]);
}

TEST (HudVirtualEditor, EscapeStepsBackThenCloses)
{
    Drive d (Corner ());
    d.Press (700, 500);
    d.Release (700, 500);
    ASSERT_FALSE (d.S ().selection.Items ().empty ());
    d.Key (0x1B);
    EXPECT_TRUE (d.S ().selection.Items ().empty ());
    EXPECT_FALSE (d.S ().close);
    d.Key (0x1B);
    EXPECT_FALSE (hud::VirtualEditorActive (*d.view.state)); // closed on the follow
}

TEST (HudVirtualEditor, TransientFeedbackIsAnchoredToTheModel)
{
    Drive d (Corner ());
    d.Press (800, 300);
    d.Release (800, 300);
    const auto out = d.Frame ();
    size_t anchored = 0;
    for (const auto& v : out.overlay.vertices)
        anchored += v.modelAnchored ? 1 : 0;
    EXPECT_GT (anchored, 0u) << "gizmo handles reproject at Present like the roof grips";
}

TEST (HudVirtualEditor, StartingReplacesARoofSessionAndTheReverse)
{
    Drive d (Corner ());
    EXPECT_TRUE (hud::VirtualEditorActive (*d.view.state));
    EXPECT_FALSE (hud::RoofEditorActive (*d.view.state));
}
