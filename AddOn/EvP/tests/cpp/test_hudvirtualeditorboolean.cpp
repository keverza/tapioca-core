// The virtual editor's boolean preview (HudVirtualEditorBoolean.cpp): offered for two or more closed
// outlines, previewed without touching the document, Esc cancels with nothing allocated, Enter
// applies one undo step and selects the result. The drive the large HUD test uses is cloned here
// so this file owns the boolean tests without touching the shared one.
#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "hud_fixture.hpp"
#include <gtest/gtest.h>

namespace ed = geomsrv::edit;
namespace eh = geomsrv::archviz::edithud;
namespace el = geomsrv::archviz::editlayers;
namespace hud = geomsrv::archviz::overlayhud;
namespace layers = geomsrv::archviz::overlaylayers;

namespace {

struct Drive {
    hudtest::Watched view;
    layers::Layer layer;
    hud::Input input = hudtest::At (0, 0);

    explicit Drive (ed::Document doc)
    {
        EXPECT_TRUE (hud::StartVirtualEditor (*view.state, std::move (doc), "test"));
        view.state->host.dock.edge = geomsrv::archviz::hudshell::DockEdge::Left;
        view.state->host.dock.along = 0;
        input.planar = true;
        input.plan[0] = 20, input.plan[2] = 600, input.plan[4] = -20, input.plan[5] = 500;
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
    void Frame ()
    {
        hud::Layout out;
        std::string error;
        EXPECT_TRUE (
            view.engine.Build ({ &layer.panels[0] }, { std::string (el::kLayer) + "#0" }, 1, input, {}, out, error))
            << error;
        input.keys.clear (), input.characters.clear (), input.buttons.clear ();
        bool clear = false;
        if (hud::FollowVirtualEditor (*view.state, layer, clear) && !clear)
            layers::Set (layer);
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

std::vector<ed::Vec3> Square (double x0, double y0, double x1, double y1)
{
    return { { x0, y0, 0 }, { x1, y0, 0 }, { x1, y1, 0 }, { x0, y1, 0 } };
}

} // namespace

TEST (HudVirtualEditorBoolean, OfferedOnlyForTwoOrMoreClosedOutlines)
{
    ed::Document doc;
    const ed::Id a = ed::AddPolyline (doc, Square (0, 0, 10, 10), true);
    const ed::Id b = ed::AddPolyline (doc, Square (3, 3, 7, 7), true);
    const ed::Id line = ed::AddPolyline (doc, { { 0, -2, 0 }, { 5, -2, 0 } }, false);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { a, ed::Part::Entity, 0 } });
    EXPECT_FALSE (d.Offers ("boolean.union"));
    d.S ().selection.Set ({ { a, ed::Part::Entity, 0 }, { line, ed::Part::Entity, 0 } });
    EXPECT_FALSE (d.Offers ("boolean.union"));
    d.S ().selection.Set ({ { a, ed::Part::Entity, 0 }, { b, ed::Part::Entity, 0 } });
    for (const char* id : { "boolean.union", "boolean.subtract", "boolean.intersect", "boolean.exclude" })
        EXPECT_TRUE (d.Offers (id)) << id;
}

TEST (HudVirtualEditorBoolean, PreviewEscapesWithNothingChangedAndEnterAppliesOnce)
{
    ed::Document doc;
    const ed::Id a = ed::AddPolyline (doc, Square (0, 0, 10, 10), true, "lawn");
    const ed::Id b = ed::AddPolyline (doc, Square (3, 3, 7, 7), true);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { a, ed::Part::Entity, 0 }, { b, ed::Part::Entity, 0 } });
    const auto before = d.D ().entities;
    const ed::Id nextId = d.D ().nextId;
    const uint64_t revision = d.D ().revision;

    eh::RunMenuItem (d.S (), "boolean.subtract");
    ASSERT_TRUE (d.S ().booleanDraft.has_value ()) << d.S ().message;
    EXPECT_TRUE (d.S ().booleanDraft->error.empty ()) << d.S ().booleanDraft->error;
    EXPECT_EQ (d.S ().booleanDraft->rings.size (), 2u); // the outline and its new hole
    d.Frame ();
    EXPECT_EQ (d.D ().entities, before); // the preview is computed on a copy
    EXPECT_EQ (d.D ().nextId, nextId);
    EXPECT_EQ (d.D ().revision, revision);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);

    d.Key (0x1B); // Esc: the draft goes, the selection and the document stay
    EXPECT_FALSE (d.S ().booleanDraft.has_value ());
    EXPECT_EQ (d.D ().entities, before);
    EXPECT_EQ (d.D ().nextId, nextId); // no id was spent on the cancelled preview
    EXPECT_EQ (d.S ().selection.Items ().size (), 2u);

    eh::RunMenuItem (d.S (), "boolean.subtract");
    ASSERT_TRUE (d.S ().booleanDraft.has_value ());
    d.Key (0x0D); // Enter applies
    EXPECT_FALSE (d.S ().booleanDraft.has_value ());
    ASSERT_EQ (d.D ().entities.size (), 1u);
    EXPECT_EQ (d.D ().entities.front ().id, a);
    EXPECT_EQ (d.D ().entities.front ().role, "lawn");
    EXPECT_TRUE (ed::IsCompound (d.D ().entities.front ()));
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    ASSERT_EQ (d.S ().selection.Items ().size (), 1u);
    EXPECT_EQ (d.S ().selection.Items ().front ().entity, a);

    ASSERT_TRUE (d.S ().edits.Undo ());
    EXPECT_EQ (d.D ().entities, before);
}

