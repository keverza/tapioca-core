#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditShape.hpp"
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

TEST (HudVirtualEditor, ClickingAGumballHandleAsksForItsNumber)
{
    Drive d (Corner ());
    d.Press (800, 300); // the vertex at (10, 10)
    d.Release (800, 300);
    d.At (840, 300); // on the X arrow
    ASSERT_GE (d.S ().hoverHandle, 0);
    d.Press (840, 300);
    d.Release (840, 300); // a click, not a drag
    ASSERT_GE (d.S ().typedHandle, 0);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u); // nothing moved yet
    ASSERT_FALSE (d.S ().tracker.Items ().empty ());
    EXPECT_EQ (d.S ().tracker.Items ()[0].quantity, ed::Quantity::Distance);
    d.input.characters = u"2.5";
    d.Frame ();
    d.Key (0x0D);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 12.5, 1e-9);
    EXPECT_EQ (d.S ().typedHandle, -1);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);

    // Rotate: a click on the ring asks for degrees.
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    d.S ().gizmo = ed::GizmoMode::Rotate;
    const ed::Frame f = eh::GizmoFrame (d.S ());
    const double r = 0.8 * 70.0 / 20.0; // the ring: 0.8 of the gizmo's 70 px, at 20 px a metre
    const float x = 600 + 20 * float (f.origin.x + r), y = 500 - 20 * float (f.origin.y);
    d.At (x, y);
    ASSERT_GE (d.S ().hoverHandle, 0);
    d.Press (x, y);
    d.Release (x, y);
    ASSERT_GE (d.S ().typedHandle, 0);
    EXPECT_EQ (d.S ().tracker.Items ()[0].quantity, ed::Quantity::Angle);
    d.Key (0x1B); // Esc gives the number up
    EXPECT_EQ (d.S ().typedHandle, -1);
}

TEST (HudVirtualEditor, DragsMoveFromABasePointAndSnapFlush)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 2, 0 }, { 0, 2, 0 } }, true); // a 4 x 2 box
    ed::AddPolyline (doc, { { -5, 6, 0 }, { 15, 6, 0 } }, false);                        // a line at y = 6
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    // Grab the selected box by its corner (4, 2) and drop that corner on the line's end (15, 6).
    d.Press (600 + 80, 500 - 40);
    d.At (600 + 300 - 3, 500 - 120 + 2); // near (15, 6)
    d.Release (600 + 300 - 3, 500 - 120 + 2);
    const auto& v = d.D ().entities[0].vertices;
    EXPECT_NEAR (v[2].p.x, 15.0, 1e-6);
    EXPECT_NEAR (v[2].p.y, 6.0, 1e-6);

    // Dragged near the line with no point to snap to (guides off, so no axis through the
    // grab), the box's top edge lands flush on it.
    d.S ().snapModes &= ~eh::kSnapGuides;
    d.Press (600 + 20 * 13, 500 - 20 * 5); // inside the moved box, away from its corners
    d.At (600 + 20 * 3, 500 - 20 * 4.8f);  // its top would sit 0.2 m below the line
    d.Release (600 + 20 * 3, 500 - 20 * 4.8f);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.y, 6.0, 1e-6);
    // A click without a drag on the selected box's corner picks that corner.
    d.Press (600 + 20 * 15, 500 - 20 * 6);
    d.Release (600 + 20 * 15, 500 - 20 * 6);
    EXPECT_EQ (d.S ().selection.Kind (), ed::SelectionKind::Vertex);
}

TEST (HudVirtualEditor, SnapGroupsSwitchFromTheMenu)
{
    Drive d (Corner ());
    d.RightClick (300, 200);
    EXPECT_TRUE (d.Offers ("snap.guides"));
    eh::RunMenuItem (d.S (), "snap.guides");
    EXPECT_EQ (d.S ().snapModes & eh::kSnapGuides, 0u);
    eh::RunMenuItem (d.S (), "snap.guides");
    EXPECT_EQ (d.S ().snapModes & eh::kSnapGuides, eh::kSnapGuides);
    eh::RunMenuItem (d.S (), "snap.flush");
    EXPECT_FALSE (d.S ().edgeSnap);
}

