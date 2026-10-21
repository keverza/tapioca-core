#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/OverlayHitMap.hpp" // overlayinput::Cursor: the cursor claim these tests read
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/OverlayScene.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditShape.hpp"
#include "hud_fixture.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>

namespace ed = geomsrv::edit;
namespace eh = geomsrv::archviz::edithud;
namespace el = geomsrv::archviz::editlayers;
namespace hud = geomsrv::archviz::overlayhud;
namespace layers = geomsrv::archviz::overlaylayers;
namespace oi = geomsrv::archviz::overlayinput;

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
        // The single Move gizmo, whose layout most of these tests press on; the gumball's own
        // are below (GumballHoldsEveryTransform).
        S ().gizmo = ed::GizmoMode::Move;
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

TEST (HudVirtualEditor, FarZoomHidesPathDetailButKeepsContoursWithoutRederivingTheSurface)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 2 } }, 2);
    Drive d (doc);
    d.At (0, 0);
    EXPECT_TRUE (d.S ().quietPaths.empty ());
    const auto revision = d.D ().revision, pavedRevision = d.S ().pavedRevision;
    const auto vertices = d.S ().paved.vertices;
    const auto contours = d.S ().contourLines.size ();
    ASSERT_GT (contours, 0u);
    d.input.plan[0] = 5;
    d.input.plan[4] = -5;
    d.Frame ();
    EXPECT_EQ (d.S ().quietPaths, std::vector<ed::Id> { id });
    EXPECT_TRUE (d.S ().placed.empty ());
    EXPECT_EQ (d.S ().pavedRevision, pavedRevision);
    EXPECT_EQ (d.S ().paved.vertices, vertices);
    EXPECT_EQ (d.S ().contourLines.size (), contours);
    size_t strokes = 0;
    for (const auto& p : d.layer.polylines) {
        EXPECT_EQ (p.endArrow, layers::Terminator::None);
        EXPECT_NE (p.rgba, el::Style {}.geometry);
        strokes += p.rgba == el::Style {}.contour;
    }
    EXPECT_GT (strokes, 0u);
    const auto generation = layers::Generation ();
    d.input.plan[0] = 14;
    d.input.plan[4] = -14; // 28px wide: hysteresis keeps the overview
    d.Frame ();
    EXPECT_EQ (layers::Generation (), generation);
    d.input.plan[0] = 20;
    d.input.plan[4] = -20;
    d.Frame ();
    EXPECT_TRUE (d.S ().quietPaths.empty ());
    EXPECT_GT (layers::Generation (), generation);
    EXPECT_EQ (d.D ().revision, revision);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, ThePathPointMenuSplitsAtItsBoundPointWithOneUndo)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 1 }, { 10, 10, 2 } }, 2);
    const auto point = doc.paths[0].points[1].id;
    Drive d (doc);
    d.S ().menu.target = eh::MenuTarget::PathPoint;
    d.S ().menu.ref = { id, ed::Part::PathPoint, point };
    d.S ().menu.at = { 8, 0, 0 }; // the part's identity, not the offset press, owns this action
    EXPECT_TRUE (d.Offers ("path.split"));
    eh::RunMenuItem (d.S (), "path.split");
    ASSERT_EQ (d.D ().paths.size (), 2u);
    EXPECT_NEAR (d.D ().paths[0].profile.back ().station, 10, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    ASSERT_TRUE (d.S ().edits.Undo ());
    EXPECT_EQ (d.D ().paths, doc.paths);
}

TEST (HudVirtualEditor, AnOffsetEndpointPressSnapsThePointAndJoinsWithAGradeControl)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, 2);
    ed::AddPath (doc, { { 12, 0, 0 }, { 12, 10, 1 } }, 2);
    Drive d (doc);
    d.Press (797, 498); // 3px off the first endpoint
    ASSERT_EQ (d.S ().target.part, ed::Part::PathPoint);
    d.At (840, 500);
    d.Release (840, 500);
    ASSERT_EQ (d.D ().paths.size (), 1u) << d.S ().message;
    const auto* path = ed::FindPath (d.D (), id);
    ASSERT_NE (path, nullptr);
    ASSERT_EQ (path->points.size (), 3u);
    EXPECT_NEAR (path->points[1].p.x, 12, 1e-9);
    EXPECT_NEAR (path->points[1].p.y, 0, 1e-9);
    ASSERT_EQ (path->profile.size (), 3u);
    EXPECT_EQ (path->profile[1].anchor, path->points[1].id);
    EXPECT_NEAR (path->profile[1].station, 12, 1e-9);
    ed::PathOutline outline;
    std::string error;
    ASSERT_TRUE (ed::Outline (*path, .5, outline, error));
    bool outsideMiter = false;
    for (auto p : outline.right)
        outsideMiter |= std::abs (p.x - 13) < 1e-9 && std::abs (p.y + 1) < 1e-9;
    EXPECT_TRUE (outsideMiter);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, SurfaceQualityChangesOnlyForChangedPreviewsAndRefinesOnceAtEnd)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 1 } }, 2);
    Drive d (doc);
    const auto generation = layers::Generation ();
    ASSERT_TRUE (d.S ().edits.Begin ());
    d.Publish ();
    EXPECT_EQ (layers::Generation (), generation) << "a no-op press must not republish a coarse model";
    const auto grade = doc.paths[0].profile.back ().id;
    ASSERT_TRUE (d.S ().edits.Move (
        [=] (ed::Document& c, std::string& e) { return ed::SetPathElevation (c, id, grade, 2, e); }));
    d.Publish ();
    EXPECT_DOUBLE_EQ (d.S ().pavedStep, 2);
    const auto coarse = d.S ().paved.triangles.size ();
    const auto revision = d.D ().revision;
    ASSERT_TRUE (d.S ().edits.End ());
    d.Publish ();
    EXPECT_EQ (d.D ().revision, revision);
    EXPECT_DOUBLE_EQ (d.S ().pavedStep, 0.5);
    EXPECT_EQ (d.S ().paved.triangles.size (), coarse) << "a planar run needs no extra resting subdivisions";
    const auto finished = layers::Generation ();
    d.Publish ();
    EXPECT_EQ (layers::Generation (), finished);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, FinishingADrawnEndpointExtensionSelectsTheJoinedPath)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, 2, "footpath");
    Drive d (doc);
    d.S ().tool = eh::Tool::Path;
    d.Press (800, 500);
    d.Release (800, 500);
    d.Press (1000, 500);
    d.Release (1000, 500);
    d.Key (0x0D);
    ASSERT_EQ (d.D ().paths.size (), 1u);
    EXPECT_EQ (d.D ().paths[0].id, id);
    EXPECT_EQ (d.D ().paths[0].role, "footpath");
    ASSERT_EQ (d.S ().selection.Items ().size (), 1u);
    EXPECT_TRUE (ed::Exists (d.D (), d.S ().selection.Items ()[0]));
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    ASSERT_TRUE (d.S ().edits.Undo ());
    EXPECT_EQ (d.D ().paths, doc.paths);
}

TEST (HudVirtualEditor, SeededPlanHoverAndNavigationKeepModelPublicationStable)
{
    ed::Document doc;
    ed::AddPath (doc, { { 0, -10, 0 }, { 25, -10, 0.6 }, { 25, 5, 1 } }, 1.8);
    ed::AddExtrusion (doc, { { 5, 5, 0 }, { 11, 5, 0 }, { 11, 9, 0 }, { 5, 9, 0 } }, 3);
    Drive d (std::move (doc));
    const auto coldStarted = std::chrono::steady_clock::now ();
    d.Frame ();
    const double cold =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - coldStarted).count ();
    const auto revision = d.D ().revision;
    const auto generation = layers::Generation ();
    std::vector<double> hover, navigation;
    for (int i = 0; i < 120; ++i) {
        const auto started = std::chrono::steady_clock::now ();
        d.input.x = 700 + float (i % 20);
        d.input.y = 700;
        if (i >= 60)
            d.input.plan[2] = 600 + i;
        const auto plan =
            geomsrv::archviz::overlayscene::PreparePlanHud (layers::Layers (), &d.view.engine, 1, d.input);
        EXPECT_TRUE (plan.locked);
        d.Publish ();
        EXPECT_EQ (d.D ().revision, revision);
        EXPECT_EQ (layers::Generation (), generation);
        (i < 60 ? hover : navigation)
            .push_back (
                std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ());
    }
    const auto report = [] (const char* name, std::vector<double>& samples) {
        std::sort (samples.begin (), samples.end ());
        std::printf ("Seeded plan %s: %.3f ms p50, %.3f p95, %.3f max (60 warm CPU layouts)\n", name,
                     samples[samples.size () / 2], samples[(samples.size () * 95 + 99) / 100 - 1], samples.back ());
    };
    std::printf ("Seeded plan first HUD build: %.3f ms (CPU only, excludes initial model publication)\n", cold);
    report ("hover", hover);
    report ("navigation", navigation);
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

TEST (HudVirtualEditor, ADistanceTypedThroughAPartDragMovesItThatFar)
{
    Drive d (Corner ());
    d.Press (800, 500); // the vertex at (10, 0), dragged 2 m along X
    d.At (840, 500);
    ASSERT_EQ (d.S ().drag, eh::Drag::Part);
    ASSERT_EQ (d.S ().tracker.Items ().size (), 1u);
    EXPECT_EQ (d.S ().tracker.Items ()[0].quantity, ed::Quantity::Distance);
    EXPECT_NEAR (d.S ().tracker.Items ()[0].value, 2.0, 1e-6);
    d.input.characters = u"5";
    d.Frame ();
    d.At (860, 520); // the number has the drag: the pointer moves nothing
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 12.0, 1e-6);
    d.Key (0x0D);
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 15.0, 1e-9); // 5 m the way it was going
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.y, 0.0, 1e-9);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    d.Release (860, 520);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);

    // Released while typed, then given up: the preview stays uncommitted until explicitly ended.
    d.S ().selection.Clear (); // no gumball over it: a press on it is the part's
    d.Press (900, 500);        // the vertex, now at (15, 0)
    d.At (920, 500);
    d.input.characters = u"9";
    d.Frame ();
    d.Release (920, 500);
    EXPECT_EQ (d.S ().drag, eh::Drag::Part);
    d.Key (0x1B);
    EXPECT_EQ (d.S ().drag, eh::Drag::Part);
    EXPECT_TRUE (d.S ().edits.InGesture ());
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 16.0, 1e-6);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.Key (0x1B);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 15.0, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
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

TEST (HudVirtualEditor, ARefusedTrackerValueStaysAvailableForCorrection)
{
    Drive d (Corner ());
    d.Press (700, 500);
    d.Release (700, 500);
    const auto original = d.D ().entities;
    d.input.characters = u"-1";
    d.Frame ();
    d.Key (0x0D);
    EXPECT_EQ (d.S ().tracker.State (), ed::TrackerState::Editing);
    EXPECT_EQ (d.S ().tracker.Text (), "-1");
    EXPECT_FALSE (d.S ().message.empty ());
    EXPECT_EQ (d.D ().entities, original);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    d.Key (0x08);
    d.Key (0x08);
    d.input.characters = u"6";
    d.Frame ();
    d.Key (0x0D);
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 6.0, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, ARefusedClickedHandleRetainsItsBinding)
{
    auto doc = Corner ();
    doc.entities[0].vertices[2].lockXY = true;
    Drive d (std::move (doc));
    d.S ().localAxes = false;
    d.Press (800, 300);
    d.Release (800, 300);
    d.Press (840, 300);
    d.Release (840, 300);
    ASSERT_GE (d.S ().typedHandle, 0);
    const int handle = d.S ().typedHandle;
    d.input.characters = u"2";
    d.Frame ();
    d.Key (0x0D);
    EXPECT_EQ (d.S ().typedHandle, handle);
    EXPECT_EQ (d.S ().tracker.State (), ed::TrackerState::Editing);
    EXPECT_EQ (d.S ().tracker.Text (), "2");
    EXPECT_NE (d.S ().message.find ("locked"), std::string::npos);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 10.0, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    d.Key (0x08);
    d.input.characters = u"0"; // an unchanged valid value closes without an undo
    d.Frame ();
    d.Key (0x0D);
    EXPECT_EQ (d.S ().typedHandle, -1);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    EXPECT_TRUE (d.S ().message.empty ());
}

TEST (HudVirtualEditor, ARefusedValueCannotCommitTheLastValidDrag)
{
    Drive d (Corner ());
    d.Press (800, 500);
    d.At (840, 500);
    d.input.characters = u"-10"; // collapses the moved vertex onto its neighbour
    d.Frame ();
    d.Release (840, 500);
    d.Key (0x0D);
    EXPECT_EQ (d.S ().drag, eh::Drag::Part);
    EXPECT_TRUE (d.S ().edits.InGesture ());
    EXPECT_EQ (d.S ().tracker.State (), ed::TrackerState::Editing);
    EXPECT_EQ (d.S ().tracker.Text (), "-10");
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 12.0, 1e-6);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    d.Key (0x1B);
    d.Key (0x1B);
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 10.0, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, UndoWaitsForTheNumberBeingTyped)
{
    Drive d (Corner ());
    d.Press (800, 500); // the vertex at (10, 0), dragged to (12, 0)
    d.At (840, 500);
    d.Release (840, 500);
    ASSERT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.Press (720, 500); // the first segment's middle, now at (6, 0)
    d.Release (720, 500);
    d.input.characters = u"5";
    d.Frame ();
    ASSERT_EQ (d.S ().tracker.State (), ed::TrackerState::Editing);
    // Ctrl+Z, and Archicad's own Undo routed to the editor: the typed number has the keyboard.
    d.input.keys = { { 'Z', true, false, true } };
    d.Frame ();
    d.input.editActions = { hud::Input::EditAction::Undo };
    d.Frame ();
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 12.0, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    EXPECT_EQ (d.S ().tracker.State (), ed::TrackerState::Editing);
    EXPECT_EQ (d.S ().tracker.Text (), "5");
    // Given up with Esc, the same chord undoes the drag.
    d.Key (0x1B);
    ASSERT_NE (d.S ().tracker.State (), ed::TrackerState::Editing);
    d.input.keys = { { 'Z', true, false, true } };
    d.Frame ();
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 10.0, 1e-9);
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

namespace {
// The vertex at (x, 10) selected and its gumball's X arrow pressed and dragged 2 m.
void DragTheXArrow (Drive& d, float x = 10)
{
    d.S ().localAxes = false; // the arrow along world X, whatever the segments do
    const float sx = 600 + 20 * x;
    d.Press (sx, 300);
    d.Release (sx, 300);
    d.At (sx + 40, 300);
    ASSERT_GE (d.S ().hoverHandle, 0);
    d.Press (sx + 40, 300);
    d.At (sx + 80, 300);
    ASSERT_EQ (d.S ().drag, eh::Drag::Handle);
    ASSERT_NEAR (d.D ().entities[0].vertices[2].p.x, x + 2.0, 1e-6);
}
} // namespace

TEST (HudVirtualEditor, ANumberTypedMidDragEndsItOnce)
{
    Drive d (Corner ());
    DragTheXArrow (d);
    // The tracker offers the arrow's own quantity, not the vertex's dimensions.
    ASSERT_EQ (d.S ().tracker.Items ().size (), 1u);
    EXPECT_EQ (d.S ().tracker.Items ()[0].quantity, ed::Quantity::Distance);
    EXPECT_NEAR (d.S ().tracker.Items ()[0].value, 2.0, 1e-6);
    d.input.characters = u"5";
    d.Frame ();
    d.At (900, 300); // the number has the drag now: the pointer moves nothing
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 12.0, 1e-6);
    d.Key (0x0D);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 15.0, 1e-9); // 5 m from where the drag began
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.Release (900, 300); // the release ends nothing more
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 15.0, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.input.keys = { { 'Z', true, false, true } };
    d.Frame ();
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 10.0, 1e-9);
}

TEST (HudVirtualEditor, ModeKeysWaitForTheDrag)
{
    Drive d (Corner ());
    DragTheXArrow (d);
    d.Key ('R'); // Rotate's handle at this index is a ring, not the arrow held
    d.Key ('E'); // and Extrude has one handle
    d.Key ('L');
    EXPECT_EQ (d.S ().gizmo, ed::GizmoMode::Move);
    EXPECT_EQ (d.S ().tool, eh::Tool::Select);
    EXPECT_FALSE (d.S ().message.empty ());
    d.At (900, 300);
    d.Release (900, 300);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 13.0, 1e-6);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.y, 10.0, 1e-6);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    // Rotation applies to a shape, not the lone point just moved.
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    d.Key ('R'); // after the gesture, an applicable mode can be chosen
    EXPECT_EQ (d.S ().gizmo, ed::GizmoMode::Rotate);
}

TEST (HudVirtualEditor, EscapeMidDragGivesUpTheNumberThenTheDrag)
{
    Drive d (Corner ());
    DragTheXArrow (d);
    d.input.characters = u"5";
    d.Frame ();
    d.Key (0x1B); // the number goes: the drag is as it was when typing began, and runs on
    EXPECT_NE (d.S ().tracker.State (), ed::TrackerState::Editing);
    EXPECT_EQ (d.S ().drag, eh::Drag::Handle);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 12.0, 1e-6);
    d.At (900, 300);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 13.0, 1e-6);
    d.Key (0x1B); // the drag goes: back to before it
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 10.0, 1e-12);
    d.Release (900, 300);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, AReleaseWhileANumberIsTypedWaitsForIt)
{
    Drive d (Corner ());
    DragTheXArrow (d);
    d.input.characters = u"5";
    d.Frame ();
    d.Release (880, 300);
    EXPECT_EQ (d.S ().drag, eh::Drag::Handle); // still the number's
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    d.Key (0x0D);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 15.0, 1e-9);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);

    // Giving up the number is not permission to commit the released drag.
    DragTheXArrow (d, 15); // the vertex is at (17, 10)
    d.input.characters = u"1";
    d.Frame ();
    d.Release (980, 300);
    d.Key (0x1B);
    EXPECT_EQ (d.S ().drag, eh::Drag::Handle);
    EXPECT_TRUE (d.S ().edits.InGesture ());
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 17.0, 1e-6);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.Key (0x1B);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 15.0, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, AReleasedNumericDragCanExplicitlyFinishItsCheckpoint)
{
    Drive d (Corner ());
    DragTheXArrow (d);
    d.input.characters = u"5";
    d.Frame ();
    d.Release (940, 340); // moving the pointer while typing must not change the checkpoint
    d.Key (0x1B);
    d.At (950, 350);
    EXPECT_EQ (d.S ().drag, eh::Drag::Handle);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 12.0, 1e-6);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    EXPECT_FALSE (d.S ().message.empty ()); // the waiting drag is explained, not hidden
    d.Key (0x0D);                           // Enter without a field explicitly finishes the preserved preview
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_FALSE (d.S ().edits.InGesture ());
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 12.0, 1e-6);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.Key (0x0D);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.input.keys = { { 'Z', true, false, true } };
    d.Frame ();
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 10.0, 1e-9);
    d.input.keys = { { 'Y', true, false, true } };
    d.Frame ();
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 12.0, 1e-6);
}

