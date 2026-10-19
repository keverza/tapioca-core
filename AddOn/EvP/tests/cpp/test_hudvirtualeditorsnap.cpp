// The virtual editor's direct-drag snap: the item keeps the exact press position it was grabbed
// by, and only a target the pointer is close to -- on screen, in front of the camera, near in 3D
// -- can move it. The drive the large HUD test uses is cloned here so this file can own the snap
// tests without touching the shared one.
#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "Edit/EditShape.hpp"
#include "hud_fixture.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace ed = geomsrv::edit;
namespace eh = geomsrv::archviz::edithud;
namespace el = geomsrv::archviz::editlayers;
namespace hud = geomsrv::archviz::overlayhud;
namespace layers = geomsrv::archviz::overlaylayers;

namespace {

// The locked view with the supplied mapping and host geometry. `planar` uses model (x, y) at
// (600 + 20 x, 500 - 20 y); otherwise a small perspective the editor inverts through the supplied
// projection only (no camera of its own).
struct Drive {
    hudtest::Watched view;
    layers::Layer layer;
    hud::Input input;

    explicit Drive (ed::Document doc, ed::SnapContext context = {}, bool planar = true) : input (hudtest::At (0, 0))
    {
        EXPECT_TRUE (hud::StartVirtualEditor (*view.state, std::move (doc), "test", std::move (context)));
        input.planar = planar;
        if (planar) {
            input.plan[0] = 20, input.plan[2] = 600, input.plan[4] = -20, input.plan[5] = 500;
        }
        else {
            input.project = [] (double x, double y, double z, float& px, float& py) {
                const double w = 1.0 + 0.001 * z;
                if (w <= 0.0)
                    return false;
                px = float (600.0 + 20.0 * x / w);
                py = float (500.0 - 20.0 * y / w);
                return true;
            };
        }
        S ().gizmo = ed::GizmoMode::Move;
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
};

ed::Document Box (double x, double y, double w = 4, double h = 2)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { x, y, 0 }, { x + w, y, 0 }, { x + w, y + h, 0 }, { x, y + h, 0 } }, true);
    return doc;
}
} // namespace

// The corner (4, 2) is at (680, 460). Pressed 6 px right and 4 px down of it, the item must move by
// the pointer's travel alone -- the captured press offset is preserved, so there is no first-frame
// jump onto the grabbed corner.
TEST (HudVirtualEditorSnap, DirectDragNearACornerKeepsTheCapturedPressPoint)
{
    Drive d (Box (0, 0));
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    d.S ().snapEnabled = false; // isolate the base-point rule from any snap
    d.S ().edgeSnap = false;
    d.Press (686, 464);
    ASSERT_EQ (d.S ().drag, eh::Drag::Part);
    d.At (726, 464); // 40 px right: 2 m
    const auto& v = d.D ().entities[0].vertices;
    EXPECT_NEAR (v[2].p.x, 6.0, 1e-6); // the corner (4, 2) went to (6, 2)
    EXPECT_NEAR (v[2].p.y, 2.0, 1e-6);
    EXPECT_NEAR (v[0].p.x, 2.0, 1e-6); // the opposite corner (0, 0) went to (2, 0)
    EXPECT_NEAR (v[0].p.y, 0.0, 1e-6);
    d.Release (726, 464);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
}

// A press exactly on the corner still puts it exactly on the target: the captured base point is
// the corner itself, so its own travel is the item's travel.
TEST (HudVirtualEditorSnap, DirectDragExactlyOnACornerPutsItOnASnapTarget)
{
    ed::Document doc = Box (0, 0);
    ed::AddPolyline (doc, { { 15, 6, 0 }, { 15, 10, 0 } }, false); // an endpoint to snap to
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    d.Press (680, 460); // exactly the corner (4, 2)
    d.At (900, 380);    // near (15, 6)
    const auto& v = d.D ().entities[0].vertices;
    EXPECT_NEAR (v[2].p.x, 15.0, 1e-6);
    EXPECT_NEAR (v[2].p.y, 6.0, 1e-6);
    d.Release (900, 380);
}

// A host point at the same plan position but far up cannot snap: it projects onto the pointer, so
// only its world point (its distance from the gesture) can rule it out.
TEST (HudVirtualEditorSnap, FarDepthTargetDoesNotSnap)
{
    ed::SnapContext ctx;
    ctx.points.push_back ({ { 0, 0, 50 }, "model" });
    Drive d (Box (20, 0, 16, 10), std::move (ctx), false);
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    d.S ().snapModes = ed::kSnapEndpoint; // isolate the endpoint rule from any guide through the anchor
    d.Press (1080, 500);                  // the box's bottom edge, clear of the gizmo
    ASSERT_EQ (d.S ().drag, eh::Drag::Part);
    d.At (600, 500); // over world (0, 0): the host point is exactly here in plan
    EXPECT_FALSE (d.S ().snap.snapped);
    d.Release (600, 500);
}