TEST (HudVirtualEditorBoolean, ARefusalIsShownAndAStaleDraftCloses)
{
    ed::Document doc;
    const ed::Id a = ed::AddPolyline (doc, Square (0, 0, 2, 2), true);
    const ed::Id b = ed::AddPolyline (doc, Square (5, 5, 7, 7), true);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { a, ed::Part::Entity, 0 }, { b, ed::Part::Entity, 0 } });
    const auto before = d.D ().entities;
    eh::RunMenuItem (d.S (), "boolean.intersect");
    ASSERT_TRUE (d.S ().booleanDraft.has_value ());
    EXPECT_NE (d.S ().booleanDraft->error.find ("empty"), std::string::npos);
    EXPECT_TRUE (d.S ().booleanDraft->rings.empty ());
    EXPECT_FALSE (eh::detail::ApplyBoolean (d.S ())); // refused: the draft stays, nothing changes
    EXPECT_EQ (d.D ().entities, before);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);

    d.S ().selection.Set ({ { a, ed::Part::Entity, 0 } }); // the selection moved on
    EXPECT_FALSE (eh::detail::CheckBooleanDraft (d.S ()));
    EXPECT_FALSE (d.S ().booleanDraft.has_value ());
}

namespace {
// The editor as the user opens it (#158): OpenVirtualEditorWorkflow seeds a landscape, and
// shapes drawn with the default role are registered as Reference-line sources by the drawing
// code itself, not seeded as bare entities.
struct WorkflowDrive {
    hudtest::Watched view;
    layers::Layer layer;
    hud::Input input = hudtest::At (0, 0);

    WorkflowDrive ()
    {
        EXPECT_TRUE (hud::OpenVirtualEditorWorkflow (*view.state));
        view.state->host.dock.edge = geomsrv::archviz::hudshell::DockEdge::Left;
        view.state->host.dock.along = 0;
        input.planar = true;
        input.plan[0] = 20, input.plan[2] = 600, input.plan[4] = -20, input.plan[5] = 500;
        // Opening already published the layer into the store; this drive keeps its own copy.
        S ().publishedRevision = UINT64_MAX;
        EXPECT_TRUE (eh::Follow (S (), layer));
        layers::Set (layer);
    }
    ~WorkflowDrive ()
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
    void Frame ()
    {
        hud::Layout out;
        std::string error;
        EXPECT_TRUE (
            view.engine.Build ({ &layer.panels[0] }, { std::string (el::kLayer) + "#0" }, 1, input, {}, out, error))
            << error;
        input.keys.clear (), input.characters.clear (), input.buttons.clear ();
        bool clear = false;
        if (hud::FollowVirtualEditor (*view.state, layer, clear) && !clear)
            layers::Set (layer);
    }
    void Key (uint32_t code)
    {
        input.keys = { { code, true } };
        Frame ();
        input.keys = { { code, false } };
        Frame ();
    }
    // A click at model (x, y): the plan view shows it at (600 + 20 x, 500 - 20 y).
    void Click (double x, double y)
    {
        input.x = 600 + 20 * float (x), input.y = 500 - 20 * float (y);
        Frame ();
        input.buttons = { { 0, true } };
        Frame ();
        input.buttons = { { 0, false } };
        Frame ();
    }
    ed::Id Rectangle (double x0, double y0, double x1, double y1)
    {
        Click (x0, y0);
        Click (x1, y1);
        return D ().entities.empty () ? ed::kNoId : D ().entities.back ().id;
    }
    const tapioca::geometry::LandscapeSource* Source (ed::Id entity)
    {
        for (const auto& s : D ().landscape->sources)
            if (!s.deleted && s.entityId == entity)
                return &s;
        return nullptr;
    }
};
} // namespace