TEST (HudVirtualEditor, CornerGripsRoundEveryCornerOfASelectedObject)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 6, 0 }, { 0, 6, 0 } }, true);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    // The grip of the corner at (10, 0) sits 16 px (0.8 m) up its bisector, at (9.43, 0.57).
    const float gx = 600 + 20 * (10 - 0.5657f), gy = 500 - 20 * 0.5657f;
    d.At (gx, gy);
    ASSERT_GE (d.S ().hoverCorner, 0);
    // Dragged far up the bisector: every corner takes the radius, clamped to the 3 m the
    // 6 m sides allow two corners each.
    d.Press (gx, gy);
    d.At (gx - 34, gy - 34);
    d.Release (gx - 34, gy - 34);
    for (const auto& v : d.D ().entities[0].vertices)
        EXPECT_NEAR (v.radius, 3.0, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);

    // A double click on a grip: its first click picks that corner, the second makes it sharp.
    const ed::Vec3 grip = ed::CornerGripAt (d.D ().entities[0], 1, 0.8);
    const float hx = 600 + 20 * float (grip.x), hy = 500 - 20 * float (grip.y);
    d.At (hx, hy);
    ASSERT_GE (d.S ().hoverCorner, 0);
    d.input.buttons = { { 0, true }, { 0, false }, { 0, true }, { 0, false } };
    d.Frame ();
    EXPECT_EQ (d.S ().selection.Kind (), ed::SelectionKind::Vertex);
    EXPECT_DOUBLE_EQ (d.D ().entities[0].vertices[1].radius, 0.0);
    EXPECT_NEAR (d.D ().entities[0].vertices[0].radius, 3.0, 1e-9);
}

TEST (HudVirtualEditor, ThePenPlacesCornersAndPullsOutCurves)
{
    Drive d (ed::Document {});
    d.S ().tool = eh::Tool::Curve;
    d.S ().snapModes = 0; // the gesture alone
    d.Press (600, 500);   // (0, 0): a click, a corner
    d.Release (600, 500);
    d.Press (800, 500); // (10, 0): dragged up 3 m, a smooth anchor
    d.At (800, 440);
    d.Release (800, 440);
    d.Press (1000, 500); // (20, 0)
    d.Release (1000, 500);
    d.Key (0x0D); // Enter finishes
    ASSERT_EQ (d.D ().entities.size (), 1u);
    const auto& v = d.D ().entities[0].vertices;
    ASSERT_EQ (v.size (), 3u);
    EXPECT_FALSE (v[0].smooth);
    EXPECT_TRUE (v[1].smooth);
    EXPECT_NEAR (v[1].out.y, 3.0, 1e-9);
    EXPECT_NEAR (v[1].in.y, -3.0, 1e-9);
    EXPECT_TRUE (ed::SegmentCurved (d.D ().entities[0], 0));
}