TEST (HudVirtualEditor, AReleasedPartDragCanRetypeBeforeFinishing)
{
    Drive d (Corner ());
    d.Press (800, 500);
    d.At (840, 500);
    d.input.characters = u"5";
    d.Frame ();
    d.Release (880, 500);
    d.Key (0x1B);
    EXPECT_EQ (d.S ().drag, eh::Drag::Part);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    d.input.characters = u"3";
    d.Frame ();
    d.Key (0x0D);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 13.0, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, EscapeRestoresTheNumericCheckpointEvenAfterPointerMovement)
{
    Drive d (Corner ());
    DragTheXArrow (d);
    d.input.characters = u"5";
    d.Frame ();
    d.At (940, 340);
    d.Key (0x1B);
    EXPECT_NE (d.S ().tracker.State (), ed::TrackerState::Editing);
    EXPECT_EQ (d.S ().drag, eh::Drag::Handle);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 12.0, 1e-6);
    d.Frame (); // repeated layout is not a new pointer move
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 12.0, 1e-6);
    d.Key (0x1B);
    d.Release (940, 340);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 10.0, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, NavigationPausesAndRebasesAPartDragWithoutHistory)
{
    Drive d (Corner ());
    d.S ().snapModes = 0;
    d.S ().edgeSnap = false;
    d.Press (800, 500);
    d.At (840, 500);
    const auto before = d.S ().edits.Before ().entities;
    const auto preview = d.D ().entities;
    d.input.navigating = true;
    ++d.input.navigationRevision;
    d.input.plan[2] += 100;
    d.At (900, 550);
    EXPECT_EQ (d.D ().entities, preview);
    EXPECT_EQ (d.S ().edits.Before ().entities, before);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    d.input.navigating = false;
    ++d.input.navigationRevision;
    d.Frame ();
    d.Frame ();
    EXPECT_EQ (d.D ().entities, preview);
    d.At (920, 550);                       // one metre after resume, still evaluated from the original before
    EXPECT_TRUE (d.S ().message.empty ()); // no stale "release navigation" instruction after resume
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 13, 1e-6);
    d.Release (920, 550);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.input.keys = { { 'Z', true, false, true } };
    d.Frame ();
    EXPECT_EQ (d.D ().entities, before);
}

// ⚠️ THE TOOL'S CURSOR IS OWED WHEREVER THE POINTER IS ON THE VIEW (2026-10-18, the user: the
// cursor kept changing from the standard one to the tool's through an interaction and for seconds
// after it). A native navigation suspends EDITING -- the hover, the picks, nothing is placed -- but
// it must not withdraw the published cursor claim, or the HUD's cursor can only come back a layout
// after the gesture ends, while the arrow is what is shown. The same claim is owed while anything
// else holds ImGui's hover away from the view (a popup, a foreign ActiveId), and it is NOT owed
// over the HUD itself: on a panel, a legend or an open menu the pointer is not the tool's.
TEST (HudVirtualEditor, TheToolCursorIsClaimedWhereverThePointerIsOnTheView)
{
    Drive d (Corner ());
    d.S ().tool = eh::Tool::Curve;
    d.At (600, 500);
    ASSERT_EQ (d.Frame ().cursor, uint8_t (oi::Cursor::Pen)) << "the pen on the view, before any navigation";
    // The native middle-button gesture: editing is suspended for it, the cursor is not.
    d.input.navigating = true;
    EXPECT_EQ (d.Frame ().cursor, uint8_t (oi::Cursor::Pen))
        << "navigation suspends editing, not the cursor the HUD has published";
    d.input.navigating = false;
    EXPECT_EQ (d.Frame ().cursor, uint8_t (oi::Cursor::Pen)) << "the pen is still the pen once it ends";
    // Over the HUD -- a panel, a legend, the open menu -- the pointer is not the tool's.
    d.input.overHud = true;
    EXPECT_NE (d.Frame ().cursor, uint8_t (oi::Cursor::Pen)) << "over the HUD the tool's cursor is not claimed";
    d.input.overHud = false;
    EXPECT_EQ (d.Frame ().cursor, uint8_t (oi::Cursor::Pen)) << "and it is claimed again back on the view";
}

TEST (HudVirtualEditor, NavigationRebasesAHandleAndKeepsItsTypedTotal)
{
    Drive d (Corner ());
    DragTheXArrow (d);
    d.input.navigating = true;
    ++d.input.navigationRevision;
    d.input.plan[0] = 40;
    d.input.plan[4] = -40;
    d.At (930, 320);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 12, 1e-6);
    d.input.navigating = false;
    ++d.input.navigationRevision;
    d.Frame ();
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 12, 1e-6);
    d.At (970, 320);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 13, 1e-6);
    d.input.characters = u"5";
    d.Frame ();
    d.Key (0x0D);
    d.Release (970, 320);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 15, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, ReleaseDuringNavigationWaitsForExplicitFinishOrCancel)
{
    for (const bool cancel : { true, false }) {
        Drive d (Corner ());
        DragTheXArrow (d);
        const auto preview = d.D ().entities;
        d.input.navigating = true;
        ++d.input.navigationRevision;
        d.Release (940, 340);
        EXPECT_TRUE (d.S ().dragReleased);
        EXPECT_TRUE (d.S ().edits.InGesture ());
        EXPECT_EQ (d.D ().entities, preview);
        d.input.navigating = false;
        ++d.input.navigationRevision;
        d.At (950, 350);
        EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
        d.Key (cancel ? 0x1B : 0x0D);
        EXPECT_EQ (d.S ().drag, eh::Drag::None);
        EXPECT_FALSE (d.S ().edits.InGesture ());
        EXPECT_EQ (d.S ().edits.UndoDepth (), cancel ? 0u : 1u);
        EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, cancel ? 10 : 12, 1e-6);
    }
}

TEST (HudVirtualEditor, WheelAnimationChangesOnlyTheViewUntilPointerMotionResumes)
{
    Drive d (Corner ());
    DragTheXArrow (d);
    const auto preview = d.D ().entities;
    ++d.input.navigationRevision; // wheel has no held-button state
    for (const double zoom : { 25.0, 30.0, 40.0 }) {
        d.input.plan[0] = zoom;
        d.input.plan[4] = -zoom;
        d.At (900, 320);
        EXPECT_EQ (d.D ().entities, preview);
    }
    d.Frame ();
    EXPECT_EQ (d.D ().entities, preview);
    d.At (940, 320);
    d.Release (940, 320);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 13, 1e-6);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, MarqueeStaysAtThePointerWhileNavigatingAndKeepsItsModifier)
{
    Drive d (Corner ());
    const auto& entity = d.D ().entities[0];
    const ed::Ref first { entity.id, ed::Part::Vertex, entity.vertices[0].id };
    const ed::Ref last { entity.id, ed::Part::Vertex, entity.vertices[2].id };
    d.S ().filter = ed::kPickVertex;
    d.S ().selection.Set ({ last });
    d.input.shift = true;
    d.Press (550, 550);
    ASSERT_EQ (d.S ().drag, eh::Drag::Marquee);
    d.input.navigating = true;
    ++d.input.navigationRevision;
    d.input.shift = false;
    d.input.plan[2] += 100;
    d.At (850, 250);
    EXPECT_EQ (d.S ().drag, eh::Drag::Marquee);
    d.Release (850, 250);
    EXPECT_FALSE (d.S ().selection.Contains (first));
    d.input.navigating = false;
    ++d.input.navigationRevision;
    d.Frame ();
    EXPECT_EQ (d.S ().drag, eh::Drag::Marquee);
    d.Key (0x0D);
    EXPECT_TRUE (d.S ().selection.Contains (first));
    EXPECT_TRUE (d.S ().selection.Contains (last));
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, WindowReplacementCancelsBeforeNumericEnterCanApply)
{
    Drive d (Corner ());
    d.input.viewIdentity = 1;
    DragTheXArrow (d);
    d.input.characters = u"5";
    d.Frame ();
    d.input.viewIdentity = 2;
    d.Key (0x0D);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 10, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, NavigationDoesNotTurnACapturedClickIntoADrag)
{
    Drive d (Corner ());
    d.Press (800, 500);
    ASSERT_EQ (d.S ().drag, eh::Drag::Part);
    const auto original = d.D ().entities;
    d.input.navigating = true;
    d.At (950, 350);
    d.input.navigating = false;
    d.input.plan[2] += 150;
    d.Frame ();
    d.Frame ();
    d.At (951, 350);
    EXPECT_FALSE (d.S ().dragStarted);
    d.Release (951, 350);
    EXPECT_EQ (d.D ().entities, original);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, RepeatedNavigationContinuesFromTheLatestValidHandlePreview)
{
    Drive d (Corner ());
    DragTheXArrow (d);
    for (int i = 0; i < 3; ++i) {
        const auto preview = d.D ().entities;
        const auto revision = d.D ().revision;
        d.input.navigating = true;
        ++d.input.navigationRevision;
        d.At (900, 350);
        d.input.navigating = false;
        ++d.input.navigationRevision;
        d.input.plan[2] += 25;
        d.Frame ();
        d.Frame ();
        d.Frame ();
        EXPECT_EQ (d.D ().entities, preview);
        EXPECT_EQ (d.D ().revision, revision);
        d.At (920, 350);
        EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 13 + i, 1e-6);
    }
    d.Release (920, 350);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, AnEdgeOnNavigationRebaseCanExplicitlyFinishOrCancel)
{
    for (const bool cancel : { false, true }) {
        Drive d (Corner ());
        // Use the 3D mapping seam from the beginning; switching projection kinds must cancel.
        d.input.planar = false;
        d.input.project = [] (double x, double y, double z, float& px, float& py) {
            px = float (600 + 20 * x), py = float (500 - 20 * y - 5 * z);
            return true;
        };
        DragTheXArrow (d);
        const auto preview = d.D ().entities;
        d.input.navigating = true;
        d.input.project = [] (double x, double, double z, float& px, float& py) {
            px = float (600 + 20 * x), py = float (500 - 20 * z); // XY seen edge-on
            return true;
        };
        d.At (950, 350);
        d.input.navigating = false;
        d.Frame ();
        d.Frame ();
        EXPECT_TRUE (d.S ().navigation.blocked);
        EXPECT_TRUE (d.S ().navigation.pending);
        EXPECT_EQ (d.D ().entities, preview);
        EXPECT_NE (d.S ().message.find ("edge-on"), std::string::npos);
        d.Key (cancel ? 0x1B : 0x0D); // explicit even before LMB release
        d.Release (950, 350);
        EXPECT_EQ (d.S ().drag, eh::Drag::None);
        EXPECT_EQ (d.S ().edits.UndoDepth (), cancel ? 0u : 1u);
        EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, cancel ? 10 : 12, 1e-6);
    }
}

TEST (HudVirtualEditor, RadiusNavigationKeepsCapturedScopeAndZoomIndependentGripInset)
{
    ed::Document doc;
    const auto id = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 6, 0 }, { 0, 6, 0 } }, true);
    Drive d (doc);
    d.S ().selection.Set ({ { id, ed::Part::Entity, 0 } });
    const float x = 600 + 20 * (10 - 0.5657f), y = 500 - 20 * 0.5657f;
    d.Press (x, y);
    d.At (x - 4, y - 4);
    ASSERT_EQ (d.S ().drag, eh::Drag::Corner);
    const auto preview = d.D ().entities;
    d.input.navigating = true;
    d.input.plan[0] = 5, d.input.plan[4] = -5;
    d.At (900, 350);
    d.input.navigating = false;
    d.Frame ();
    d.Frame ();
    EXPECT_EQ (d.D ().entities, preview);
    d.At (899.5f, 349.5f);
    for (const auto& v : d.D ().entities[0].vertices)
        EXPECT_GT (v.radius, 0.0); // zoom must not change the grip's sharp-radius threshold
    d.Key (0x1B);
    d.Release (899.5f, 349.5f);
    EXPECT_EQ (d.D ().entities, doc.entities);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);

    ed::Document paths;
    const auto path = ed::AddPath (paths, { { 0, 0, 100 }, { 20, 0, 101 } }, 4);
    paths.paths[0].profile.front ().fixed = paths.paths[0].profile.back ().fixed = true;
    Drive p (paths);
    p.S ().selection.Set ({ { path, ed::Part::Entity, 0 } });
    const auto c = ed::Pave (paths.paths, 0.5, false).corners.front ();
    const auto at = ed::KerbGripAt (c, 0.8);
    p.Press (float (600 + 20 * at.x), float (500 - 20 * at.y));
    ASSERT_EQ (p.S ().drag, eh::Drag::Kerb);
    p.At (float (600 + 20 * (at.x + c.bisector.x)), float (500 - 20 * (at.y + c.bisector.y)));
    const auto pathPreview = p.D ().paths;
    p.input.navigating = true;
    p.input.plan[0] = 40, p.input.plan[4] = -40;
    p.Release (900, 350);
    EXPECT_EQ (p.D ().paths, pathPreview);
    p.input.navigating = false;
    p.Frame ();
    p.Frame ();
    EXPECT_EQ (p.D ().paths, pathPreview);
    p.Key (0x0D);
    ASSERT_EQ (p.D ().paths[0].kerbs.size (), 1u);
    EXPECT_TRUE (p.D ().paths[0].kerbs[0].Same (c.key));
    EXPECT_EQ (p.D ().paths[0].profile, paths.paths[0].profile);
    EXPECT_EQ (p.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, CurveHandlesAndBendingResumeWithoutReplacingTheirBaseline)
{
    ed::Document doc;
    const auto id = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 20, 0, 0 } }, false);
    std::string error;
    ASSERT_TRUE (ed::SetSmooth (doc, id, doc.entities[0].vertices[1].id, true, error)) << error;
    for (const bool bend : { false, true }) {
        Drive d (doc);
        const float x = bend ? 700 : 600 + 20 * (10 + 10.0f / 3.0f);
        if (!bend)
            d.S ().selection.Set ({ { id, ed::Part::Vertex, doc.entities[0].vertices[1].id } });
        d.Press (x, 500);
        ASSERT_EQ (d.S ().drag, bend ? eh::Drag::Bend : eh::Drag::Bezier);
        d.At (x, 460);
        const auto preview = d.D ().entities;
        d.input.navigating = true;
        d.input.plan[2] += 100;
        d.At (900, 350);
        d.input.navigating = false;
        d.Frame ();
        d.Frame ();
        EXPECT_EQ (d.D ().entities, preview);
        EXPECT_EQ (d.S ().edits.Before ().entities, doc.entities);
        d.At (900, 330);
        EXPECT_NE (d.D ().entities, preview);
        d.input.navigating = true;
        d.Release (910, 320);
        EXPECT_TRUE (d.S ().edits.InGesture ());
        d.Key (0x1B);
        EXPECT_EQ (d.D ().entities, doc.entities);
        EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    }
}

TEST (HudVirtualEditor, NavigationRetainsRadiusLocksAndUncommittedTrackerText)
{
    ed::Document doc;
    const auto id = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 6, 0 }, { 0, 6, 0 } }, true);
    Drive d (doc);
    d.S ().selection.Set ({ { id, ed::Part::Entity, 0 } });
    const float x = 600 + 20 * (10 - 0.5657f), y = 500 - 20 * 0.5657f;
    d.Press (x, y);
    d.At (x - 20, y - 20);
    d.input.characters = u"1.5";
    d.Frame ();
    d.Key (0x09);
    ASSERT_TRUE (d.S ().tracker.Locked (ed::Quantity::Radius));
    const auto lockedPreview = d.D ().entities;
    d.input.navigating = true;
    d.input.plan[2] += 100;
    d.At (900, 350);
    d.input.navigating = false;
    d.Frame ();
    d.Frame ();
    d.At (920, 330);
    EXPECT_EQ (d.D ().entities, lockedPreview);
    ASSERT_TRUE (d.S ().tracker.Locked (ed::Quantity::Radius));
    EXPECT_DOUBLE_EQ (*d.S ().tracker.Locked (ed::Quantity::Radius), 1.5);
    d.input.characters = u"2.25";
    d.Frame ();
    d.input.navigating = true;
    d.Release (950, 350);
    EXPECT_EQ (d.S ().tracker.Text (), "2.25");
    EXPECT_EQ (d.D ().entities, lockedPreview);
    d.input.navigating = false;
    d.Frame ();
    d.Frame ();
    d.Key (0x1B); // number first, gesture second
    EXPECT_EQ (d.D ().entities, lockedPreview);
    d.Key (0x1B);
    EXPECT_EQ (d.D ().entities, doc.entities);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, ThePenKeepsItsPointsAndHandlesDuringNavigation)
{
    Drive d ({});
    d.S ().tool = eh::Tool::Curve;
    d.S ().snapModes = 0;
    d.Press (600, 500);
    d.At (640, 480);
    const auto points = d.S ().drawing, handles = d.S ().drawHandles;
    d.input.navigating = true;
    d.input.plan[2] += 100;
    d.At (900, 350);
    d.input.navigating = false;
    d.Frame ();
    d.Frame ();
    EXPECT_EQ (d.S ().drawing, points);
    EXPECT_EQ (d.S ().drawHandles, handles);
    d.At (920, 350);
    EXPECT_NEAR (d.S ().drawHandles[0].x, handles[0].x + 1, 1e-6);
    d.input.navigating = true;
    d.Release (930, 340);
    EXPECT_FALSE (d.S ().penDown);
    EXPECT_EQ (d.S ().drawing, points);
    EXPECT_TRUE (d.D ().entities.empty ());
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, NavigationBeforePenMotionDoesNotPullOutAHandle)
{
    Drive d ({});
    d.S ().tool = eh::Tool::Curve;
    d.S ().snapModes = 0;
    d.Press (600, 500);
    d.input.navigating = true;
    d.input.plan[2] += 100;
    d.At (900, 350);
    d.input.navigating = false;
    d.Frame ();
    d.Frame ();
    d.At (901, 350);
    d.Release (901, 350);
    ASSERT_EQ (d.S ().drawHandles.size (), 1u);
    EXPECT_DOUBLE_EQ (ed::Length (d.S ().drawHandles[0]), 0.0);
}