TEST (HudVirtualEditorBoolean, DefaultReferenceLinesDrawnInTheNormalWorkflowSubtractIntoAHole)
{
    WorkflowDrive d;
    ASSERT_TRUE (d.D ().landscape.has_value ());
    ASSERT_EQ (d.S ().landscapeRole, tapioca::geometry::SourceRole::ReferenceLine);
    d.S ().snapEnabled = false;
    d.Key ('Q');
    ASSERT_EQ (d.S ().tool, eh::Tool::Rectangle);
    const ed::Id outer = d.Rectangle (0, 0, 10, 10);
    const ed::Id inner = d.Rectangle (3, 3, 7, 7);
    ASSERT_EQ (d.D ().entities.size (), 2u) << d.S ().message;
    ASSERT_NE (d.Source (outer), nullptr);
    ASSERT_NE (d.Source (inner), nullptr);
    EXPECT_EQ (d.Source (outer)->role, tapioca::geometry::SourceRole::ReferenceLine);
    const ed::Id innerSource = d.Source (inner)->id;
    ASSERT_EQ (d.S ().edits.UndoDepth (), 2u);

    d.Key ('V');
    d.S ().selection.Set ({ { outer, ed::Part::Entity, 0 }, { inner, ed::Part::Entity, 0 } });
    const ed::Document drawn = d.D ();
    eh::RunMenuItem (d.S (), "boolean.subtract");
    ASSERT_TRUE (d.S ().booleanDraft.has_value ()) << d.S ().message;
    EXPECT_TRUE (d.S ().booleanDraft->error.empty ()) << d.S ().booleanDraft->error;
    EXPECT_EQ (d.S ().booleanDraft->rings.size (), 2u);
    d.Key (0x1B); // the preview cancels with no document, allocator or history change
    EXPECT_FALSE (d.S ().booleanDraft.has_value ());
    EXPECT_EQ (d.D ().entities, drawn.entities);
    EXPECT_EQ (d.D ().landscape, drawn.landscape);
    EXPECT_EQ (d.D ().nextId, drawn.nextId);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 2u);

    eh::RunMenuItem (d.S (), "boolean.subtract");
    ASSERT_TRUE (d.S ().booleanDraft.has_value ());
    d.Key (0x0D);
    EXPECT_FALSE (d.S ().booleanDraft.has_value ()) << d.S ().message;
    ASSERT_EQ (d.D ().entities.size (), 1u);
    EXPECT_EQ (d.D ().entities.front ().id, outer);
    EXPECT_TRUE (ed::IsCompound (d.D ().entities.front ()));
    ASSERT_NE (d.Source (outer), nullptr);
    EXPECT_EQ (d.Source (outer)->role, tapioca::geometry::SourceRole::ReferenceLine);
    bool tombstoned = false;
    for (const auto& s : d.D ().landscape->sources)
        tombstoned |= s.id == innerSource && s.deleted;
    EXPECT_TRUE (tombstoned);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 3u);
    d.Frame (); // the published layer draws the compound reference line
    std::string error;
    EXPECT_TRUE (ed::Validate (d.D (), error)) << error;

    ASSERT_TRUE (d.S ().edits.Undo ());
    EXPECT_EQ (d.D ().entities, drawn.entities);
    EXPECT_EQ (d.D ().landscape->sources, drawn.landscape->sources);
    ASSERT_TRUE (d.S ().edits.Redo ());
    EXPECT_TRUE (ed::IsCompound (d.D ().entities.front ()));
}