TEST (HudVirtualEditor, AnchorHandlesDragAndCurvedSegmentsBend)
{
    ed::Document doc;
    const ed::Id e = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 20, 0, 0 } }, false);
    std::string error;
    EXPECT_TRUE (ed::SetSmooth (doc, e, ed::Find (doc, e)->vertices[1].id, true, error)) << error;
    Drive d (std::move (doc));
    // The middle anchor selected: its leaving handle ends at (13.33, 0).
    d.S ().selection.Set ({ { e, ed::Part::Vertex, d.D ().entities[0].vertices[1].id } });
    const float hx = 600 + 20 * (10 + 10.0f / 3.0f), hy = 500;
    d.At (hx, hy);
    ASSERT_GE (d.S ().hoverBezier, 0);
    d.Press (hx, hy);
    d.At (hx, hy - 60); // up 3 m
    d.Release (hx, hy - 60);
    const auto& v = d.D ().entities[0].vertices[1];
    EXPECT_NEAR (v.out.y, 3.0, 1e-9);
    // Smooth: the other handle turns into line with it, opposite, at its own length.
    const double outLength = std::hypot (v.out.x, v.out.y), inLength = std::hypot (v.in.x, v.in.y);
    EXPECT_NEAR (inLength, 10.0 / 3.0, 1e-9);
    EXPECT_NEAR (v.in.x / inLength, -v.out.x / outLength, 1e-9);
    EXPECT_NEAR (v.in.y / inLength, -v.out.y / outLength, 1e-9);
    // A press on the curved first segment bends it.
    const ed::Vec3 mid = ed::SegmentPoint (d.D ().entities[0], 0, 0.5);
    const float mx = 600 + 20 * float (mid.x), my = 500 - 20 * float (mid.y);
    d.S ().selection.Clear ();
    d.Press (mx, my);
    EXPECT_EQ (d.S ().drag, eh::Drag::Bend);
    d.At (mx, my - 40); // 2 m further
    d.Release (mx, my - 40);
    const ed::Vec3 bent = ed::SegmentPoint (d.D ().entities[0], 0, d.S ().bendT);
    EXPECT_GT (bent.y, mid.y + 1.5);
}

TEST (HudVirtualEditor, AClickOnAnEdgeNeverMovesIt)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, false);
    ed::AddPolyline (doc, { { 4.2, 0.5, 0 }, { 4.2, 5, 0 } }, false); // an end in snap reach, not pick reach
    Drive d (std::move (doc));
    const auto before = d.D ().entities[0].vertices;
    d.Press (600 + 84, 500 - 1); // on the edge, 0.45 m (9 px) below the other line's end
    d.At (600 + 85, 500 - 1);    // a pixel of jitter
    d.Release (600 + 85, 500 - 1);
    EXPECT_EQ (d.D ().entities[0].vertices, before);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    EXPECT_EQ (d.S ().selection.Kind (), ed::SelectionKind::Segment);
}

TEST (HudVirtualEditor, AClosedPenShapeJoinsSmoothly)
{
    Drive d (ed::Document {});
    d.S ().tool = eh::Tool::Curve;
    d.S ().snapModes = ed::kSnapEndpoint; // the first anchor closes
    for (auto [x, y] : { std::pair { 600.f, 500.f }, std::pair { 800.f, 500.f }, std::pair { 700.f, 380.f } }) {
        d.Press (x, y);
        d.Release (x, y);
    }
    d.Press (600, 500); // back on the first anchor
    d.Release (600, 500);
    ASSERT_EQ (d.D ().entities.size (), 1u);
    const auto& e = d.D ().entities[0];
    EXPECT_TRUE (e.closed);
    EXPECT_TRUE (e.vertices[0].smooth); // the curve runs on through it
    EXPECT_GT (std::hypot (e.vertices[0].out.x, e.vertices[0].out.y), 0.0);
}

TEST (HudVirtualEditor, APathsBendHasACornerGrip)
{
    ed::Document doc;
    const ed::Id p = ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 } }, 1.5);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { p, ed::Part::Entity, 0 } });
    // The sharp bend's grip sits 16 px (0.8 m) up its bisector from (10, 0).
    const float gx = 600 + 20 * (10 - 0.5657f), gy = 500 - 20 * 0.5657f;
    d.At (gx, gy);
    ASSERT_GE (d.S ().hoverCorner, 0);
    // Dragged 24 px each way from the corner, 1.70 m in: the middle of a 4.10 m arc, as
    // r (1 / sin 45 - 1) puts it (to the centimetre).
    const float in = 24.0f;
    d.Press (gx, gy);
    d.At (600 + 200 - in, 500 - in);
    d.Release (600 + 200 - in, 500 - in);
    EXPECT_NEAR (ed::FindPath (d.D (), p)->points[1].radius, 4.10, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, AnOpenPolylineOffersToBecomeAPath)
{
    ed::Document doc;
    const ed::Id e = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 } }, false);
    ed::AddPolyline (doc, { { 20, 0, 0 }, { 25, 0, 0 }, { 25, 5, 0 } }, true);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { e, ed::Part::Entity, 0 } });
    d.S ().menu.target = eh::MenuTarget::Object;
    d.S ().menu.ref = { e, ed::Part::Entity, 0 };
    ASSERT_TRUE (d.Offers ("action.make-path"));
    eh::RunMenuItem (d.S (), "action.make-path");
    ASSERT_EQ (d.D ().paths.size (), 1u);
    // What it made is selected, ready for its grips.
    ASSERT_EQ (d.S ().selection.Items ().size (), 1u);
    EXPECT_EQ (d.S ().selection.Items ()[0].entity, d.D ().paths[0].id);
    // A closed outline has no ends to be a path's: the menu does not offer it.
    const ed::Id closed = d.D ().entities[0].id;
    d.S ().selection.Set ({ { closed, ed::Part::Entity, 0 } });
    d.S ().menu.ref = { closed, ed::Part::Entity, 0 };
    EXPECT_FALSE (d.Offers ("action.make-path"));
}