TEST (HudVirtualEditor, PlaneElevationUsesTheExistingTrackerWithoutEditingTheDocument)
{
    Drive d ({});
    d.S ().tool = eh::Tool::Polyline;
    d.S ().context.height = [] (double, double, double& z) {
        z = 999;
        return true;
    };
    EXPECT_EQ (d.S ().snapModes & ed::kSnapTerrain, 0u); // context availability must not opt in to terrain Z
    const auto revision = d.D ().revision;
    ASSERT_TRUE (eh::BeginPlaneEntry (d.S ()));
    d.Frame ();
    ASSERT_EQ (d.S ().tracker.Items ().size (), 1u);
    EXPECT_EQ (d.S ().tracker.Items ()[0].quantity, ed::Quantity::Elevation);
    d.input.characters = u"12.5";
    d.Frame ();
    d.Key (0x09); // no hidden Tab-lock binding for a plane setting
    EXPECT_EQ (d.S ().tracker.State (), ed::TrackerState::Editing);
    EXPECT_DOUBLE_EQ (d.S ().plane.origin.z, 0);
    d.Key (0x0D);
    EXPECT_FALSE (d.S ().planeTyping);
    EXPECT_DOUBLE_EQ (d.S ().plane.origin.z, 12.5);
    EXPECT_EQ (d.D ().revision, revision);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    d.Press (600, 500);
    d.Release (600, 500);
    ASSERT_TRUE (d.S ().capturedPlane);
    EXPECT_DOUBLE_EQ (d.S ().capturedPlane->origin.z, 12.5);
    d.Press (800, 500);
    d.Release (800, 500);
    d.Key (0x0D);
    ASSERT_EQ (d.D ().entities.size (), 1u);
    for (const auto& v : d.D ().entities[0].vertices)
        EXPECT_DOUBLE_EQ (v.p.z, 12.5);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, PlaneEntryEscapesAndCannotStealAnActiveNumberOrDrag)
{
    Drive d (Corner ());
    const auto original = d.D ().entities;
    d.S ().selection.Set ({ { original[0].id, ed::Part::Entity, 0 } });
    ASSERT_TRUE (eh::BeginPlaneEntry (d.S ()));
    d.Frame ();
    for (const auto key : { 0x08u, 0x2Eu, uint32_t ('S') }) {
        d.Key (key); // the armed field owns destructive/tool keys even before its first digit
        EXPECT_EQ (d.D ().entities, original);
        EXPECT_TRUE (d.S ().planeTyping);
        EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    }
    d.input.characters = u"9";
    d.Frame ();
    d.Key (0x1B);
    EXPECT_FALSE (d.S ().planeTyping);
    EXPECT_TRUE (d.S ().message.empty ());
    EXPECT_DOUBLE_EQ (d.S ().plane.origin.z, 0);
    DragTheXArrow (d);
    EXPECT_FALSE (eh::BeginPlaneEntry (d.S ()));
    EXPECT_EQ (d.S ().drag, eh::Drag::Handle);
    d.Key (0x1B);
    d.Release (900, 300);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, PlaneChangesCancelBeforeQueuedNumericEnterAndKeepOneBaseline)
{
    Drive d (Corner ());
    const auto original = d.D ().entities;
    DragTheXArrow (d);
    const auto preview = d.D ().entities;
    ASSERT_TRUE (d.S ().capturedPlane);
    EXPECT_TRUE (eh::SetWorkingPlane (d.S (), d.S ().plane)); // idempotent setting does not cancel
    EXPECT_EQ (d.D ().entities, preview);
    d.input.characters = u"5";
    d.Frame ();
    d.S ().plane = ed::WorldXY (8); // external frame change, before the next key-dispatch/layout
    d.Key (0x0D);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_FALSE (d.S ().edits.InGesture ());
    EXPECT_EQ (d.D ().entities, original);
    EXPECT_DOUBLE_EQ (d.S ().plane.origin.z, 8);
    d.Release (900, 300);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, PlaneChangesCancelUnfinishedDrawingInsteadOfMixingDatums)
{
    Drive d ({});
    d.S ().tool = eh::Tool::Curve;
    d.S ().snapModes = 0;
    d.Press (600, 500);
    d.At (620, 480);
    d.Release (620, 480);
    ASSERT_FALSE (d.S ().drawing.empty ());
    EXPECT_TRUE (eh::SetWorkingPlane (d.S (), ed::WorldXY (5)));
    EXPECT_TRUE (d.S ().drawing.empty ());
    EXPECT_TRUE (d.S ().drawHandles.empty ());
    EXPECT_FALSE (d.S ().capturedPlane);
    EXPECT_TRUE (d.D ().entities.empty ());
    EXPECT_FALSE (d.S ().message.empty ());
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, LockedAndUnsupportedPlanesRefuseWithoutDisturbingThePreview)
{
    Drive d (Corner ());
    auto locked = ed::WorldXY (2.5);
    locked.locked = true;
    ASSERT_TRUE (eh::SetWorkingPlane (d.S (), locked));
    DragTheXArrow (d);
    const auto preview = d.D ().entities;
    EXPECT_FALSE (eh::SetWorkingPlane (d.S (), ed::WorldXY (8), true));
    EXPECT_TRUE (ed::SamePlane (d.S ().plane, locked));
    auto oblique = locked;
    oblique.source = ed::PlaneSource::Explicit;
    oblique.u = { 0, 1, 0 }, oblique.v = { 0, 0, 1 }, oblique.normal = { 1, 0, 0 };
    EXPECT_FALSE (eh::SetWorkingPlane (d.S (), oblique));
    EXPECT_NE (d.S ().message.find ("oblique"), std::string::npos);
    EXPECT_EQ (d.D ().entities, preview);
    EXPECT_EQ (d.S ().drag, eh::Drag::Handle);
    d.input.planar = false;
    d.Frame (); // view changes cancel the geometry but never reorient or unlock the plane
    EXPECT_TRUE (ed::SamePlane (d.S ().plane, locked));
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, LockingTheCapturedPlanePreservesItsPreviewAndNumericText)
{
    Drive d (Corner ());
    DragTheXArrow (d);
    const auto preview = d.D ().entities;
    d.input.characters = u"5";
    d.Frame ();
    auto plane = d.S ().plane;
    plane.locked = true;
    ASSERT_TRUE (eh::SetWorkingPlane (d.S (), plane));
    EXPECT_EQ (d.S ().drag, eh::Drag::Handle);
    EXPECT_EQ (d.S ().tracker.State (), ed::TrackerState::Editing);
    EXPECT_EQ (d.S ().tracker.Text (), "5");
    EXPECT_EQ (d.D ().entities, preview);
    EXPECT_TRUE (d.S ().capturedPlane->locked);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    d.Key (0x0D);
    d.Release (900, 300);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 15, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, NonfiniteStartupCannotReplaceTheSessionAndSeedGeometryDoesNotChooseThePlane)
{
    auto doc = Corner ();
    for (auto& v : doc.entities[0].vertices)
        v.p.z = 101;
    Drive d (doc);
    EXPECT_DOUBLE_EQ (d.S ().plane.origin.z, 0);
    const auto* session = &d.S ();
    EXPECT_FALSE (hud::StartVirtualEditor (*d.view.state, {}, "invalid", {}, std::numeric_limits<double>::infinity ()));
    EXPECT_EQ (&d.S (), session);
    EXPECT_EQ (d.D ().entities, doc.entities);
    EXPECT_TRUE (hud::StartVirtualEditor (*d.view.state, {}, "explicit", {}, 103.25, true));
    EXPECT_DOUBLE_EQ (d.S ().plane.origin.z, 103.25);
    EXPECT_TRUE (d.S ().plane.locked);
}

TEST (HudVirtualEditor, PerspectiveDrawingResolvesAtTheExplicitPlaneAndEdgeOnRefuses)
{
    Drive d ({});
    d.S ().tool = eh::Tool::Polyline;
    d.S ().snapModes = 0;
    ASSERT_TRUE (eh::SetWorkingPlane (d.S (), ed::WorldXY (2.5)));
    d.input.planar = false;
    d.input.project = [] (double x, double y, double z, float& px, float& py) {
        const double w = 1 + 0.2 * z;
        px = float (600 + (20 * x + 2 * z) / w), py = float (500 - (20 * y + 5 * z) / w);
        return true;
    };
    d.Press (630, 445); // integer physical pixels: ImGui rounds the delivered pointer position
    d.Release (d.input.x, d.input.y);
    ASSERT_EQ (d.S ().drawing.size (), 1u);
    // Map's existing inverse stops within 0.05 physical pixels (0.00375 m at this datum).
    EXPECT_NEAR (d.S ().drawing[0].x, 2, 0.00375);
    EXPECT_NEAR (d.S ().drawing[0].y, 3.5, 0.00375);
    EXPECT_DOUBLE_EQ (d.S ().drawing[0].z, 2.5);
    d.input.project = [] (double x, double, double z, float& px, float& py) {
        px = float (600 + 20 * x), py = float (500 - 20 * z);
        return true;
    };
    d.Press (900, 300);
    d.Release (900, 300);
    EXPECT_EQ (d.S ().drawing.size (), 1u);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    EXPECT_NE (d.S ().message.find ("edge-on"), std::string::npos);
}

TEST (HudVirtualEditor, PickedFacePlaneIsReachableFromTheMenuAndDrawsAtItsDatum)
{
    ed::Document doc;
    const auto id = ed::AddExtrusion (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, 3);
    Drive d (doc);
    d.input.planar = false;
    d.input.project = [] (double x, double y, double z, float& px, float& py) {
        px = float (600 + 20 * x + 5 * z), py = float (500 - 20 * y - 10 * z);
        return true;
    };
    d.input.hasCameraDirection = true;
    d.input.cameraDirection[1] = 0.5, d.input.cameraDirection[2] = -1;
    d.S ().filter = ed::kPickFace;
    d.RightClick (715, 370);
    ASSERT_TRUE (d.Offers ("plane.face"));
    ASSERT_EQ (d.S ().menu.ref, (ed::Ref { id, ed::Part::Face, 1 }));
    eh::RunMenuItem (d.S (), "plane.face");
    d.S ().menu.open = false;
    EXPECT_EQ (d.S ().plane.source, ed::PlaneSource::Face);
    EXPECT_EQ (d.S ().plane.origin, (ed::Vec3 { 5, 5, 3 }));
    EXPECT_EQ (d.S ().plane.sourceRevision, doc.revision);
    EXPECT_EQ (d.D ().entities, doc.entities);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    d.Key (0x1B); // close the ImGui popup, not the editor
    d.S ().tool = eh::Tool::Polyline;
    d.S ().snapModes = 0;
    d.Press (1015, 270); // (20,10,3), beyond the source box
    d.Release (1015, 270);
    ASSERT_EQ (d.S ().drawing.size (), 1u);
    ASSERT_TRUE (d.S ().capturedPlane);
    EXPECT_TRUE (ed::SamePlane (*d.S ().capturedPlane, d.S ().plane));
    d.Press (1115, 270);
    d.Release (1115, 270);
    d.Key (0x0D);
    ASSERT_EQ (d.D ().entities.size (), 2u);
    for (const auto& vertex : d.D ().entities.back ().vertices)
        EXPECT_DOUBLE_EQ (vertex.p.z, 3);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, AFacePickCancelsInputAgainstTheCommittedFaceNotItsPreview)
{
    ed::Document doc;
    const auto id = ed::AddExtrusion (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, 3);
    Drive d (doc);
    ASSERT_TRUE (d.S ().edits.Begin ({ id, ed::Part::Face, 1 }));
    ASSERT_TRUE (d.S ().edits.Move ([id] (ed::Document& candidate, std::string&) {
        ed::Find (candidate, id)->height = 9;
        return true;
    }));
    d.S ().drag = eh::Drag::Handle;
    ASSERT_TRUE (eh::UseFacePlane (d.S (), { id, ed::Part::Face, 1 }));
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_FALSE (d.S ().edits.InGesture ());
    EXPECT_EQ (d.D ().entities, doc.entities);
    EXPECT_DOUBLE_EQ (d.S ().plane.origin.z, 3);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    EXPECT_NE (d.S ().message.find ("cancelled"), std::string::npos);
}

TEST (HudVirtualEditor, SideOrStaleFacePicksPreserveUnfinishedInputAndThePlane)
{
    ed::Document doc;
    const auto id = ed::AddExtrusion (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, 3);
    Drive d (doc);
    d.S ().tool = eh::Tool::Polyline;
    d.S ().drawing = { { 20, 0, 0 } };
    const auto plane = d.S ().plane;
    EXPECT_FALSE (eh::UseFacePlane (d.S (), { id, ed::Part::Face, 2 }));
    EXPECT_NE (d.S ().message.find ("side/oblique"), std::string::npos);
    EXPECT_FALSE (eh::UseFacePlane (d.S (), { id, ed::Part::Face, 999 }));
    EXPECT_TRUE (ed::SamePlane (d.S ().plane, plane));
    EXPECT_EQ (d.S ().drawing.size (), 1u);
    EXPECT_EQ (d.D ().entities, doc.entities);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    ASSERT_TRUE (eh::UseFacePlane (d.S (), { id, ed::Part::Face, 1 }));
    EXPECT_TRUE (d.S ().drawing.empty ());
    EXPECT_EQ (d.S ().plane.source, ed::PlaneSource::Face);
}

TEST (HudVirtualEditor, FaceSnapshotsDoNotFollowDeletedOrMovedSourcesOrOverrideLocksAutomatically)
{
    ed::Document doc;
    const auto id = ed::AddExtrusion (doc, { { 0, 0, 100 }, { 10, 0, 100 }, { 10, 10, 100 }, { 0, 10, 100 } }, 3);
    Drive d (doc);
    auto locked = ed::WorldXY (7);
    locked.locked = true;
    ASSERT_TRUE (eh::SetWorkingPlane (d.S (), locked));
    ed::WorkingPlane candidate;
    std::string error;
    ASSERT_TRUE (ed::PlaneFromFace (d.D (), { id, ed::Part::Face, 0 }, candidate, error));
    EXPECT_FALSE (eh::SetWorkingPlane (d.S (), candidate, true));
    ASSERT_TRUE (eh::UseFacePlane (d.S (), { id, ed::Part::Face, 0 }));
    const auto snapshot = d.S ().plane;
    EXPECT_TRUE (snapshot.locked);
    EXPECT_DOUBLE_EQ (snapshot.normal.z, -1);
    ASSERT_TRUE (d.S ().edits.Apply ([] (ed::Document& candidate, std::string&) {
        candidate.entities[0].height = 5;
        for (auto& vertex : candidate.entities[0].vertices)
            vertex.p.z += 2;
        return true;
    }));
    d.Frame ();
    EXPECT_TRUE (ed::SamePlane (d.S ().plane, snapshot));
    ASSERT_TRUE (d.S ().edits.Apply ([] (ed::Document& candidate, std::string&) {
        candidate.entities.clear ();
        return true;
    }));
    d.Frame ();
    EXPECT_TRUE (ed::SamePlane (d.S ().plane, snapshot));
    EXPECT_FALSE (eh::UseFacePlane (d.S (), snapshot.sourceRef));
    EXPECT_TRUE (ed::SamePlane (d.S ().plane, snapshot));
    ASSERT_TRUE (eh::BeginPlaneEntry (d.S ()));
    d.input.characters = u"8";
    d.Frame ();
    d.Key (0x0D);
    EXPECT_EQ (d.S ().plane.source, ed::PlaneSource::WorldXY);
    EXPECT_DOUBLE_EQ (d.S ().plane.origin.z, 8);
    EXPECT_TRUE (d.S ().plane.locked);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 2u); // only the two document edits, never the plane changes
}

TEST (HudVirtualEditor, DrawingTrackerAnglesUseThePickedFaceAxes)
{
    ed::Document doc;
    const auto id = ed::AddExtrusion (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, 3);
    Drive d (doc);
    ASSERT_TRUE (eh::UseFacePlane (d.S (), { id, ed::Part::Face, 1 }));
    const auto plane = d.S ().plane;
    d.S ().drawing = { plane.origin };
    d.S ().snap.point = plane.origin + plane.u * 4;
    std::vector<ed::Dimension> dims;
    const auto fields = eh::TrackerFields (d.S (), dims);
    ASSERT_EQ (fields.size (), 2u);
    EXPECT_NEAR (fields[0].value, 4, 1e-12);
    EXPECT_NEAR (fields[1].value, 0, 1e-12);
    d.S ().tool = eh::Tool::Polyline;
    d.S ().tracker.Show (fields);
    // Lock a zero angle relative to the face's U, then enter a 2 m length.
    d.S ().tracker.Key ({ ed::TrackerKey::Kind::Tab });
    d.S ().tracker.Key ({ ed::TrackerKey::Kind::Char, '0' });
    d.S ().tracker.Key ({ ed::TrackerKey::Kind::Tab });
    d.input.characters = u"2";
    d.Frame ();
    d.Key (0x0D);
    ASSERT_EQ (d.S ().drawing.size (), 2u);
    EXPECT_NEAR (ed::Distance (d.S ().drawing.back (), plane.origin + plane.u * 2), 0, 1e-12);
}