// A host point near in 3D whose view mapping fails (behind the camera) cannot snap either.
TEST (HudVirtualEditorSnap, UnmappableTargetDoesNotSnap)
{
    ed::SnapContext ctx;
    ctx.points.push_back ({ { -0.1, 0, 0 }, "model" }); // 0.1 m from the drop point
    Drive d (Box (20, 0, 16, 10), std::move (ctx), false);
    d.input.project = [] (double x, double y, double, float& px, float& py) {
        if (x < 0.0)
            return false; // cannot be shown
        px = float (600.0 + 20.0 * x);
        py = float (500.0 - 20.0 * y);
        return true;
    };
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    d.S ().snapModes = ed::kSnapEndpoint;
    d.Press (1080, 500);
    ASSERT_EQ (d.S ().drag, eh::Drag::Part);
    d.At (600, 500);
    EXPECT_FALSE (d.S ().snap.snapped);
    d.Release (600, 500);
}

// The control: the same drag with a target that is on screen and in reach snaps to that endpoint.
TEST (HudVirtualEditorSnap, NearOnScreenTargetSnaps)
{
    ed::SnapContext ctx;
    ctx.points.push_back ({ { 0, 0, 0 }, "model" });
    Drive d (Box (20, 0, 16, 10), std::move (ctx), false);
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    d.S ().snapModes = ed::kSnapEndpoint;
    d.Press (1080, 500);
    ASSERT_EQ (d.S ().drag, eh::Drag::Part);
    d.At (600, 500);
    ASSERT_TRUE (d.S ().snap.snapped);
    EXPECT_EQ (d.S ().snap.kind, ed::SnapKind::Endpoint);
    EXPECT_EQ (d.S ().snap.contextSource, "model");
    d.Release (600, 500);
}

// The flush follows the edge the pointer holds: grabbed near the top edge, the move pulls that
// edge flush onto a parallel target near the pointer.
TEST (HudVirtualEditorSnap, DirectDragFlushFollowsTheGrabbedEdgeNearThePointer)
{
    ed::Document doc = Box (0, 0, 10, 10);
    ed::AddPolyline (doc, { { -5, 14.3, 0 }, { 15, 14.3, 0 } }, false); // a target above the box
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    d.S ().snapModes = eh::kSnapPoints | eh::kSnapEdges; // no guide through the anchor
    d.S ().edgeSnap = true;
    d.Press (640, 320); // world (2, 9): 1 m below the top edge, clear of the gizmo
    ASSERT_EQ (d.S ().drag, eh::Drag::Part);
    d.At (640, 240); // world (2, 13): the top edge would sit 0.3 below the target
    const auto& v = d.D ().entities[0].vertices;
    EXPECT_NEAR (v[2].p.x, 10.0, 1e-6);
    EXPECT_NEAR (v[2].p.y, 14.3, 1e-6); // pulled flush: 14 -> 14.3
    d.Release (640, 240);
}

// A moving edge the pointer is not near never pulls the item flush, even when its gap to a
// parallel target is within tolerance: the new contract rejects a remote-edge flush.
TEST (HudVirtualEditorSnap, DirectDragFlushIgnoresARemoteMovingEdge)
{
    ed::Document doc = Box (0, 0, 10, 10);
    ed::AddPolyline (doc, { { -5, 11.0, 0 }, { 15, 11.0, 0 } }, false); // 0.4 below the moved top edge
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { d.D ().entities[0].id, ed::Part::Entity, 0 } });
    d.S ().snapModes = eh::kSnapPoints | eh::kSnapEdges;
    d.S ().edgeSnap = true;
    d.Press (640, 480); // world (2, 1): 1 m above the bottom edge, clear of the gizmo
    ASSERT_EQ (d.S ().drag, eh::Drag::Part);
    d.At (640, 452);                    // world (2, 2.4): the top edge would sit 0.4 above the target
    EXPECT_FALSE (d.S ().snap.snapped); // the remote top edge is not pointer-local
    const auto& v = d.D ().entities[0].vertices;
    EXPECT_NEAR (v[2].p.y, 11.4, 1e-6); // it did not jump down to 11.0
    d.Release (640, 452);
}
