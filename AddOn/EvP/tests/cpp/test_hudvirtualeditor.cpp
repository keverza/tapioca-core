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
    void RightClick (float x, float y)
    {
        At (x, y);
        input.buttons = { { 1, true } };
        Frame ();
        input.buttons = { { 1, false } };
        Frame ();
    }
    bool Offers (const std::string& id)
    {
        for (const auto& item : eh::MenuItems (S ()))
            if (item.id == id)
                return item.enabled;
        return false;
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
    // The view labels the new length; the layer carries no dimension of its own.
    EXPECT_TRUE (d.layer.dimensions.empty ());
    const auto labels = el::LiveDimensions (d.D (), d.S ().selection.Items ().front ());
    ASSERT_FALSE (labels.empty ());
    EXPECT_EQ (labels[0].text, "12.00 m");
    EXPECT_TRUE (labels[0].line);
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

TEST (HudVirtualEditor, RightClickMenuFollowsWhatIsUnderThePointer)
{
    Drive d (Corner ());
    // On the empty view: tools, selection and closing the editor.
    d.RightClick (300, 200);
    ASSERT_TRUE (d.S ().menu.open);
    EXPECT_EQ (d.S ().menu.target, eh::MenuTarget::View);
    EXPECT_TRUE (d.Offers ("close"));
    EXPECT_TRUE (d.Offers ("select.all"));
    EXPECT_FALSE (d.Offers ("undo")); // nothing done yet
    EXPECT_FALSE (d.Offers ("segment.insert"));
    // On a segment: it is selected and the menu is the segment's.
    d.RightClick (700, 500);
    EXPECT_EQ (d.S ().menu.target, eh::MenuTarget::Segment);
    EXPECT_EQ (d.S ().selection.Kind (), ed::SelectionKind::Segment);
    EXPECT_TRUE (d.Offers ("segment.insert"));
    EXPECT_FALSE (d.Offers ("close"));
    // On a vertex: its own actions.
    d.RightClick (800, 300);
    EXPECT_EQ (d.S ().menu.target, eh::MenuTarget::Vertex);
    EXPECT_TRUE (d.Offers ("action.delete-vertex"));
    EXPECT_FALSE (d.Offers ("segment.insert"));
}

TEST (HudVirtualEditor, MenuItemsEditAndUndo)
{
    Drive d (Corner ());
    d.RightClick (660, 500); // a third of the way along the first segment
    ASSERT_EQ (d.S ().menu.target, eh::MenuTarget::Segment);
    eh::RunMenuItem (d.S (), "segment.insert");
    ASSERT_EQ (d.D ().entities[0].vertices.size (), 4u);
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 3.0, 1e-6); // where the pointer was, not the middle
    EXPECT_TRUE (d.Offers ("undo"));
    eh::RunMenuItem (d.S (), "undo");
    EXPECT_EQ (d.D ().entities[0].vertices.size (), 3u);
    EXPECT_TRUE (d.Offers ("redo"));
    d.RightClick (300, 200);
    eh::RunMenuItem (d.S (), "select.all");
    EXPECT_EQ (d.S ().selection.Items ().size (), 1u);
    eh::RunMenuItem (d.S (), "close");
    EXPECT_TRUE (d.S ().close);
}

TEST (HudVirtualEditor, PathMenusOfferThePathsOwnEdits)
{
    ed::Document doc;
    ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 1 } }, 2.0);
    Drive d (std::move (doc));
    d.RightClick (700, 500); // on the centreline at x = 5
    ASSERT_TRUE (d.S ().menu.open);
    const auto target = d.S ().menu.target;
    EXPECT_TRUE (target == eh::MenuTarget::Path || target == eh::MenuTarget::Range) << int (target);
    EXPECT_TRUE (d.Offers ("path.splitRange"));
    const size_t breaks = d.D ().paths[0].profile.size ();
    if (target == eh::MenuTarget::Path) {
        eh::RunMenuItem (d.S (), "path.insertBreak");
        EXPECT_EQ (d.D ().paths[0].profile.size (), breaks + 1);
    }
}

TEST (HudVirtualEditor, GumballFollowsTheShapeAndKeepsAMovedPivot)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 6, 6, 0 }, { 4, 8, 0 }, { -2, 2, 0 } }, true); // turned 45 degrees
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    ed::Frame f = eh::GizmoFrame (d.S ());
    EXPECT_NEAR (f.axes[0].x, std::sqrt (0.5), 1e-9);
    EXPECT_NEAR (f.axes[0].y, std::sqrt (0.5), 1e-9);
    d.S ().localAxes = false;
    EXPECT_NEAR (eh::GizmoFrame (d.S ()).axes[0].x, 1.0, 1e-12);

    // Dragging the centre moves the pivot, not the shape, and the pivot stays.
    d.S ().localAxes = true;
    f = eh::GizmoFrame (d.S ());
    const float x = 600 + 20 * float (f.origin.x), y = 500 - 20 * float (f.origin.y);
    d.At (x, y);
    ASSERT_EQ (d.S ().hoverHandle, 0); // the centre
    d.Press (x, y);
    d.At (x + 40, y);
    d.Release (x + 40, y);
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 6.0, 1e-9);
    const ed::Frame moved = eh::GizmoFrame (d.S ());
    EXPECT_NEAR (moved.origin.x, f.origin.x + 2.0, 1e-6);
    EXPECT_NEAR (moved.origin.y, f.origin.y, 1e-6);
    d.S ().selection.Clear (); // another selection has its own centre
    EXPECT_NEAR (eh::GizmoFrame (d.S ()).origin.x, 0.0, 1e-9);
}