TEST (HudVirtualEditor, PathPlaneStationUsesTheTrackerAndPreservesTheDocumentAndLock)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 100 }, { 30, 40, 110 } }, 2);
    Drive d (doc);
    auto locked = ed::WorldXY (7);
    locked.locked = true;
    ASSERT_TRUE (eh::SetWorkingPlane (d.S (), locked));
    ASSERT_TRUE (eh::BeginPathPlaneEntry (d.S (), id));
    d.Frame ();
    ASSERT_EQ (d.S ().tracker.Items ().size (), 1u);
    EXPECT_EQ (d.S ().tracker.Items ()[0].quantity, ed::Quantity::Distance);
    EXPECT_FALSE (eh::BeginPlaneEntry (d.S ())); // even the armed, not-yet-typed field owns its binding
    EXPECT_EQ (d.S ().planeEntryPath, id);
    d.input.characters = u"25";
    d.Frame ();
    d.Key (0x09);
    EXPECT_EQ (d.S ().tracker.Text (), "25");
    EXPECT_FALSE (d.S ().tracker.Locked (ed::Quantity::Distance));
    d.Key (0x0D);
    EXPECT_FALSE (d.S ().planeTyping);
    EXPECT_EQ (d.S ().planeEntryPath, ed::kNoId);
    EXPECT_EQ (d.S ().plane.source, ed::PlaneSource::PathPlan);
    EXPECT_EQ (d.S ().plane.origin, (ed::Vec3 { 15, 20, 105 }));
    EXPECT_DOUBLE_EQ (d.S ().plane.sourceStation, 25);
    EXPECT_EQ (d.S ().plane.sourceRevision, doc.revision);
    EXPECT_TRUE (d.S ().plane.locked);
    EXPECT_EQ (d.D ().paths, doc.paths);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    ed::WorkingPlane automatic;
    std::string error;
    ASSERT_TRUE (ed::PlaneFromPath (d.D (), id, 30, ed::PathPlaneKind::Plan, automatic, error));
    automatic.locked = true;
    EXPECT_FALSE (eh::SetWorkingPlane (d.S (), automatic, true));
    EXPECT_DOUBLE_EQ (d.S ().plane.sourceStation, 25);
}

TEST (HudVirtualEditor, PathPlaneEntryIsReachableFromTheActualPathMenu)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 100 }, { 20, 0, 102 } }, 2);
    Drive d (doc);
    d.RightClick (700, 500);
    ASSERT_TRUE (d.S ().menu.open);
    ASSERT_TRUE (d.Offers ("plane.pathStation"));
    eh::RunMenuItem (d.S (), "plane.pathStation");
    EXPECT_TRUE (d.S ().planeTyping);
    EXPECT_EQ (d.S ().planeEntryPath, id);
    EXPECT_EQ (d.S ().tracker.Items ()[0].quantity, ed::Quantity::Distance);
    EXPECT_EQ (d.S ().plane.source, ed::PlaneSource::WorldXY); // opening the field never adopts a default station
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, PathPartPlaneActionsUseIdentityStationsWithoutChangingPickedRangeSplit)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 100 }, { 10, 0, 101 }, { 20, 0, 102 } }, 2);
    auto* path = ed::FindPath (doc, id);
    std::string error;
    const auto grade = ed::NewId (doc), range = ed::NewId (doc);
    ASSERT_TRUE (ed::InsertBreak (path->profile, 7, grade, error));
    ASSERT_TRUE (ed::SplitSection (path->sections, 14, range, error));
    const auto point = path->points[1].id;
    Drive d (doc);
    const ed::Ref refs[] = { { id, ed::Part::PathPoint, point },
                             { id, ed::Part::Break, grade },
                             { id, ed::Part::Range, range } };
    const eh::MenuTarget targets[] = { eh::MenuTarget::PathPoint, eh::MenuTarget::GradeBreak, eh::MenuTarget::Range };
    const double stations[] = { 10, 7, 14 };
    for (size_t i = 0; i < 3; ++i) {
        d.S ().menu.target = targets[i];
        d.S ().menu.ref = refs[i];
        d.S ().menu.at = { 19, 0, 101.9 }; // deliberately not the marker's station
        d.S ().selection.Set ({ refs[i] });
        ASSERT_TRUE (d.Offers ("plane.pathPart"));
        eh::RunMenuItem (d.S (), "plane.pathPart");
        EXPECT_DOUBLE_EQ (d.S ().plane.sourceStation, stations[i]);
        EXPECT_NEAR (d.S ().plane.origin.x, stations[i], 1e-12);
        EXPECT_NEAR (d.S ().plane.origin.z, 100 + stations[i] * 0.1, 1e-12);
        EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    }
    EXPECT_EQ (d.D ().paths, doc.paths);
    eh::RunMenuItem (d.S (), "path.splitRange"); // still splits at the picked place, not the selected range's start
    ASSERT_EQ (d.D ().paths[0].sections.size (), 3u);
    EXPECT_DOUBLE_EQ (d.D ().paths[0].sections.back ().station, 19);
    EXPECT_DOUBLE_EQ (d.S ().plane.sourceStation, 14);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.S ().menu.ref.item = 999;
    const auto snapshot = d.S ().plane;
    eh::RunMenuItem (d.S (), "plane.pathPart");
    EXPECT_TRUE (ed::SamePlane (d.S ().plane, snapshot));
    EXPECT_NE (d.S ().message.find ("existing path"), std::string::npos);
}

TEST (HudVirtualEditor, PathStationRefusalKeepsTextForCorrectionAndEscLeavesThePlaneAlone)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 100 }, { 30, 40, 110 } }, 2);
    Drive d (doc);
    const auto original = d.S ().plane;
    ASSERT_TRUE (eh::BeginPathPlaneEntry (d.S (), id));
    d.input.characters = u"-1";
    d.Frame ();
    d.Key (0x0D);
    EXPECT_TRUE (d.S ().planeTyping);
    EXPECT_EQ (d.S ().tracker.Text (), "-1");
    EXPECT_TRUE (ed::SamePlane (d.S ().plane, original));
    EXPECT_NE (d.S ().message.find ("not clamped"), std::string::npos);
    d.Key (0x1B);
    EXPECT_FALSE (d.S ().planeTyping);
    EXPECT_EQ (d.S ().planeEntryPath, ed::kNoId);
    EXPECT_TRUE (d.S ().message.empty ());
    ASSERT_TRUE (eh::BeginPathPlaneEntry (d.S (), id));
    d.input.characters = u"51";
    d.Frame ();
    d.Key (0x0D);
    EXPECT_EQ (d.S ().tracker.Text (), "51");
    d.Key (0x08);
    d.Key (0x08);
    d.input.characters = u"25";
    d.Frame ();
    d.Key (0x0D);
    EXPECT_EQ (d.S ().plane.source, ed::PlaneSource::PathPlan);
    EXPECT_DOUBLE_EQ (d.S ().plane.sourceStation, 25);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, AQueuedStationEnterCannotRebindToAChangedPathDocument)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 100 }, { 30, 40, 110 } }, 2);
    Drive d (doc);
    const auto original = d.S ().plane;
    ASSERT_TRUE (eh::BeginPathPlaneEntry (d.S (), id));
    d.input.characters = u"25";
    d.Frame ();
    ASSERT_TRUE (d.S ().edits.Apply ([] (ed::Document& candidate, std::string&) {
        candidate.paths[0].profile.back ().z += 1;
        return true;
    }));
    d.Key (0x0D);
    EXPECT_TRUE (d.S ().planeTyping);
    EXPECT_EQ (d.S ().tracker.Text (), "25");
    EXPECT_TRUE (ed::SamePlane (d.S ().plane, original));
    EXPECT_NE (d.S ().message.find ("document changed"), std::string::npos);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.Key (0x1B);
    ASSERT_TRUE (eh::BeginPathPlaneEntry (d.S (), id));
    d.input.characters = u"25";
    d.Frame ();
    d.Key (0x0D);
    EXPECT_DOUBLE_EQ (d.S ().plane.origin.z, 105.5);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, PathPlaneDrawingUsesItsCapturedDatumAndLocalTypedDirection)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 100 }, { 30, 40, 110 } }, 2);
    Drive d (doc);
    ASSERT_TRUE (eh::UsePathPlane (d.S (), id, 25));
    d.S ().tool = eh::Tool::Polyline;
    d.S ().snapModes = 0;
    d.Press (1000, 500); // (20, 0), well away from the path's geometry
    d.Release (1000, 500);
    ASSERT_EQ (d.S ().drawing.size (), 1u);
    ASSERT_TRUE (d.S ().capturedPlane);
    EXPECT_EQ (d.S ().drawing[0], (ed::Vec3 { 20, 0, 105 }));
    d.S ().tracker.Key ({ ed::TrackerKey::Kind::Tab });
    d.S ().tracker.Key ({ ed::TrackerKey::Kind::Char, '0' });
    d.S ().tracker.Key ({ ed::TrackerKey::Kind::Tab });
    d.input.characters = u"2";
    d.Frame ();
    d.Key (0x0D);
    ASSERT_EQ (d.S ().drawing.size (), 2u);
    EXPECT_NEAR (ed::Distance (d.S ().drawing.back (), { 21.2, 1.6, 105 }), 0, 1e-12);
    d.Key (0x0D);
    ASSERT_EQ (d.D ().entities.size (), 1u);
    EXPECT_EQ (d.D ().paths, doc.paths);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, PathPlaneAdoptionCancelsAgainstTheCommittedProfileNotItsPreview)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 100 }, { 30, 40, 110 } }, 2);
    Drive d (doc);
    ASSERT_TRUE (d.S ().edits.Begin ({ id, ed::Part::Entity, 0 }));
    ASSERT_TRUE (d.S ().edits.Move ([] (ed::Document& candidate, std::string&) {
        candidate.paths[0].profile.back ().z = 200;
        return true;
    }));
    d.S ().drag = eh::Drag::Part;
    ASSERT_TRUE (eh::UsePathPlane (d.S (), id, 25));
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_FALSE (d.S ().edits.InGesture ());
    EXPECT_EQ (d.D ().paths, doc.paths);
    EXPECT_DOUBLE_EQ (d.S ().plane.origin.z, 105);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    EXPECT_NE (d.S ().message.find ("cancelled"), std::string::npos);
}

TEST (HudVirtualEditor, VerticalSectionAndStalePathChoicesDoNotDisturbUnfinishedInput)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 100 }, { 30, 40, 110 } }, 2);
    Drive d (doc);
    d.S ().tool = eh::Tool::Polyline;
    d.S ().drawing = { { 20, 0, 0 } };
    const auto plane = d.S ().plane;
    EXPECT_FALSE (eh::UsePathPlane (d.S (), id, 25, ed::PathPlaneKind::Section));
    EXPECT_NE (d.S ().message.find ("Vertical path-section"), std::string::npos);
    EXPECT_FALSE (eh::UsePathPlane (d.S (), 999, 25));
    EXPECT_TRUE (ed::SamePlane (d.S ().plane, plane));
    EXPECT_EQ (d.S ().drawing.size (), 1u);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    ASSERT_TRUE (eh::UsePathPlane (d.S (), id, 25));
    EXPECT_TRUE (d.S ().drawing.empty ());
    EXPECT_EQ (d.S ().plane.source, ed::PlaneSource::PathPlan);
}

TEST (HudVirtualEditor, PathStationSnapshotsDoNotFollowProfileEditsDeletionOrUndo)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 100 }, { 30, 40, 110 } }, 2);
    Drive d (doc);
    ASSERT_TRUE (eh::UsePathPlane (d.S (), id, 25));
    const auto snapshot = d.S ().plane;
    d.S ().tool = eh::Tool::Polyline;
    d.S ().drawing = { { 20, 0, 105 } };
    ASSERT_TRUE (d.S ().edits.Apply ([] (ed::Document& candidate, std::string&) {
        candidate.paths[0].profile.back ().z += 10;
        return true;
    }));
    d.Frame ();
    EXPECT_TRUE (ed::SamePlane (d.S ().plane, snapshot));
    ASSERT_TRUE (d.S ().edits.Apply ([] (ed::Document& candidate, std::string&) {
        candidate.paths.clear ();
        return true;
    }));
    d.Frame ();
    EXPECT_FALSE (eh::UsePathPlane (d.S (), id, 25));
    EXPECT_TRUE (ed::SamePlane (d.S ().plane, snapshot));
    EXPECT_EQ (d.S ().drawing.size (), 1u);
    ASSERT_TRUE (d.S ().edits.Undo ());
    d.Frame ();
    EXPECT_TRUE (ed::SamePlane (d.S ().plane, snapshot));
    ASSERT_TRUE (eh::UsePathPlane (d.S (), id, 25)); // only an explicit repick follows the restored source
    EXPECT_DOUBLE_EQ (d.S ().plane.origin.z, 110);
    EXPECT_TRUE (d.S ().drawing.empty ());
}

TEST (HudVirtualEditor, UnsupportedPointAndPathModesCannotBeChosenByShortcutOrMenu)
{
    Drive d (Corner ());
    d.S ().gizmo = ed::GizmoMode::Gumball;
    d.S ().localAxes = false;
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Vertex, d.D ().entities[0].vertices[1].id } });
    EXPECT_TRUE (eh::GizmoAvailable (d.S (), ed::GizmoMode::Move));
    for (const auto mode :
         { ed::GizmoMode::Scale, ed::GizmoMode::Rotate, ed::GizmoMode::Stretch, ed::GizmoMode::Extrude })
        EXPECT_FALSE (eh::GizmoAvailable (d.S (), mode));
    d.Key ('S');
    EXPECT_EQ (d.S ().gizmo, ed::GizmoMode::Gumball);
    EXPECT_FALSE (d.S ().message.empty ());
    EXPECT_EQ (d.S ().message.find ("path"), std::string::npos); // a lone point is not a path's doing
    d.At (835, 500); // the absent X scale box must not steal a press from the model
    EXPECT_LT (d.S ().hoverHandle, 0);

    ed::Document path;
    const auto id = ed::AddPath (path, { { 0, 0, 0 }, { 10, 0, 1 }, { 10, 10, 2 } }, 2);
    d.S ().edits = ed::Session (path);
    d.S ().selection.Set ({ { id, ed::Part::Entity, 0 } });
    d.Key ('E');
    EXPECT_EQ (d.S ().gizmo, ed::GizmoMode::Gumball);
    EXPECT_FALSE (eh::SetGizmoMode (d.S (), ed::GizmoMode::Extrude));
    d.Key ('T');
    EXPECT_EQ (d.S ().gizmo, ed::GizmoMode::Gumball);
    EXPECT_TRUE (eh::GizmoAvailable (d.S (), ed::GizmoMode::Scale));
    EXPECT_TRUE (eh::GizmoAvailable (d.S (), ed::GizmoMode::Rotate));
}

TEST (HudVirtualEditor, GumballHoldsEveryTransform)
{
    EXPECT_EQ (eh::Session ().gizmo, ed::GizmoMode::Gumball); // what a session starts with
    Drive d (Corner ());
    d.S ().gizmo = ed::GizmoMode::Gumball;
    d.S ().localAxes = false;
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    // On the box's centre (5, 5) -- (700, 400) -- 70 px across: a scale box 35 px out, the arrow
    // 50 to 70 px out, an arc at 60 px between the axes, a side grip on the box's right side.
    const auto kindAt = [&] (float x, float y) {
        d.At (x, y);
        return d.S ().hoverHandle < 0 ? -1
                                      : int (eh::GizmoHandles (d.S (), d.S ().gizmo)[size_t (d.S ().hoverHandle)].kind);
    };
    EXPECT_EQ (kindAt (735, 400), int (ed::HandleKind::ScaleAxis));
    EXPECT_EQ (kindAt (765, 400), int (ed::HandleKind::Axis));
    EXPECT_EQ (kindAt (742, 358), int (ed::HandleKind::Ring));
    EXPECT_EQ (kindAt (724, 376), int (ed::HandleKind::Plane));
    EXPECT_EQ (kindAt (810, 400), int (ed::HandleKind::Side));
    EXPECT_NE (kindAt (700, 400), int (ed::HandleKind::Centre));
    // No uniform-scale square round the pivot any more: Shift on a scale box does that.
    EXPECT_EQ (kindAt (710, 400), -1);
    for (const auto& h : eh::GizmoHandles (d.S (), d.S ().gizmo))
        EXPECT_NE (h.kind, ed::HandleKind::ScaleUniform);

    // The side grip stretches: the right side out by 1 m.
    d.Press (810, 400);
    d.At (830, 400);
    d.Release (830, 400);
    EXPECT_NEAR (d.D ().entities[0].vertices[1].p.x, 11.0, 1e-6);
    EXPECT_NEAR (d.D ().entities[0].vertices[0].p.x, 0.0, 1e-6);
    // The arrow moves, from the new box's centre (5.5, 5): (710, 400).
    d.Press (770, 400);
    d.At (790, 400);
    d.Release (790, 400);
    EXPECT_NEAR (d.D ().entities[0].vertices[0].p.x, 1.0, 1e-6);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 2u);
    // A mode key shows that transform alone; again, the gumball is back.
    d.Key ('R');
    EXPECT_EQ (d.S ().gizmo, ed::GizmoMode::Rotate);
    d.Key ('R');
    EXPECT_EQ (d.S ().gizmo, ed::GizmoMode::Gumball);
}