TEST (HudVirtualEditor, KerbGripsRoundAPathsCornersAndItsJunctions)
{
    // A tee: B (2 m wide) runs up from A's centreline. A is selected whole.
    ed::Document doc;
    const ed::Id a = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 0 } }, 2.0);
    ed::AddPath (doc, { { 10, 0, 0 }, { 10, 10, 0 } }, 2.0);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { a, ed::Part::Entity, 0 } });
    const auto screen = [] (ed::Vec3 p) { return ImVec2 { 600 + 20 * float (p.x), 500 - 20 * float (p.y) }; };
    const auto corner = [&] (bool junction) {
        for (const auto& c : ed::Pave (d.D ().paths, 0.5, false).corners)
            if ((c.key.other != ed::kNoId) == junction && c.Involves (a))
                return c;
        return ed::PavedCorner {};
    };
    // A junction's grip, 16 px (0.8 m) out along its bisector, dragged 2 m further out: that
    // corner alone takes the radius whose arc's middle is there.
    const ed::PavedCorner j = corner (true);
    ASSERT_EQ (j.path, a);
    const ImVec2 g = screen (ed::KerbGripAt (j, 0.8)), to = screen (ed::KerbGripAt (j, 0.8) + j.bisector * 2.0);
    d.At (g.x, g.y);
    ASSERT_GE (d.S ().hoverKerb, 0);
    d.Press (g.x, g.y);
    d.At (to.x, to.y);
    d.Release (to.x, to.y);
    const ed::Path& pa = *ed::FindPath (d.D (), a);
    ASSERT_EQ (pa.kerbs.size (), 1u);
    EXPECT_TRUE (pa.kerbs[0].Same (j.key));
    EXPECT_NEAR (pa.kerbs[0].radius, 2.8 / (1.0 / std::sin (j.half) - 1.0), 0.02);
    EXPECT_DOUBLE_EQ (pa.edgeRadius, 0.0);
    // An own corner's grip sets the path's kerb radius, clamped to most of its half width.
    const ed::PavedCorner own = corner (false);
    const ImVec2 h = screen (ed::KerbGripAt (own, 0.8)), out = screen (ed::KerbGripAt (own, 0.8) + own.bisector * 3.0);
    d.At (h.x, h.y);
    ASSERT_GE (d.S ().hoverKerb, 0);
    d.Press (h.x, h.y);
    d.At (out.x, out.y);
    d.Release (out.x, out.y);
    EXPECT_NEAR (ed::FindPath (d.D (), a)->edgeRadius, 0.98, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 2u);
}