TEST (HudVirtualEditor, ShiftOnAGumballScaleBoxScalesEveryAxis)
{
    // The X scale box sits 35 px out from the box's centre (5, 5) at (700, 400): 1.75 m.
    // Dragged to 70 px out it doubles; with Shift held, along Y too.
    const auto scaled = [] (bool shift) {
        Drive d (Corner ());
        d.S ().gizmo = ed::GizmoMode::Gumball;
        d.S ().localAxes = false;
        d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
        d.At (735, 400);
        EXPECT_EQ (int (eh::GizmoHandles (d.S (), d.S ().gizmo)[size_t (d.S ().hoverHandle)].kind),
                   int (ed::HandleKind::ScaleAxis));
        d.input.shift = shift;
        d.Press (735, 400);
        d.At (752, 400);
        d.At (770, 400);
        d.Release (770, 400);
        d.input.shift = false;
        EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
        return std::make_pair (d.D ().entities[0].vertices[0].p, d.D ().entities[0].vertices[2].p);
    };
    const auto [axisFirst, axisLast] = scaled (false);
    EXPECT_NEAR (axisFirst.x, -5.0, 1e-6);
    EXPECT_NEAR (axisFirst.y, 0.0, 1e-6);
    EXPECT_NEAR (axisLast.y, 10.0, 1e-6);
    const auto [uniformFirst, uniformLast] = scaled (true);
    EXPECT_NEAR (uniformFirst.x, -5.0, 1e-6);
    EXPECT_NEAR (uniformFirst.y, -5.0, 1e-6);
    EXPECT_NEAR (uniformLast.x, 15.0, 1e-6);
    EXPECT_NEAR (uniformLast.y, 15.0, 1e-6);
}

TEST (HudVirtualEditor, AClickedGumballHandleOffersItsField)
{
    Drive d (Corner ());
    d.S ().gizmo = ed::GizmoMode::Gumball;
    d.S ().localAxes = false;
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    d.At (765, 400); // the X arrow
    ASSERT_GE (d.S ().hoverHandle, 0);
    d.Press (765, 400);
    d.Release (765, 400);
    ASSERT_GE (d.S ().typedHandle, 0);
    // The field shows before anything is typed: the arrow's distance, selected.
    const auto out = d.Frame ();
    ASSERT_EQ (d.S ().tracker.Items ().size (), 1u);
    EXPECT_EQ (d.S ().tracker.Items ()[0].quantity, ed::Quantity::Distance);
    EXPECT_EQ (d.S ().tracker.State (), ed::TrackerState::Showing);
    d.input.characters = u"3";
    d.Frame ();
    EXPECT_EQ (d.S ().tracker.Text (), "3"); // the first key replaced the value
    d.Key (0x0D);
    EXPECT_NEAR (d.D ().entities[0].vertices[0].p.x, 3.0, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
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

TEST (HudVirtualEditor, BackspaceTakesBackTheLastPointDrawn)
{
    Drive d (Corner ());
    d.Press (700, 500); // the first segment, selected
    d.Release (700, 500);
    ASSERT_FALSE (d.S ().selection.Items ().empty ());
    d.Key ('L');
    for (const auto& [x, y] :
         std::vector<std::pair<float, float>> { { 600.f, 100.f }, { 700.f, 100.f }, { 700.f, 160.f } }) {
        d.Press (x, y);
        d.Release (x, y);
    }
    ASSERT_EQ (d.S ().drawing.size (), 3u);
    d.Key (0x08);
    EXPECT_EQ (d.S ().drawing.size (), 2u);
    ASSERT_EQ (d.D ().entities.size (), 1u); // the selected corner is still there
    EXPECT_EQ (d.D ().entities[0].vertices.size (), 3u);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    d.Key (0x0D);
    ASSERT_EQ (d.D ().entities.size (), 2u);
    EXPECT_EQ (d.D ().entities[1].vertices.size (), 2u);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);

    // The pen's point goes with its handle; taken back to nothing, the line is simply not there.
    d.Key ('B');
    d.Press (600, 600);
    d.Release (600, 600);
    d.Press (700, 600);
    d.At (720, 620); // pulled out: a smooth anchor
    d.Release (720, 620);
    ASSERT_EQ (d.S ().drawing.size (), 2u);
    d.Key (0x08);
    EXPECT_EQ (d.S ().drawing.size (), 1u);
    EXPECT_LE (d.S ().drawHandles.size (), 1u);
    d.Key (0x08);
    EXPECT_TRUE (d.S ().drawing.empty ());
    EXPECT_EQ (d.D ().entities.size (), 2u);
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

TEST (HudVirtualEditor, PerspectiveGizmoSizeDoesNotFollowThePointerDepth)
{
    Drive d (Corner ());
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Vertex, d.D ().entities[0].vertices.back ().id } });
    d.input.planar = false;
    d.input.project = [] (double x, double y, double z, float& px, float& py) {
        const double w = 1 + 0.03 * y + 0.015 * z;
        px = float (600 + 20 * x / w), py = float (500 - (20 * y + 5 * z) / w);
        return w > 0;
    };
    const auto extent = [&] (float x, float y) {
        d.input.x = x, d.input.y = y;
        const auto layout = d.Frame ();
        double maxX = -1e9;
        for (const auto& vertex : layout.overlay.vertices)
            if (vertex.modelAnchored)
                maxX = std::max (maxX, vertex.model[0]);
        return maxX;
    };
    const double near = extent (1100, 100);
    const double far = extent (1100, 700);
    ASSERT_GT (near, 10);
    EXPECT_DOUBLE_EQ (near, far) << "the manipulator is sized at its pivot, not under the pointer";
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
    d.RightClick (1000, 100); // empty canvas, clear of the expanded working-plane miniGUI
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
    EXPECT_FALSE (eh::GizmoHandles (d.S (), ed::GizmoMode::Gumball)[0].movable);
    EXPECT_TRUE (d.Offers ("gizmo.reposition"));
    eh::RunMenuItem (d.S (), "gizmo.reposition");
    ASSERT_TRUE (eh::GumballRepositioning (d.S ()));
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
    EXPECT_FALSE (eh::GumballRepositioning (d.S ()));
    EXPECT_FALSE (eh::GizmoHandles (d.S (), ed::GizmoMode::Gumball)[0].movable);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    d.S ().selection.Clear (); // another selection has its own centre
    EXPECT_NEAR (eh::GizmoFrame (d.S ()).origin.x, 0.0, 1e-9);
}

TEST (HudVirtualEditor, RepositionGumballRequiresMenuActivationAndEscCancelsItsPreview)
{
    Drive d (Corner ());
    d.S ().gizmo = ed::GizmoMode::Gumball;
    d.S ().localAxes = false;
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    const auto before = d.D ();
    EXPECT_FALSE (eh::GumballRepositioning (d.S ()));
    const auto handles = eh::GizmoHandles (d.S (), d.S ().gizmo);
    EXPECT_FALSE (handles[0].movable);
    d.At (700, 400);
    EXPECT_NE (d.S ().hoverHandle, 0);
    eh::RunMenuItem (d.S (), "gizmo.reposition");
    ASSERT_TRUE (eh::GumballRepositioning (d.S ()));
    EXPECT_TRUE (d.Offers ("gizmo.reposition.cancel"));
    EXPECT_FALSE (eh::SetGizmoMode (d.S (), ed::GizmoMode::Rotate));
    d.Press (700, 400);
    d.At (740, 400);
    ASSERT_EQ (d.S ().drag, eh::Drag::Handle);
    EXPECT_EQ (d.S ().heldControl.kind, ed::HandleKind::Centre);
    d.Key (0x1B);
    d.Release (740, 400);
    EXPECT_FALSE (eh::GumballRepositioning (d.S ()));
    EXPECT_FALSE (d.S ().pivot);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_EQ (d.D ().entities, before.entities);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    EXPECT_FALSE (d.S ().close);
    ASSERT_TRUE (eh::BeginGumballReposition (d.S ()));
    d.S ().selection.Clear ();
    d.Frame ();
    EXPECT_TRUE (d.S ().gumballRepositionFor.empty ());
    d.S ().selection.Set ({ { before.entities[0].id, ed::Part::Entity, 0 } });
    EXPECT_FALSE (eh::GumballRepositioning (d.S ()));
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

TEST (HudVirtualEditor, AClickOnAGumballHandleMovesNothing)
{
    Drive d (Corner ());
    d.S ().localAxes = false;
    d.Press (800, 300); // the vertex at (10, 10)
    d.Release (800, 300);
    // The XY plane's square: 0.25 to 0.45 of the gizmo's 70 px along X and Y.
    d.At (824, 276);
    ASSERT_GE (d.S ().hoverHandle, 0);
    ASSERT_EQ (eh::GizmoHandles (d.S (), d.S ().gizmo)[size_t (d.S ().hoverHandle)].kind, ed::HandleKind::Plane);
    d.Press (824, 276);
    d.At (826, 277); // a tremor inside the drag threshold
    d.Release (826, 277);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 10.0, 1e-12);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.y, 10.0, 1e-12);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    // Past the threshold the same handle drags.
    d.Press (824, 276);
    d.At (844, 276);
    d.Release (844, 276);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 11.0, 1e-6);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, AClickOnAFaceDoesNotPushIt)
{
    ed::Document doc;
    ed::AddExtrusion (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, 3.0);
    Drive d (std::move (doc));
    // A 3D view looking down and north: a face is picked only where it faces the camera.
    d.input.planar = false;
    d.input.project = [] (double x, double y, double z, float& px, float& py) {
        px = float (600 + 20 * x + 5 * z), py = float (500 - 20 * y - 10 * z);
        return true;
    };
    d.input.hasCameraDirection = true;
    d.input.cameraDirection[1] = 0.5, d.input.cameraDirection[2] = -1.0;
    d.S ().filter = ed::kPickFace;
    d.At (715, 370); // the top's middle, (5, 5, 3)
    ASSERT_EQ (d.S ().selection.hover.part, ed::Part::Face);
    d.Press (715, 370);
    d.At (716, 372); // a tremor inside the drag threshold
    d.Release (716, 372);
    EXPECT_EQ (d.S ().selection.Kind (), ed::SelectionKind::Face);
    EXPECT_NEAR (d.D ().entities[0].height, 3.0, 1e-12);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, AFacePressPushesAlongTheModesOwnFaceControl)
{
    ed::Document doc;
    ed::AddExtrusion (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, 3.0);
    Drive d (std::move (doc));
    // The same 3D view as the test above: the top face at (5, 5, 3).
    d.input.planar = false;
    d.input.project = [] (double x, double y, double z, float& px, float& py) {
        px = float (600 + 20 * x + 5 * z), py = float (500 - 20 * y - 10 * z);
        return true;
    };
    d.input.hasCameraDirection = true;
    d.input.cameraDirection[1] = 0.5, d.input.cameraDirection[2] = -1.0;
    d.S ().filter = ed::kPickFace;
    d.S ().gizmo = ed::GizmoMode::Extrude; // a face's push/pull: Extrude's own handle
    d.At (715, 370);
    ASSERT_EQ (d.S ().selection.hover.part, ed::Part::Face);
    d.Press (715, 370);
    // The gesture resolved the mode's face control: a real slot, never a sentinel, so the
    // drawing, the drag and a mid-drag number all name the same captured handle.
    ASSERT_EQ (d.S ().drag, eh::Drag::Handle);
    const auto handles = eh::GizmoHandles (d.S (), d.S ().gizmo);
    EXPECT_EQ (d.S ().handle, eh::GizmoFaceSlot (handles));
    EXPECT_GE (d.S ().handle, 0);
    EXPECT_EQ (d.S ().heldControl.kind, ed::HandleKind::Face);
    EXPECT_TRUE (d.S ().facePush);
    // ... and that slot is the one a drag draws, so the arrow stays visible through the push.
    bool drawn = false;
    for (const auto& h : handles)
        drawn = drawn || (int (h.index) == d.S ().handle && h.movable && h.kind == ed::HandleKind::Face);
    EXPECT_TRUE (drawn);
    d.At (715, 340); // up the screen: the top lifts along its normal
    ASSERT_EQ (d.S ().drag, eh::Drag::Handle);
    const double lifted = d.D ().entities[0].height;
    EXPECT_GT (lifted, 3.4);
    d.Release (715, 340);
    EXPECT_NEAR (d.D ().entities[0].height, lifted, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);

    // Esc gives the push back; a click on the face is still only a select (no field, no push).
    d.Press (715, 370);
    d.At (715, 340);
    EXPECT_EQ (eh::HeldControl (d.S ()).kind, ed::HandleKind::Face);
    d.Key (0x1B);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_NEAR (d.D ().entities[0].height, lifted, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.Press (715, 370);
    d.Release (715, 370);
    EXPECT_EQ (d.S ().typedHandle, -1);
    EXPECT_NEAR (d.D ().entities[0].height, lifted, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, AForcedModeChangeDuringAGumballDragIsRefused)
{
    Drive d (Corner ());
    d.S ().gizmo = ed::GizmoMode::Gumball;
    d.S ().localAxes = false;
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    d.Press (765, 400); // the gumball's X arrow
    ASSERT_EQ (d.S ().drag, eh::Drag::Handle);
    EXPECT_EQ (d.S ().heldControl.kind, ed::HandleKind::Axis);
    d.At (785, 400);
    ASSERT_NEAR (d.D ().entities[0].vertices[2].p.x, 11.0, 1e-6);
    // A mode change under the gesture would re-name the control it captured: refused, before
    // anything is written, so the drag keeps resolving its own handle (never a stale index).
    const auto mode = d.S ().gizmo;
    EXPECT_FALSE (eh::SetGizmoMode (d.S (), ed::GizmoMode::Rotate));
    EXPECT_EQ (d.S ().gizmo, mode);
    EXPECT_FALSE (d.S ().message.empty ());
    EXPECT_EQ (d.S ().drag, eh::Drag::Handle);
    d.At (805, 400);
    EXPECT_NEAR (d.D ().entities[0].vertices[2].p.x, 12.0, 1e-6);
    d.Release (805, 400);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    // With the gesture over the same change applies.
    EXPECT_TRUE (eh::SetGizmoMode (d.S (), ed::GizmoMode::Rotate));
    EXPECT_EQ (d.S ().gizmo, ed::GizmoMode::Rotate);
}

TEST (HudVirtualEditor, AnExtrusionKeepsOnePivotInEveryMode)
{
    ed::Document doc;
    ed::AddExtrusion (doc, { { 0, 0, 0 }, { 6, 0, 0 }, { 6, 6, 0 }, { 0, 6, 0 } }, 3.0);
    Drive d (std::move (doc));
    d.S ().localAxes = false;
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    ed::Frame box;
    d.S ().gizmo = ed::GizmoMode::Gumball;
    box = eh::GizmoFrame (d.S ());
    // The shape's box, not its base: an extrusion is measured to its top.
    EXPECT_NEAR (box.origin.x, 3.0, 1e-9);
    EXPECT_NEAR (box.origin.z, 1.5, 1e-9);
    EXPECT_NEAR (box.half[2], 1.5, 1e-9);
    for (const auto mode :
         { ed::GizmoMode::Move, ed::GizmoMode::Rotate, ed::GizmoMode::Scale, ed::GizmoMode::Stretch }) {
        d.S ().gizmo = mode;
        const ed::Frame f = eh::GizmoFrame (d.S ());
        EXPECT_NEAR (f.origin.x, box.origin.x, 1e-9) << int (mode);
        EXPECT_NEAR (f.origin.y, box.origin.y, 1e-9) << int (mode);
        EXPECT_NEAR (f.origin.z, box.origin.z, 1e-9) << int (mode); // the pivot never jumps
    }
}

TEST (HudVirtualEditor, AMixedSelectionThatSupportsNothingFallsBackToTheGumballOnce)
{
    ed::Document doc;
    const ed::Id path = ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 } }, 2.0);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { path, ed::Part::Entity, 0 } });
    ASSERT_TRUE (eh::SetGizmoMode (d.S (), ed::GizmoMode::Rotate));
    // A range beside the whole path keeps its own width control: no handle applies to the pair.
    d.S ().selection.Set ({ { path, ed::Part::Entity, 0 }, { path, ed::Part::Range, d.D ().paths[0].sections[0].id } });
    EXPECT_FALSE (eh::GizmoAvailable (d.S (), ed::GizmoMode::Rotate));
    bool drawn = false;
    for (const auto& h : eh::GizmoHandles (d.S (), ed::GizmoMode::Rotate))
        drawn = drawn || h.movable; // nothing is drawn, so nothing need reserve label space
    EXPECT_FALSE (drawn);
    d.Frame ();
    EXPECT_EQ (d.S ().gizmo, ed::GizmoMode::Gumball);
    EXPECT_FALSE (d.S ().message.empty ()); // the fallback is said
    d.S ().message.clear ();
    d.Frame ();
    EXPECT_TRUE (d.S ().message.empty ()); // and not said again every frame
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

TEST (HudVirtualEditor, RadiusGripsWaitForTheDragThreshold)
{
    ed::Document doc;
    const ed::Id shape = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 6, 0 }, { 0, 6, 0 } }, true);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { shape, ed::Part::Entity, 0 } });
    const auto before = d.D ().entities;
    const auto revision = d.D ().revision;
    const float gx = 600 + 20 * (10 - 0.5657f), gy = 500 - 20 * 0.5657f;
    d.Press (gx + 7, gy - 7); // inside the grip's hit area but not on its centre
    ASSERT_EQ (d.S ().drag, eh::Drag::Corner);
    EXPECT_EQ (d.D ().entities, before);
    EXPECT_EQ (d.D ().revision, revision);
    d.At (gx + 8, gy - 7);
    EXPECT_EQ (d.D ().entities, before);
    EXPECT_EQ (d.D ().revision, revision);
    d.Release (gx + 8, gy - 7);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);

    ed::Document paths;
    const ed::Id path = ed::AddPath (paths, { { 0, 0, 0 }, { 20, 0, 0 } }, 2.0);
    Drive p (std::move (paths));
    p.S ().selection.Set ({ { path, ed::Part::Entity, 0 } });
    const auto original = p.D ().paths;
    const auto pathRevision = p.D ().revision;
    const auto c = ed::Pave (p.D ().paths, 0.5, false).corners.front ();
    const auto at = ed::KerbGripAt (c, 0.8);
    const float x = 600 + 20 * float (at.x) + 7, y = 500 - 20 * float (at.y) - 7;
    p.Press (x, y);
    ASSERT_EQ (p.S ().drag, eh::Drag::Kerb);
    EXPECT_EQ (p.D ().paths, original);
    EXPECT_EQ (p.D ().revision, pathRevision);
    p.At (x + 1, y);
    EXPECT_EQ (p.D ().paths, original);
    EXPECT_EQ (p.D ().revision, pathRevision);
    p.Release (x + 1, y);
    EXPECT_EQ (p.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, ATypedRadiusThroughACornerDragKeepsItsScope)
{
    ed::Document doc;
    const ed::Id shape = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 6, 0 }, { 0, 6, 0 } }, true);
    Drive d (std::move (doc));
    const auto original = d.D ().entities;
    d.S ().selection.Set ({ { shape, ed::Part::Entity, 0 } });
    const float gx = 600 + 20 * (10 - 0.5657f), gy = 500 - 20 * 0.5657f;
    d.Press (gx, gy);
    d.At (gx - 20, gy - 20);
    ASSERT_EQ (d.S ().drag, eh::Drag::Corner);
    ASSERT_EQ (d.S ().tracker.Items ().size (), 1u);
    EXPECT_EQ (d.S ().tracker.Items ()[0].quantity, ed::Quantity::Radius);
    const auto preview = d.D ().entities;
    d.input.characters = u"1.25";
    d.Frame ();
    d.At (gx - 40, gy - 40);
    EXPECT_EQ (d.D ().entities, preview);
    d.Release (gx - 40, gy - 40);
    EXPECT_TRUE (d.S ().edits.InGesture ());
    d.Key (0x0D);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    for (const auto& v : d.D ().entities[0].vertices)
        EXPECT_DOUBLE_EQ (v.radius, 1.25);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.input.keys = { { 'Z', true, false, true } };
    d.Frame ();
    EXPECT_EQ (d.D ().entities, original);
    d.input.keys = { { 'Y', true, false, true } };
    d.Frame ();
    for (const auto& v : d.D ().entities[0].vertices)
        EXPECT_DOUBLE_EQ (v.radius, 1.25);
}

TEST (HudVirtualEditor, ACornerDragValidatesAndLocksItsNumericRadius)
{
    ed::Document doc;
    const ed::Id shape = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 6, 0 }, { 0, 6, 0 } }, true);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { shape, ed::Part::Entity, 0 } });
    const float gx = 600 + 20 * (10 - 0.5657f), gy = 500 - 20 * 0.5657f;
    d.Press (gx, gy);
    d.At (gx - 20, gy - 20);
    const auto preview = d.D ().entities;
    for (const auto text : { u"-1", u"4" }) { // negative, then beyond the 3 m all-corner fit
        d.input.characters = text;
        d.Frame ();
        d.Key (0x09);
        EXPECT_EQ (d.S ().tracker.State (), ed::TrackerState::Editing);
        EXPECT_FALSE (d.S ().message.empty ());
        EXPECT_EQ (d.D ().entities, preview);
        while (!d.S ().tracker.Text ().empty ())
            d.Key (0x08);
    }
    d.input.characters = u"1.5";
    d.Frame ();
    d.Key (0x09);
    ASSERT_TRUE (d.S ().tracker.Locked (ed::Quantity::Radius));
    d.At (gx - 50, gy - 50);
    for (const auto& v : d.D ().entities[0].vertices)
        EXPECT_DOUBLE_EQ (v.radius, 1.5);
    d.Release (gx - 50, gy - 50);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, EscapeUnwindsARadiusFieldThenItsGripDrag)
{
    ed::Document doc;
    const ed::Id path = ed::AddPath (doc, { { 0, 0, 100 }, { 10, 0, 101 }, { 10, 10, 102 } }, 1.5);
    Drive d (std::move (doc));
    const auto original = d.D ().paths;
    d.S ().selection.Set ({ { path, ed::Part::Entity, 0 } });
    const float gx = 600 + 20 * (10 - 0.5657f), gy = 500 - 20 * 0.5657f;
    d.Press (gx, gy);
    d.At (gx - 20, gy - 20);
    ASSERT_EQ (d.S ().drag, eh::Drag::Corner);
    const auto preview = d.D ().paths;
    d.input.characters = u"2";
    d.Frame ();
    d.Release (gx - 40, gy - 40);
    d.Key (0x1B);
    EXPECT_EQ (d.S ().drag, eh::Drag::Corner);
    EXPECT_EQ (d.D ().paths, preview);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    d.Key (0x1B);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_EQ (d.D ().paths, original);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, ATypedBendRadiusPreservesFixedProfileElevations)
{
    ed::Document doc;
    const ed::Id path = ed::AddPath (doc, { { 0, 0, 100 }, { 10, 0, 101 }, { 10, 10, 102 } }, 1.5);
    doc.paths[0].profile.front ().fixed = true;
    doc.paths[0].profile.back ().fixed = true;
    Drive d (std::move (doc));
    const auto original = d.D ().paths;
    d.S ().selection.Set ({ { path, ed::Part::Entity, 0 } });
    const float gx = 600 + 20 * (10 - 0.5657f), gy = 500 - 20 * 0.5657f;
    d.Press (gx, gy);
    d.At (gx - 20, gy - 20);
    d.input.characters = u"2.25";
    d.Frame ();
    d.Key (0x0D);
    d.Release (gx - 20, gy - 20);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    const auto& p = d.D ().paths[0];
    EXPECT_DOUBLE_EQ (p.points[1].radius, 2.25);
    ASSERT_EQ (p.sections.size (), original[0].sections.size ());
    for (size_t i = 0; i < p.sections.size (); ++i) {
        EXPECT_EQ (p.sections[i].id, original[0].sections[i].id);
        EXPECT_DOUBLE_EQ (p.sections[i].widthLeft, original[0].sections[i].widthLeft);
        EXPECT_DOUBLE_EQ (p.sections[i].widthRight, original[0].sections[i].widthRight);
        EXPECT_DOUBLE_EQ (p.sections[i].crossfall, original[0].sections[i].crossfall);
        EXPECT_EQ (p.sections[i].fall, original[0].sections[i].fall);
    }
    ASSERT_EQ (p.profile.size (), original[0].profile.size ());
    for (size_t i = 0; i < p.profile.size (); ++i) {
        EXPECT_EQ (p.profile[i].id, original[0].profile[i].id);
        EXPECT_DOUBLE_EQ (p.profile[i].z, original[0].profile[i].z);
        EXPECT_EQ (p.profile[i].fixed, original[0].profile[i].fixed);
    }
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.input.keys = { { 'Z', true, false, true } };
    d.Frame ();
    EXPECT_EQ (d.D ().paths, original);
}

TEST (HudVirtualEditor, ATypedKerbDragSetsOnlyItsCapturedCorner)
{
    ed::Document doc;
    const ed::Id path = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 0 } }, 4.0);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { path, ed::Part::Entity, 0 } });
    const auto original = d.D ().paths;
    const auto c = ed::Pave (d.D ().paths, 0.5, false).corners.front ();
    const auto at = ed::KerbGripAt (c, 0.8), to = at + c.bisector;
    const auto screen = [] (ed::Vec3 p) { return ImVec2 { 600 + 20 * float (p.x), 500 - 20 * float (p.y) }; };
    const auto g = screen (at), t = screen (to);
    d.Press (g.x, g.y);
    d.At (t.x, t.y);
    ASSERT_EQ (d.S ().drag, eh::Drag::Kerb);
    ASSERT_EQ (d.S ().tracker.Items ().size (), 1u);
    EXPECT_EQ (d.S ().tracker.Items ()[0].quantity, ed::Quantity::Radius);
    const auto preview = d.D ().paths;
    d.input.characters = u"-1";
    d.Frame ();
    d.Release (t.x + 30, t.y + 30);
    d.Key (0x0D); // negative input is still refused, not silently made sharp
    ASSERT_EQ (d.S ().tracker.State (), ed::TrackerState::Editing);
    EXPECT_EQ (d.D ().paths, preview);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    d.Key (0x08);
    d.Key (0x08);
    d.input.characters = u"8.5"; // preserve the request beyond the current fitted maximum
    d.Frame ();
    d.Key (0x0D);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_FALSE (d.S ().edits.InGesture ());
    ASSERT_EQ (d.D ().paths[0].kerbs.size (), 1u);
    EXPECT_TRUE (d.D ().paths[0].kerbs[0].Same (c.key));
    EXPECT_DOUBLE_EQ (d.D ().paths[0].kerbs[0].radius, 8.5);
    bool limited = false;
    for (const auto& fitted : ed::Pave (d.D ().paths, 0.5, false).corners)
        if (fitted.key.Same (c.key)) {
            EXPECT_DOUBLE_EQ (fitted.requestedRadius, 8.5);
            EXPECT_DOUBLE_EQ (fitted.key.radius, fitted.maxRadius);
            limited = fitted.Limited ();
        }
    EXPECT_TRUE (limited);
    EXPECT_DOUBLE_EQ (d.D ().paths[0].edgeRadius, 0.0);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.input.keys = { { 'Z', true, false, true } };
    d.Frame ();
    EXPECT_EQ (d.D ().paths, original);
}

TEST (HudVirtualEditor, ATypedJunctionRadiusCanBeCancelledOrCommitted)
{
    ed::Document doc;
    const ed::Id path = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 0 } }, 2.0);
    ed::AddPath (doc, { { 10, 0, 0 }, { 10, 10, 0 } }, 2.0);
    const auto original = doc.paths;
    ed::PavedCorner c;
    for (const auto& corner : ed::Pave (doc.paths, 0.5, false).corners)
        if (corner.key.other != ed::kNoId)
            c = corner;
    ASSERT_NE (c.key.other, ed::kNoId);
    const auto screen = [] (ed::Vec3 p) { return ImVec2 { 600 + 20 * float (p.x), 500 - 20 * float (p.y) }; };
    const auto g = screen (ed::KerbGripAt (c, 0.8)), to = screen (ed::KerbGripAt (c, 0.8) + c.bisector);
    for (const bool cancel : { true, false }) {
        // Independent gestures, not two immediate same-position presses (the grip's sharp action).
        Drive d (doc);
        d.S ().selection.Set ({ { path, ed::Part::Entity, 0 } });
        SCOPED_TRACE (cancel);
        d.Press (g.x, g.y);
        d.At (to.x, to.y);
        ASSERT_EQ (d.S ().drag, eh::Drag::Kerb);
        const auto preview = d.D ().paths;
        d.input.characters = u"0.75";
        d.Frame ();
        d.Release (to.x, to.y);
        if (cancel) {
            d.Key (0x1B);
            EXPECT_EQ (d.D ().paths, preview);
            d.Key (0x1B);
            EXPECT_EQ (d.D ().paths, original);
            EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
        }
        else {
            d.Key (0x0D);
            const auto* p = ed::FindPath (d.D (), c.path);
            ASSERT_NE (p, nullptr);
            ASSERT_EQ (p->kerbs.size (), 1u);
            EXPECT_TRUE (p->kerbs[0].Same (c.key));
            EXPECT_DOUBLE_EQ (p->kerbs[0].radius, 0.75);
            EXPECT_DOUBLE_EQ (p->edgeRadius, 0.0);
            EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
        }
        EXPECT_FALSE (d.S ().edits.InGesture ());
    }
}

TEST (HudVirtualEditor, ALimitedKerbLabelEditsItsUnroundedRequestedRadius)
{
    ed::Document doc;
    const ed::Id path = ed::AddPath (doc, { { 0, 0, 0 }, { 8, 0, 0 } }, 4.0);
    const double request = 10.123456789;
    std::string error;
    ASSERT_TRUE (ed::SetPathEdgeRadius (doc, path, request, error)) << error;
    Drive d (std::move (doc));
    d.input.plan[0] = 100, d.input.plan[4] = -100, d.input.plan[2] = 300;
    d.At (1100, 100);
    bool purple = false;
    for (const auto& vertex : d.Frame ().overlay.vertices)
        purple = purple || vertex.rgba == geomsrv::archviz::hudshell::Unpacked (IM_COL32 (122, 63, 196, 255));
    EXPECT_TRUE (purple);
    eh::Session::PlacedLabel label;
    bool found = false;
    for (const auto& candidate : d.S ().placed)
        if (candidate.target.limitedRadius && candidate.box.x0 > 0 && candidate.box.y0 > 0) {
            label = candidate;
            found = true;
            break;
        }
    ASSERT_TRUE (found);
    const float x = (label.box.x0 + label.box.x1) * 0.5f, y = (label.box.y0 + label.box.y1) * 0.5f;
    d.Press (x, y);
    d.Release (x, y);
    ASSERT_TRUE (d.S ().labelOpen);
    double edited = 0;
    ASSERT_TRUE (ed::ParseNumber (d.S ().labelText, edited));
    EXPECT_DOUBLE_EQ (edited, request); // not the smaller fitted text, and not rounded to two decimals
    const auto before = d.D ().paths;
    d.Key (0x0D);
    EXPECT_FALSE (d.S ().labelOpen);
    EXPECT_EQ (d.D ().paths, before);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, SwitchingPlanAnd3DCancelsAnyGesture)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 6, 0 }, { 0, 6, 0 } }, true);
    Drive d (std::move (doc));
    const ed::Document start = d.D ();
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    const float gx = 600 + 20 * (10 - 0.5657f), gy = 500 - 20 * 0.5657f; // the corner grip at (10, 0)
    d.At (gx, gy);
    ASSERT_GE (d.S ().hoverCorner, 0);
    d.Press (gx, gy);
    d.At (gx - 20, gy - 20);
    ASSERT_EQ (d.S ().drag, eh::Drag::Corner);
    ASSERT_GT (d.D ().entities[0].vertices[1].radius, 0.0); // the preview rounds it
    // The same picture, now as 3D's projection: the grip's drag cannot carry over.
    d.input.planar = false;
    d.input.project = [] (double x, double y, double, float& px, float& py) {
        px = float (600 + 20 * x), py = float (500 - 20 * y);
        return true;
    };
    d.At (gx - 24, gy - 24);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_FALSE (d.S ().edits.InGesture ());
    EXPECT_EQ (d.D ().entities, start.entities);
    EXPECT_FALSE (d.S ().message.empty ());
    d.Release (gx - 24, gy - 24);
    EXPECT_EQ (d.D ().entities, start.entities);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);

    // A gumball handle waiting for its number is let go too.
    d.input.planar = true;
    d.Press (800, 500); // the vertex at (10, 0)
    d.Release (800, 500);
    d.At (840, 500); // its X arrow
    ASSERT_GE (d.S ().hoverHandle, 0);
    d.Press (840, 500);
    d.Release (840, 500);
    ASSERT_GE (d.S ().typedHandle, 0);
    d.input.planar = false;
    d.Frame ();
    EXPECT_EQ (d.S ().typedHandle, -1);
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
    // A tee: B (2 m wide) runs up from A's centreline, its arms long enough for the radii
    // asked (a corner keeps to half of each leg). A is selected whole.
    ed::Document doc;
    const ed::Id a = ed::AddPath (doc, { { 0, 0, 0 }, { 32, 0, 0 } }, 2.0);
    ed::AddPath (doc, { { 16, 0, 0 }, { 16, 16, 0 } }, 2.0);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { a, ed::Part::Entity, 0 } });
    const auto screen = [] (ed::Vec3 p) { return ImVec2 { 600 + 20 * float (p.x), 500 - 20 * float (p.y) }; };
    const auto corner = [&] (bool junction) { // the one of that kind nearest A's start, in view
        ed::PavedCorner best;
        best.at.x = 1e9;
        for (const auto& c : ed::Pave (d.D ().paths, 0.5, false).corners)
            if ((c.key.other != ed::kNoId) == junction && c.Involves (a) && c.at.x < best.at.x)
                best = c;
        return best;
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
    EXPECT_NEAR (pa.kerbs[0].radius, 2.8 / (1.0 / std::sin (j.half) - 1.0), 0.12); // a pixel is 0.12 m of radius here
    EXPECT_DOUBLE_EQ (pa.edgeRadius, 0.0);
    // An own corner's grip -- the path selected whole -- sets that corner alone too, clamped
    // to most of its half width: the path's radius, and so its other corners, stay as they were.
    const ed::PavedCorner own = corner (false);
    const ImVec2 h = screen (ed::KerbGripAt (own, 0.8)), out = screen (ed::KerbGripAt (own, 0.8) + own.bisector * 3.0);
    d.At (h.x, h.y);
    ASSERT_GE (d.S ().hoverKerb, 0);
    d.Press (h.x, h.y);
    d.At (out.x, out.y);
    d.Release (out.x, out.y);
    const ed::Path& after = *ed::FindPath (d.D (), a);
    EXPECT_DOUBLE_EQ (after.edgeRadius, 0.0);
    ASSERT_EQ (after.kerbs.size (), 2u);
    EXPECT_TRUE (after.kerbs[1].Same (own.key));
    EXPECT_NEAR (after.kerbs[1].radius, 0.98, 1e-9);
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

TEST (HudVirtualEditor, NothingFollowsThePointerButWhatIsTyped)
{
    // A path selected, its labels and dimensions shown: moving the pointer over empty view
    // moves nothing -- no label steps aside, and the selection's values are the panel's, not a
    // box beside the pointer.
    ed::Document doc;
    const ed::Id a = ed::AddPath (doc, { { 0, 0, 100 }, { 20, 0, 101 } }, 2.0);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { a, ed::Part::Entity, 0 } });
    const auto frame = [&] (float x, float y) {
        d.input.x = x, d.input.y = y;
        return d.Frame ().overlay.vertices;
    };
    frame (700, 160);
    const auto here = frame (700, 160), there = frame (760, 200);
    ASSERT_FALSE (here.empty ());
    ASSERT_EQ (here.size (), there.size ());
    for (size_t i = 0; i < here.size (); ++i) {
        EXPECT_FLOAT_EQ (here[i].x, there[i].x) << i;
        EXPECT_FLOAT_EQ (here[i].y, there[i].y) << i;
    }
}