TEST (HudVirtualEditor, KerbGripsShowAtACrossingFromEitherPath)
{
    // A + crossing; only a control point of the newer path B is selected. The four corners
    // where their sides cross still show grips, and one dragged is rounded alone.
    ed::Document doc;
    const ed::Id a = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 0 } }, 2.0);
    const ed::Id b = ed::AddPath (doc, { { 10, -10, 0 }, { 10, 10, 0 } }, 2.0);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { b, ed::Part::PathPoint, ed::FindPath (d.D (), b)->points[0].id } });
    const auto screen = [] (ed::Vec3 p) { return ImVec2 { 600 + 20 * float (p.x), 500 - 20 * float (p.y) }; };
    std::vector<ed::PavedCorner> junctions;
    for (const auto& c : ed::Pave (d.D ().paths, 0.5, false).corners)
        if (c.key.other != ed::kNoId)
            junctions.push_back (c);
    ASSERT_EQ (junctions.size (), 4u);
    for (const auto& c : junctions) {
        const ImVec2 g = screen (ed::KerbGripAt (c, 0.8));
        d.At (g.x, g.y);
        EXPECT_GE (d.S ().hoverKerb, 0) << c.at.x << ", " << c.at.y;
    }
    const ed::PavedCorner& j = junctions[0];
    const ImVec2 g = screen (ed::KerbGripAt (j, 0.8)), to = screen (ed::KerbGripAt (j, 0.8) + j.bisector * 2.0);
    d.At (g.x, g.y);
    d.Press (g.x, g.y);
    d.At (to.x, to.y);
    d.Release (to.x, to.y);
    const ed::Path& pa = *ed::FindPath (d.D (), a);
    ASSERT_EQ (pa.kerbs.size (), 1u);
    EXPECT_TRUE (pa.kerbs[0].Same (j.key));
    EXPECT_GT (pa.kerbs[0].radius, 1.0);
    EXPECT_DOUBLE_EQ (pa.edgeRadius, 0.0);
    // Nothing of a path selected: no grips.
    d.S ().selection.Set ({});
    const ImVec2 h = screen (ed::KerbGripAt (junctions[1], 0.8));
    d.At (h.x, h.y);
    EXPECT_LT (d.S ().hoverKerb, 0);
}

TEST (HudVirtualEditor, KerbGripsShowOnEveryCornerOfThePavedArea)
{
    // An L, and a straight path across its upright leg. With only the straight path selected,
    // the L's bend corners -- the outside and the inside one of its edges -- show grips too,
    // and one dragged is filleted alone.
    ed::Document doc;
    const ed::Id l = ed::AddPath (doc, { { 0, 0, 0 }, { 12, 0, 0 }, { 12, 18, 0 } }, 2.0);
    const ed::Id across = ed::AddPath (doc, { { 6, 10, 0 }, { 20, 10, 0 } }, 2.0);
    const ed::Id apart = ed::AddPath (doc, { { 2, -6, 0 }, { 10, -6, 0 } }, 2.0);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { across, ed::Part::Entity, 0 } });
    const auto screen = [] (ed::Vec3 p) { return ImVec2 { 600 + 20 * float (p.x), 500 - 20 * float (p.y) }; };
    const ed::Id bend = ed::FindPath (d.D (), l)->points[1].id;
    const ed::PavedCorner* inside = nullptr;
    const auto paved = ed::Pave (d.D ().paths, 0.5, false);
    for (const auto& c : paved.corners) {
        const ImVec2 g = screen (ed::KerbGripAt (c, 0.8));
        d.At (g.x, g.y);
        EXPECT_EQ (d.S ().hoverKerb >= 0, !c.Involves (apart)) << c.at.x << ", " << c.at.y;
        if (c.key.point == bend && !c.convex)
            inside = &c;
    }
    ASSERT_NE (inside, nullptr);
    const ImVec2 g = screen (ed::KerbGripAt (*inside, 0.8)),
                 to = screen (ed::KerbGripAt (*inside, 0.8) + inside->bisector * 1.5);
    d.At (g.x, g.y);
    d.Press (g.x, g.y);
    d.At (to.x, to.y);
    d.Release (to.x, to.y);
    const ed::Path& pl = *ed::FindPath (d.D (), l);
    ASSERT_EQ (pl.kerbs.size (), 1u);
    EXPECT_TRUE (pl.kerbs[0].Same (inside->key));
    EXPECT_GT (pl.kerbs[0].radius, 1.0);
    EXPECT_DOUBLE_EQ (pl.edgeRadius, 0.0);
}