TEST (HudVirtualEditor, KerbGripsRideTheirFillets)
{
    // Both paths' kerb radius 3 m: every junction grip sits -- and is drawn -- on its fillet's middle, not at the sharp
    // corner, and is found there.
    ed::Document doc;
    const ed::Id a = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 0 } }, 2.0);
    const ed::Id b = ed::AddPath (doc, { { 10, -10, 0 }, { 10, 10, 0 } }, 2.0);
    std::string error;
    ASSERT_TRUE (ed::SetPathEdgeRadius (doc, a, 3.0, error)) << error;
    ASSERT_TRUE (ed::SetPathEdgeRadius (doc, b, 3.0, error)) << error;
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { a, ed::Part::Entity, 0 } });
    const auto screen = [] (ed::Vec3 p) { return ImVec2 { 600 + 20 * float (p.x), 500 - 20 * float (p.y) }; };
    for (const auto& c : ed::Pave (d.D ().paths, 0.5, false).corners) {
        if (c.key.other == ed::kNoId)
            continue;
        ASSERT_NEAR (c.key.radius, 3.0, 1e-9);
        const ed::Vec3 at = ed::KerbGripAt (c, 0.8);
        const ImVec2 g = screen (at);
        d.At (g.x, g.y);
        EXPECT_GE (d.S ().hoverKerb, 0) << c.at.x << ", " << c.at.y;
        // Drawn there too: the live view re-projects each grip from its anchor.
        bool anchored = false;
        d.input.x = 700, d.input.y = 160;
        for (const auto& v : d.Frame ().overlay.vertices)
            anchored |= v.modelAnchored && std::hypot (v.model[0] - at.x, v.model[1] - at.y) < 1e-3;
        EXPECT_TRUE (anchored) << c.at.x << ", " << c.at.y;
    }
}

TEST (HudVirtualEditor, LabelsAreInkWithoutAPill)
{
    // A path's heights, grade and crossfall are secondary: drawn as ink, nothing under them.
    ed::Document doc;
    ed::AddPath (doc, { { 0, 0, 100 }, { 0, 12, 101 } }, 2.0);
    Drive d (std::move (doc));
    d.input.x = 700, d.input.y = 160;
    const auto drawn = d.Frame ().overlay.vertices;
    ASSERT_FALSE (drawn.empty ());
    for (const auto& v : drawn)
        EXPECT_NE (v.rgba, geomsrv::archviz::hudshell::Unpacked (IM_COL32 (247, 248, 250, 235))) << v.x << ", " << v.y;
}

TEST (HudVirtualEditor, DimensionsShowWhenCloseEnoughToRead)
{
    // At 20 px a metre, a 6 m wide path's width dimension reads and is drawn; a 1 m one's
    // would be longer than its line, so it waits for the view to come closer.
    const auto drawn = [] (double width) {
        ed::Document doc;
        ed::AddPath (doc, { { 0, -10, 0 }, { 0, 12, 0 } }, width);
        Drive d (std::move (doc));
        d.input.x = 950, d.input.y = 160;
        size_t n = 0;
        for (const auto& v : d.Frame ().overlay.vertices)
            n += v.rgba == el::Style {}.dimension;
        return n;
    };
    EXPECT_GT (drawn (6.0), 0u);
    EXPECT_EQ (drawn (1.0), 0u);
}

TEST (HudVirtualEditor, AClickedKerbGripTakesATypedRadius)
{
    // A tee's junction grip clicked (not dragged): the tracker offers its radius, and a number
    // typed then sets that corner alone.
    ed::Document doc;
    const ed::Id a = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 0 } }, 2.0);
    ed::AddPath (doc, { { 10, 0, 0 }, { 10, 10, 0 } }, 2.0);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { a, ed::Part::Entity, 0 } });
    ed::PavedCorner j;
    for (const auto& c : ed::Pave (d.D ().paths, 0.5, false).corners)
        if (c.key.other != ed::kNoId)
            j = c;
    const ed::Vec3 at = ed::KerbGripAt (j, 0.8);
    const ImVec2 g { 600 + 20 * float (at.x), 500 - 20 * float (at.y) };
    d.Press (g.x, g.y);
    d.Release (g.x, g.y);
    ASSERT_TRUE (d.S ().kerbPicked);
    d.Frame ();
    ASSERT_EQ (d.S ().tracker.Items ().size (), 1u);
    EXPECT_EQ (d.S ().tracker.Items ()[0].quantity, ed::Quantity::Radius);
    d.input.characters = u"2.5";
    d.Frame ();
    d.Key (0x0D);
    const ed::Path& pa = *ed::FindPath (d.D (), a);
    ASSERT_EQ (pa.kerbs.size (), 1u);
    EXPECT_TRUE (pa.kerbs[0].Same (j.key));
    EXPECT_DOUBLE_EQ (pa.kerbs[0].radius, 2.5);
    EXPECT_DOUBLE_EQ (pa.edgeRadius, 0.0);
}

namespace {
// The centre of the placed label that sets `quantity` of a path part (or a kerb's radius).
bool LabelAt (Drive& d, ed::Quantity quantity, bool kerb, ImVec2& at)
{
    d.input.x = 1000, d.input.y = 100;
    d.Frame ();
    for (const auto& p : d.S ().placed)
        if ((p.target.kerbPath != ed::kNoId) == kerb && (kerb || p.target.value.quantity == quantity)) {
            at = { (p.box.x0 + p.box.x1) * 0.5f, (p.box.y0 + p.box.y1) * 0.5f };
            return true;
        }
    return false;
}
} // namespace

TEST (HudVirtualEditor, AClickedLabelIsTypedOver)
{
    // A 6 m wide path's width label, clicked: open on its value, all selected; a key replaces
    // it, Backspace and keys edit it, Enter sets it.
    ed::Document doc;
    const ed::Id p = ed::AddPath (doc, { { 0, -10, 0 }, { 0, 12, 0 } }, 6.0);
    Drive d (std::move (doc));
    ImVec2 at;
    ASSERT_TRUE (LabelAt (d, ed::Quantity::Width, false, at));
    d.Press (at.x, at.y);
    d.Release (at.x, at.y);
    ASSERT_TRUE (d.S ().labelOpen);
    EXPECT_TRUE (d.S ().labelSelected);
    EXPECT_EQ (d.S ().labelText, "6.00");
    EXPECT_TRUE (d.S ().selection.Items ().empty ()); // the click was the label's
    d.input.characters = u"35";
    d.Frame ();
    EXPECT_EQ (d.S ().labelText, "35");
    d.Key (0x08);
    EXPECT_EQ (d.S ().labelText, "3");
    d.Key (0x0D);
    EXPECT_FALSE (d.S ().labelOpen);
    const auto& r = ed::FindPath (d.D (), p)->sections[0];
    EXPECT_NEAR (r.widthLeft + r.widthRight, 3.0, 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);

    // Esc leaves it as it was; a click elsewhere sets what was typed.
    ASSERT_TRUE (LabelAt (d, ed::Quantity::Width, false, at));
    d.Press (at.x, at.y);
    d.Release (at.x, at.y);
    d.input.characters = u"9";
    d.Frame ();
    d.Key (0x1B);
    EXPECT_FALSE (d.S ().labelOpen);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.Press (at.x, at.y);
    d.Release (at.x, at.y);
    d.input.characters = u"4";
    d.Frame ();
    d.Press (1000, 100);
    d.Release (1000, 100);
    const auto& w = ed::FindPath (d.D (), p)->sections[0];
    EXPECT_NEAR (w.widthLeft + w.widthRight, 4.0, 1e-9);
}

TEST (HudVirtualEditor, ALabelOpenedBeforeAnotherEditIsStale)
{
    ed::Document doc;
    const ed::Id p = ed::AddPath (doc, { { 0, -10, 0 }, { 0, 12, 0 } }, 6.0);
    Drive d (std::move (doc));
    ImVec2 at;
    ASSERT_TRUE (LabelAt (d, ed::Quantity::Width, false, at));
    d.Press (at.x, at.y);
    d.Release (at.x, at.y);
    ASSERT_TRUE (d.S ().labelOpen);
    d.input.characters = u"4";
    d.Frame ();
    // Something else edits the path while the number is typed (the panel, the menu).
    const ed::Id range = ed::FindPath (d.D (), p)->sections[0].id;
    ASSERT_TRUE (d.S ().edits.Apply (
        [p, range] (ed::Document& doc, std::string& e) { return ed::SetPathWidth (doc, p, range, 2.5, 2.5, e); }));
    d.Key (0x0D);
    EXPECT_FALSE (d.S ().labelOpen);
    EXPECT_FALSE (d.S ().message.empty ());
    const auto& r = ed::FindPath (d.D (), p)->sections[0];
    EXPECT_NEAR (r.widthLeft + r.widthRight, 5.0, 1e-9); // the other edit's, not the label's
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, ATypedGradeKeepsTheWayItFalls)
{
    // Falling 1 m over 20 m: its label says 5.0 %; 2 typed over it is a 2 % fall.
    ed::Document doc;
    const ed::Id p = ed::AddPath (doc, { { 0, -8, 1 }, { 0, 12, 0 } }, 2.0);
    Drive d (std::move (doc));
    ImVec2 at;
    bool found = false;
    d.input.x = 1000, d.input.y = 100;
    d.Frame ();
    for (const auto& l : d.S ().placed)
        if (l.target.value.ref.part == ed::Part::Break && l.target.value.quantity == ed::Quantity::Slope)
            at = { (l.box.x0 + l.box.x1) * 0.5f, (l.box.y0 + l.box.y1) * 0.5f }, found = true;
    ASSERT_TRUE (found);
    d.Press (at.x, at.y);
    d.Release (at.x, at.y);
    EXPECT_EQ (d.S ().labelText, "5.0");
    d.input.characters = u"2";
    d.Frame ();
    d.Key (0x0D);
    EXPECT_NEAR (ed::TangentGrade (ed::FindPath (d.D (), p)->profile, 0), -0.02, 1e-9);
}

TEST (HudVirtualEditor, AKerbFilletsRadiusIsTypedOverItsLabel)
{
    // A tee's junction rounded 6 m: its radius dimension, clicked and typed over, sets it.
    ed::Document doc;
    const ed::Id a = ed::AddPath (doc, { { 0, 0, 0 }, { 32, 0, 0 } }, 2.0);
    ed::AddPath (doc, { { 16, 0, 0 }, { 16, 16, 0 } }, 2.0);
    for (const auto& c : ed::Pave (doc.paths, 0.5, false).corners)
        if (c.key.other != ed::kNoId) {
            std::string error;
            ASSERT_TRUE (ed::SetKerbRadius (doc, c.path, c.key, 6.0, error)) << error;
            break;
        }
    Drive d (std::move (doc));
    ImVec2 at;
    ASSERT_TRUE (LabelAt (d, ed::Quantity::Radius, true, at));
    d.Press (at.x, at.y);
    d.Release (at.x, at.y);
    ASSERT_TRUE (d.S ().labelOpen);
    EXPECT_EQ (d.S ().labelText, "6.00");
    d.input.characters = u"4";
    d.Frame ();
    d.Key (0x0D);
    const ed::Path& pa = *ed::FindPath (d.D (), a);
    ASSERT_EQ (pa.kerbs.size (), 1u);
    EXPECT_DOUBLE_EQ (pa.kerbs[0].radius, 4.0);
}

TEST (HudVirtualEditor, ALabelKeepsWhatItCannotSetOpen)
{
    ed::Document doc;
    const ed::Id a = ed::AddPath (doc, { { 0, 0, 0 }, { 32, 0, 0 } }, 2.0);
    ed::AddPath (doc, { { 16, 0, 0 }, { 16, 16, 0 } }, 2.0);
    for (const auto& c : ed::Pave (doc.paths, 0.5, false).corners)
        if (c.key.other != ed::kNoId) {
            std::string error;
            ASSERT_TRUE (ed::SetKerbRadius (doc, c.path, c.key, 6.0, error)) << error;
            break;
        }
    Drive d (std::move (doc));
    ImVec2 at;
    ASSERT_TRUE (LabelAt (d, ed::Quantity::Radius, true, at));
    d.Press (at.x, at.y);
    d.Release (at.x, at.y);
    d.input.characters = u"7";
    d.Frame ();
    d.Key (0x0D);
    ASSERT_DOUBLE_EQ (ed::FindPath (d.D (), a)->kerbs[0].radius, 7.0);
    ASSERT_EQ (d.S ().edits.UndoDepth (), 1u);
    // The value it opens on, set again: it closes, and nothing is edited.
    ASSERT_TRUE (LabelAt (d, ed::Quantity::Radius, true, at));
    d.Press (at.x, at.y);
    d.Release (at.x, at.y);
    ASSERT_TRUE (d.S ().labelOpen);
    d.Key (0x0D);
    EXPECT_FALSE (d.S ().labelOpen);
    EXPECT_TRUE (d.S ().message.empty ());
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    d.Press (at.x, at.y);
    d.Release (at.x, at.y);
    ASSERT_TRUE (d.S ().labelOpen);

    // A negative radius: refused, not made sharp; the label stays open with the reason.
    d.input.characters = u"-2";
    d.Frame ();
    d.Key (0x0D);
    EXPECT_TRUE (d.S ().labelOpen);
    EXPECT_FALSE (d.S ().message.empty ());
    EXPECT_DOUBLE_EQ (ed::FindPath (d.D (), a)->kerbs[0].radius, 7.0);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);

    // No number at all, set by a click elsewhere: the click is spent on the refusal -- nothing
    // is selected and no marquee starts -- and the label is still open.
    d.input.characters = u".";
    d.Frame ();
    EXPECT_EQ (d.S ().labelText, "."); // the refused text was selected: the key replaced it
    d.Press (1000, 100);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    d.Release (1000, 100);
    EXPECT_TRUE (d.S ().labelOpen);
    EXPECT_TRUE (d.S ().selection.Items ().empty ());
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);

    // Corrected, it sets.
    d.input.characters = u"3";
    d.Frame ();
    d.Key (0x0D);
    EXPECT_FALSE (d.S ().labelOpen);
    EXPECT_DOUBLE_EQ (ed::FindPath (d.D (), a)->kerbs[0].radius, 3.0);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 2u);
}

TEST (HudVirtualEditor, AHoveredPathLightsItsWholeCentreline)
{
    // The pointer on a path's centreline, between its points: the whole line lights, end to
    // end -- not a dot at its middle. At its end point, the point lights with it.
    ed::Document doc;
    const ed::Id p = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 0 } }, 2.0);
    Drive d (std::move (doc));
    const auto lit = [&] (float x, float y, float& from, float& to) {
        d.At (x, y);
        from = 1e9f, to = -1e9f;
        size_t n = 0;
        for (const auto& v : d.Frame ().overlay.vertices)
            if (v.rgba == geomsrv::archviz::hudshell::Unpacked (IM_COL32 (14, 98, 201, 255)))
                from = std::min (from, v.x), to = std::max (to, v.x), ++n;
        return n;
    };
    float from = 0, to = 0;
    ASSERT_GT (lit (600 + 20 * 6.5f, 500, from, to), 0u);
    EXPECT_EQ (d.S ().selection.hover.entity, p);
    EXPECT_EQ (d.S ().selection.hover.part, ed::Part::Entity);
    EXPECT_LT (from, 605.0f); // from its start...
    EXPECT_GT (to, 995.0f);   // ...to its end
    ASSERT_GT (lit (600 + 20 * 20.0f, 500, from, to), 0u);
    EXPECT_EQ (d.S ().selection.hover.part, ed::Part::PathPoint);
    EXPECT_LT (from, 605.0f); // the line, quieter, and the point
}

TEST (HudVirtualEditor, ContoursShowTheGradingUntilHidden)
{
    // A 4 m wide path climbing 2 m: its contour lines go to the layer and their heights are
    // written along them in the view; the panel's toggle takes both away.
    ed::Document doc;
    ed::AddPath (doc, { { 0, 0, 100 }, { 20, 0, 102 } }, 4.0);
    Drive d (std::move (doc));
    d.input.x = 300, d.input.y = 160;
    const auto drawn = [&] (size_t& strokes) {
        strokes = 0;
        for (const auto& l : d.layer.polylines)
            strokes += l.rgba == el::Style {}.contour;
        size_t n = 0;
        for (const auto& v : d.Frame ().overlay.vertices)
            n += v.rgba == el::Style {}.contourText;
        return n;
    };
    size_t strokes = 0;
    EXPECT_GT (drawn (strokes), 0u);
    EXPECT_GT (strokes, 0u);
    EXPECT_DOUBLE_EQ (d.S ().contoursInterval, 0.5); // about three guides, not ten
    d.S ().contours = false;
    d.S ().publishedRevision = UINT64_MAX; // as the panel's toggle does
    d.Publish ();
    EXPECT_EQ (drawn (strokes), 0u);
    EXPECT_EQ (strokes, 0u);
}

TEST (HudVirtualEditor, TheProfileHasAGridOfRoundHeights)
{
    // A selected path's profile in the panel: thin lines at round heights, each written in
    // the canvas's left gutter, behind the grade line.
    ed::Document doc;
    const ed::Id p = ed::AddPath (doc, { { 0, 0, 110 }, { 40, 0, 112 } }, 2.0);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { p, ed::Part::Entity, 0 } });
    d.Frame ();
    const auto grid = geomsrv::archviz::hudshell::Unpacked (IM_COL32 (200, 205, 212, 255));
    const auto text = geomsrv::archviz::hudshell::Unpacked (IM_COL32 (110, 118, 130, 255));
    size_t lines = 0, written = 0;
    float gridFrom = 1e9f, textTo = -1e9f;
    for (const auto& v : d.Frame ().host.vertices) {
        if (v.rgba == grid)
            ++lines, gridFrom = std::min (gridFrom, v.x);
        if (v.rgba == text)
            ++written, textTo = std::max (textTo, v.x);
    }
    EXPECT_GE (lines, 4u * 4u); // four or more lines, a quad each
    EXPECT_GT (written, 0u);
    EXPECT_LT (textTo, gridFrom); // the heights in the gutter, clear of the lines
}

TEST (HudVirtualEditor, APathFallsToASideAndFlipsFromItsCentreline)
{
    // Right-clicked on its centreline, a path offers its fall for every range and, once it
    // falls to a side, a flip.
    ed::Document doc;
    const ed::Id p = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 1 } }, 2.0);
    Drive d (std::move (doc));
    d.RightClick (700, 500);
    ASSERT_TRUE (d.S ().menu.open);
    ASSERT_TRUE (d.Offers ("fall.left"));
    EXPECT_FALSE (d.Offers ("fall.flip")); // crowned: no side to flip
    eh::RunMenuItem (d.S (), "fall.left");
    EXPECT_EQ (ed::FindPath (d.D (), p)->sections[0].fall, ed::Fall::Left);
    EXPECT_TRUE (d.Offers ("fall.flip"));
    eh::RunMenuItem (d.S (), "fall.flip");
    EXPECT_EQ (ed::FindPath (d.D (), p)->sections[0].fall, ed::Fall::Right);
}

TEST (HudVirtualEditor, ACrossfallTypedNegativeFallsTheOtherWay)
{
    ed::Document doc;
    const ed::Id p = ed::AddPath (doc, { { 0, -10, 0 }, { 0, 12, 0 } }, 6.0);
    std::string error;
    ASSERT_TRUE (ed::SetPathFall (doc, p, ed::kNoId, ed::Fall::Left, error)) << error;
    Drive d (std::move (doc));
    d.input.x = 1000, d.input.y = 100;
    d.Frame ();
    ImVec2 at;
    bool found = false;
    for (const auto& l : d.S ().placed)
        if (l.target.value.ref.part == ed::Part::Range && l.target.value.quantity == ed::Quantity::Slope)
            at = { (l.box.x0 + l.box.x1) * 0.5f, (l.box.y0 + l.box.y1) * 0.5f }, found = true;
    ASSERT_TRUE (found);
    d.Press (at.x, at.y);
    d.Release (at.x, at.y);
    ASSERT_TRUE (d.S ().labelOpen);
    d.input.characters = u"-3";
    d.Frame ();
    d.Key (0x0D);
    const auto& r = ed::FindPath (d.D (), p)->sections[0];
    EXPECT_EQ (r.fall, ed::Fall::Right);
    EXPECT_NEAR (r.crossfall, 3.0, 1e-9);
}

// ---- the profile canvas' own labels: an elevation beside each grade point and a signed grade
// along each tangent, each bound to the exact dimension a click reads (HudVirtualEditorPanel.cpp)

namespace {
// The placed profile label for a quantity and grade point, as the canvas published it this frame.
const eh::Session::PlacedProfileLabel* ProfileLabel (const eh::Session& s, ed::Quantity quantity, ed::Id item)
{
    for (auto it = s.profilePlaced.rbegin (); it != s.profilePlaced.rend (); ++it)
        if (it->target.quantity == quantity && it->target.ref.item == item)
            return &*it;
    return nullptr;
}
ImVec2 Centre (const eh::Session::PlacedProfileLabel& l)
{
    return { (l.box.x0 + l.box.x1) * 0.5f, (l.box.y0 + l.box.y1) * 0.5f };
}
// The canvas is clickable only where it is really drawn: a viewport tall enough that the page
// does not scroll it out of the panel (the default harness can), then a frame to lay it out.
void ShowProfile (Drive& d)
{
    d.input.width = 1200.0f;
    d.input.height = 1500.0f;
    d.Frame ();
}
// A straight 100 m ramp, 100.0 to 103.0, with a break at 40 m set to 102.0 (5 % then 1 %).
ed::Document ProfileRamp (ed::Id& path, ed::Id& mid)
{
    ed::Document doc;
    path = ed::AddPath (doc, { { 0, 0, 100.0 }, { 100, 0, 103.0 } }, 1.8);
    std::string error;
    EXPECT_TRUE (ed::InsertPathBreak (doc, path, 40.0, error, &mid)) << error;
    EXPECT_TRUE (ed::SetPathElevation (doc, path, mid, 102.0, error)) << error;
    return doc;
}
} // namespace

TEST (HudVirtualEditor, AProfileElevationLabelBindsTheBreakItSitsBeside)
{
    ed::Id path = 0, mid = 0;
    Drive d (ProfileRamp (path, mid));
    ShowProfile (d);
    ASSERT_FALSE (d.S ().profilePlaced.empty ());
    size_t elevations = 0, slopes = 0;
    for (const auto& l : d.S ().profilePlaced) {
        elevations += l.target.quantity == ed::Quantity::Elevation;
        slopes += l.target.quantity == ed::Quantity::Slope;
    }
    EXPECT_EQ (elevations, 3u); // one beside each grade point
    EXPECT_EQ (slopes, 2u);     // one along each tangent
    const auto* box = ProfileLabel (d.S (), ed::Quantity::Elevation, mid);
    ASSERT_NE (box, nullptr);
    const auto revision = d.D ().revision;
    const float cx = (box->box.x0 + box->box.x1) * 0.5f, cy = (box->box.y0 + box->box.y1) * 0.5f;
    d.Press (cx, cy);
    EXPECT_EQ (d.S ().drag, eh::Drag::None); // a label click is never a drag
    ASSERT_TRUE (d.S ().profileTyping);
    EXPECT_EQ (d.S ().profileTarget.quantity, ed::Quantity::Elevation);
    EXPECT_EQ (d.S ().profileTarget.ref.entity, path);
    EXPECT_EQ (d.S ().profileTarget.ref.item, mid);
    EXPECT_EQ (d.S ().profileRevision, revision);
    const auto active = d.Frame ();
    size_t entryVertices = 0;
    for (const auto& vertex : active.host.vertices)
        entryVertices += vertex.rgba == geomsrv::archviz::hudshell::Unpacked (IM_COL32 (255, 248, 220, 255));
    EXPECT_GT (entryVertices, 0u) << "the clicked label must show an inline outlined edit field";
    ASSERT_EQ (d.S ().selection.Items ().size (), 1u);
    EXPECT_EQ (d.S ().selection.Items ()[0].part, ed::Part::Break);
    ASSERT_EQ (d.S ().tracker.Items ().size (), 1u);
    EXPECT_EQ (d.S ().tracker.Items ()[0].quantity, ed::Quantity::Elevation);

    // The exact number typed is what is written, at full precision.
    d.input.characters = u"102.375";
    d.Frame ();
    EXPECT_EQ (d.S ().tracker.State (), ed::TrackerState::Editing);
    d.Key (0x0D);
    const ed::Path* p = ed::FindPath (d.D (), path);
    ASSERT_EQ (p->profile.size (), 3u);
    EXPECT_DOUBLE_EQ (p->profile[1].z, 102.375);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    EXPECT_FALSE (d.S ().profileTyping);
    d.input.keys = { { 'Z', true, false, true } };
    d.Frame ();
    EXPECT_DOUBLE_EQ (ed::FindPath (d.D (), path)->profile[1].z, 102.0); // one undoable step
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, AProfileSlopeLabelReadsAlongItsTangentAndTakesASignedGrade)
{
    ed::Id path = 0, mid = 0;
    Drive d (ProfileRamp (path, mid));
    ShowProfile (d);
    const auto* box = ProfileLabel (d.S (), ed::Quantity::Slope, mid);
    ASSERT_NE (box, nullptr);
    // It reads along the projected run, not level: the canvas exaggerates the heights, so the
    // angle on screen is the tangent's, and the second tangent rises to the right.
    EXPECT_NE (box->box.ux, 1.0f);
    EXPECT_LT (box->box.uy, 0.0f);
    const float cx = (box->box.x0 + box->box.x1) * 0.5f, cy = (box->box.y0 + box->box.y1) * 0.5f;
    d.Press (cx, cy);
    ASSERT_TRUE (d.S ().profileTyping);
    EXPECT_EQ (d.S ().profileTarget.quantity, ed::Quantity::Slope);
    EXPECT_EQ (d.S ().profileTarget.ref.item, mid);
    d.input.characters = u"-1.25"; // signed: this tangent now falls
    d.Frame ();
    d.Key (0x0D);
    const ed::Path* p = ed::FindPath (d.D (), path);
    EXPECT_NEAR ((p->profile[2].z - p->profile[1].z) / (p->profile[2].station - p->profile[1].station) * 100.0, -1.25,
                 1e-9);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

TEST (HudVirtualEditor, AProfileElevationRefusalKeepsTheFieldAndWhatWasTyped)
{
    ed::Id path = 0, mid = 0;
    auto doc = ProfileRamp (path, mid);
    doc.paths[0].profile[1].fixed = true; // a reference elevation the edit must keep
    Drive d (std::move (doc));
    ShowProfile (d);
    const auto* box = ProfileLabel (d.S (), ed::Quantity::Elevation, mid);
    ASSERT_NE (box, nullptr);
    d.Press ((box->box.x0 + box->box.x1) * 0.5f, (box->box.y0 + box->box.y1) * 0.5f);
    ASSERT_TRUE (d.S ().profileTyping);
    const auto before = d.D ().paths;
    d.input.characters = u"103.5";
    d.Frame ();
    d.Key (0x0D);
    EXPECT_NE (d.S ().message.find ("fixed"), std::string::npos);
    EXPECT_EQ (d.S ().tracker.State (), ed::TrackerState::Editing); // the field is kept...
    EXPECT_EQ (d.S ().tracker.Text (), "103.5");                    // ...with what was typed
    EXPECT_EQ (d.D ().paths, before);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    EXPECT_TRUE (d.S ().profileTyping);
}

TEST (HudVirtualEditor, AProfileBindingRefusesWhatAChangedDocumentNoLongerMatches)
{
    ed::Id path = 0, mid = 0;
    Drive d (ProfileRamp (path, mid));
    ShowProfile (d);
    const auto* box = ProfileLabel (d.S (), ed::Quantity::Elevation, mid);
    ASSERT_NE (box, nullptr);
    d.Press ((box->box.x0 + box->box.x1) * 0.5f, (box->box.y0 + box->box.y1) * 0.5f);
    ASSERT_TRUE (d.S ().profileTyping);
    // An edit elsewhere moves the document on: the value the click read is no longer the one
    // that would be written, so Enter refuses instead of writing it.
    ASSERT_TRUE (d.S ().edits.Apply (
        [path] (ed::Document& doc, std::string& e) { return ed::SetPathEdgeRadius (doc, path, 2.0, e); }));
    d.input.characters = u"105";
    d.Frame ();
    const auto before = d.D ().paths;
    d.Key (0x0D);
    EXPECT_NE (d.S ().message.find ("changed"), std::string::npos);
    EXPECT_EQ (d.D ().paths, before);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u); // only the kerb edit
    EXPECT_TRUE (d.S ().profileTyping);        // the binding waits to be corrected
}

TEST (HudVirtualEditor, EscapeGivesUpAProfileBindingWithoutEditing)
{
    ed::Id path = 0, mid = 0;
    Drive d (ProfileRamp (path, mid));
    ShowProfile (d);
    const auto* box = ProfileLabel (d.S (), ed::Quantity::Elevation, mid);
    ASSERT_NE (box, nullptr);
    d.Press ((box->box.x0 + box->box.x1) * 0.5f, (box->box.y0 + box->box.y1) * 0.5f);
    ASSERT_TRUE (d.S ().profileTyping);
    d.input.characters = u"104";
    d.Frame ();
    const auto before = d.D ().paths;
    d.Key (0x1B);
    EXPECT_FALSE (d.S ().profileTyping);
    EXPECT_NE (d.S ().tracker.State (), ed::TrackerState::Editing);
    EXPECT_EQ (d.D ().paths, before);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, AProfileLabelCannotBindWhileAnotherEntryOwnsTheNumber)
{
    ed::Id path = 0, mid = 0;
    Drive d (ProfileRamp (path, mid));
    ShowProfile (d);
    const auto* box = ProfileLabel (d.S (), ed::Quantity::Elevation, mid);
    ASSERT_NE (box, nullptr);
    const float cx = (box->box.x0 + box->box.x1) * 0.5f, cy = (box->box.y0 + box->box.y1) * 0.5f;
    ASSERT_TRUE (eh::BeginPlaneEntry (d.S ())); // the working plane already owns the tracker
    d.Press (cx, cy);
    EXPECT_TRUE (d.S ().planeTyping);
    EXPECT_FALSE (d.S ().profileTyping);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, AProfileLabelClickIsNeverADragNorABreak)
{
    ed::Id path = 0, mid = 0;
    Drive d (ProfileRamp (path, mid));
    ShowProfile (d);
    const auto* box = ProfileLabel (d.S (), ed::Quantity::Elevation, mid);
    ASSERT_NE (box, nullptr);
    const size_t breaks = ed::FindPath (d.D (), path)->profile.size ();
    const float cx = (box->box.x0 + box->box.x1) * 0.5f, cy = (box->box.y0 + box->box.y1) * 0.5f;
    d.Press (cx, cy);
    EXPECT_TRUE (d.S ().profileTyping); // the click was the label's
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_FALSE (d.S ().edits.InGesture ());
    EXPECT_EQ (ed::FindPath (d.D (), path)->profile.size (), breaks);
}

TEST (HudVirtualEditor, ARestingProfileBindingOwnsTheInputBeforeItsFirstDigit)
{
    ed::Id path = 0, mid = 0;
    Drive d (ProfileRamp (path, mid));
    ShowProfile (d);
    const auto* box = ProfileLabel (d.S (), ed::Quantity::Elevation, mid);
    ASSERT_NE (box, nullptr);
    const ImVec2 at = Centre (*box);
    d.Press (at.x, at.y);
    ASSERT_TRUE (d.S ().profileTyping);
    EXPECT_TRUE (eh::GizmoModeLocked (d.S ())) << "mode ownership starts before the first typed digit";
    ASSERT_EQ (d.S ().tracker.State (), ed::TrackerState::Showing); // resting: no digit yet

    // A mode key does not take the view while the field rests.
    const auto gizmo = d.S ().gizmo;
    d.input.keys = { { 'R', true } };
    d.Frame ();
    d.input.keys = { { 'R', false } };
    d.Frame ();
    EXPECT_EQ (d.S ().gizmo, gizmo);
    EXPECT_TRUE (d.S ().profileTyping);

    // The drawing plane cannot take the number from the resting field.
    EXPECT_FALSE (eh::BeginPlaneEntry (d.S ()));
    EXPECT_FALSE (d.S ().planeTyping);
    EXPECT_TRUE (d.S ().profileTyping);
    EXPECT_FALSE (d.S ().message.empty ());

    // Undo waits for the value, exactly as it does for one being typed.
    const auto depth = d.S ().edits.UndoDepth ();
    d.input.keys = { { 'Z', true, false, true } };
    d.Frame ();
    d.input.keys = { { 'Z', false, false, true } };
    d.Frame ();
    EXPECT_EQ (d.S ().edits.UndoDepth (), depth);
    EXPECT_TRUE (d.S ().profileTyping);

    // The view neither picks nor drags under it: press on the path's own centreline.
    d.Press (1000, 500);
    d.Release (1000, 500);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_TRUE (d.S ().profileTyping);
}

TEST (HudVirtualEditor, AProfileLabelThePageHasScrolledAwayIsNotClickable)
{
    ed::Id path = 0, mid = 0;
    Drive d (ProfileRamp (path, mid));
    ShowProfile (d);
    // Too short a viewport for the page: the canvas is clipped out of the panel, and ImGui's own
    // hover must be what a click passes -- the published box alone is never enough.
    d.input.height = 260.0f;
    d.Frame ();
    const auto* box = ProfileLabel (d.S (), ed::Quantity::Elevation, mid);
    ASSERT_NE (box, nullptr); // still published...
    const ImVec2 at = Centre (*box);
    d.Press (at.x, at.y);
    EXPECT_FALSE (d.S ().profileTyping); // ...and not clickable where it is not drawn
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditor, AProfileBindingSurvivesAPlanToThreeDViewSwitch)
{
    ed::Id path = 0, mid = 0;
    Drive d (ProfileRamp (path, mid));
    ShowProfile (d);
    const auto* box = ProfileLabel (d.S (), ed::Quantity::Elevation, mid);
    ASSERT_NE (box, nullptr);
    const ImVec2 at = Centre (*box);
    d.Press (at.x, at.y);
    ASSERT_TRUE (d.S ().profileTyping);
    d.input.characters = u"102.5";
    d.Frame ();
    ASSERT_EQ (d.S ().tracker.State (), ed::TrackerState::Editing);
    ASSERT_EQ (d.S ().tracker.Text (), "102.5");
    const auto revision = d.S ().profileRevision;

    // The view changes plan -> 3D. The panel's field is not the view's to give up.
    d.input.planar = false;
    d.Frame ();
    EXPECT_TRUE (d.S ().profileTyping);
    EXPECT_EQ (d.S ().tracker.State (), ed::TrackerState::Editing);
    EXPECT_EQ (d.S ().tracker.Text (), "102.5");
    EXPECT_EQ (d.S ().profileTarget.ref.item, mid);
    EXPECT_EQ (d.S ().profileRevision, revision);

    d.Key (0x0D);
    EXPECT_DOUBLE_EQ (ed::FindPath (d.D (), path)->profile[1].z, 102.5);
    EXPECT_FALSE (d.S ().profileTyping);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}
