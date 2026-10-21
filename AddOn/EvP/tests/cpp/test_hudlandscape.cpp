#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "hud_fixture.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>

namespace ed = geomsrv::edit;
namespace eh = geomsrv::archviz::edithud;
namespace el = geomsrv::archviz::editlayers;
namespace hud = geomsrv::archviz::overlayhud;
namespace layers = geomsrv::archviz::overlaylayers;
namespace lg = tapioca::geometry;

namespace {
ed::Document Demo ()
{
    ed::Document doc;
    std::string error;
    EXPECT_TRUE (lg::SeedLandscapeDemo (doc, 0, error)) << error;
    return doc;
}
double Area (eh::Session& s)
{
    double area = 0;
    for (const auto& r : eh::Landscape (s).regions)
        area += r.area;
    return area;
}
void KeepAll (eh::Session& s)
{
    ASSERT_TRUE (eh::SetLandscapeMode (s, eh::LandscapeMode::Face));
    const auto faces = eh::Landscape (s).faces;
    for (const auto& face : faces)
        ASSERT_TRUE (eh::PickLandscapeFace (s, face.seed, true, true));
    ASSERT_EQ (s.selection.Items ().size (), faces.size ());
    ASSERT_TRUE (eh::KeepLandscapeFaces (s)) << s.message;
}
struct Drive {
    hudtest::Watched view;
    layers::Layer layer;
    hud::Input input = hudtest::At (0, 0);
    Drive ()
    {
        EXPECT_TRUE (hud::StartVirtualEditor (*view.state, Demo (), "landscape"));
        input.planar = true;
        input.plan[0] = 20;
        input.plan[2] = 600;
        input.plan[4] = -20;
        input.plan[5] = 500;
        EXPECT_TRUE (eh::SetLandscapeMode (S (), eh::LandscapeMode::Face));
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
    hud::Layout Frame ()
    {
        hud::Layout out;
        std::string error;
        EXPECT_TRUE (
            view.engine.Build ({ &layer.panels[0] }, { std::string (el::kLayer) + "#0" }, 1, input, {}, out, error))
            << error;
        input.buttons.clear ();
        input.keys.clear ();
        bool clear = false;
        if (hud::FollowVirtualEditor (*view.state, layer, clear) && !clear)
            layers::Set (layer);
        return out;
    }
    void At (ed::Vec3 p)
    {
        input.x = float (600 + p.x * 20);
        input.y = float (500 - p.y * 20);
        Frame ();
    }
    void Click (ed::Vec3 p)
    {
        At (p);
        input.buttons = { { 0, true } };
        Frame ();
        input.buttons = { { 0, false } };
        Frame ();
    }
};
} // namespace

TEST (HudLandscape, SixSourcesStayInOneDocumentAndCacheIsRevisionScoped)
{
    eh::Session s (Demo ());
    ASSERT_EQ (s.edits.Doc ().landscape->sources.size (), 6u);
    EXPECT_EQ (s.edits.Doc ().entities.size (), 6u);
    ASSERT_GT (eh::Landscape (s).faces.size (), 6u);
    EXPECT_EQ (s.landscapeBuilds, 1u);
    EXPECT_TRUE (s.landscape.regions.empty ()); // Hover faces are not authored regions.
    eh::Landscape (s);
    EXPECT_EQ (s.landscapeBuilds, 1u);
    ASSERT_TRUE (eh::SetLandscapeMode (s, eh::LandscapeMode::Face));
    ASSERT_TRUE (eh::PickLandscapeFace (s, s.landscape.faces.front ().seed, true));
    const auto old = s.selection.Items ().front ();
    ASSERT_EQ (old.part, ed::Part::LandscapeFace);
    EXPECT_FALSE (ed::Exists (s.edits.Doc (), old)); // Not a transformable source entity.
    ASSERT_TRUE (eh::KeepLandscapeFaces (s)) << s.message;
    EXPECT_EQ (s.selection.Items ().size (), 0u);
    EXPECT_GT (s.landscapeBuilds, 1u);
    EXPECT_NE (old.revision, s.landscapeRevision);
    EXPECT_EQ (s.edits.UndoDepth (), 1u);
    EXPECT_EQ (s.edits.Doc ().landscape->sources.size (), 6u);
    ASSERT_EQ (s.edits.Doc ().landscape->operations.size (), 1u);
    EXPECT_EQ (s.edits.Doc ().landscape->operations[0].faces.size (), 1u);
}

TEST (HudLandscape, KeepSubtractEditUndoRedoAndCodecUseTheExistingHistory)
{
    eh::Session s (Demo ());
    double faceArea = 0;
    for (const auto& face : eh::Landscape (s).faces)
        faceArea += face.area;
    EXPECT_NEAR (faceArea, 384, 1e-5) << "The raw arrangement must partition the property rectangle.";
    KeepAll (s);
    EXPECT_NEAR (Area (s), 384, 1e-5);
    const auto cutter = s.edits.Doc ().landscape->sources[2].id;
    ASSERT_TRUE (eh::BooleanLandscape (s, lg::OperationKind::Difference, cutter)) << s.message;
    const double cut = Area (s);
    EXPECT_NEAR (cut, 384 - 9 * 3.141592653589793, 0.03);
    EXPECT_EQ (s.edits.UndoDepth (), 2u);
    const auto propertyEntity = s.edits.Doc ().landscape->sources[0].entityId;
    const auto vertex = ed::Find (s.edits.Doc (), propertyEntity)->vertices[2].id;
    ed::Transform transform;
    transform.translate = { 1, 0, 0 };
    ASSERT_TRUE (s.edits.Apply (ed::TransformOperation ({ { propertyEntity, ed::Part::Vertex, vertex } }, transform)))
        << s.edits.Message ();
    EXPECT_GT (Area (s), cut + 7.9);
    EXPECT_EQ (s.edits.Doc ().landscape->sources.size (), 6u);
    ASSERT_TRUE (s.edits.Undo ());
    EXPECT_NEAR (Area (s), cut, 1e-6);
    ASSERT_TRUE (s.edits.Redo ());
    EXPECT_GT (Area (s), cut + 7.9);
    std::string definition, error;
    ASSERT_TRUE (lg::WriteLandscape (s.edits.Doc (), lg::LandscapeStorage::Virtual, definition, error)) << error;
    const auto highWater = s.edits.Doc ().nextId;
    ASSERT_TRUE (s.edits.Undo ());
    ASSERT_TRUE (eh::ReloadLandscape (s, definition)) << s.message;
    EXPECT_GE (s.edits.Doc ().nextId, highWater);
    EXPECT_GT (Area (s), cut + 7.9);
    EXPECT_EQ (s.edits.Doc ().landscape->operations.size (), 2u);
    ASSERT_TRUE (s.edits.Undo ());
    EXPECT_NEAR (Area (s), cut, 1e-6);
}

TEST (HudLandscape, FacePointerHoverClickShiftToggleAndEscapeDoNotStartADrag)
{
    Drive d;
    const auto face = eh::Landscape (d.S ()).faces.back ();
    d.At (face.seed);
    EXPECT_EQ (d.S ().selection.hover.part, ed::Part::LandscapeFace);
    EXPECT_EQ (d.S ().selection.hover.item, face.faceId);
    EXPECT_TRUE (d.S ().selection.Items ().empty ());
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    d.Click (face.seed);
    ASSERT_EQ (d.S ().selection.Items ().size (), 1u);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_FALSE (d.S ().edits.InGesture ());
    d.input.shift = true;
    d.Frame ();
    d.Click (face.seed);
    EXPECT_TRUE (d.S ().selection.Items ().empty ());
    d.input.shift = false;
    d.Frame ();
    d.Click (face.seed);
    d.input.keys = { { 0x1B, true } };
    d.Frame ();
    EXPECT_TRUE (d.S ().selection.Items ().empty ());
    EXPECT_FALSE (d.S ().close);
}

TEST (HudLandscape, DraftSourceRegistersAndUndoesWithItsGeometry)
{
    eh::Session s (Demo ());
    ASSERT_TRUE (eh::SetLandscapeMode (s, eh::LandscapeMode::DrawSource));
    s.drawing = { { -11, -7, 0 }, { -9, -7, 0 }, { -9, -5, 0 }, { -11, -7, 0 } };
    eh::detail::FinishDrawing (s);
    EXPECT_TRUE (s.message.empty ()) << s.message;
    EXPECT_EQ (s.edits.Doc ().landscape->sources.size (), 7u);
    EXPECT_EQ (s.edits.Doc ().entities.size (), 7u);
    EXPECT_EQ (s.edits.UndoDepth (), 1u);
    ASSERT_TRUE (s.edits.Undo ());
    EXPECT_EQ (s.edits.Doc ().landscape->sources.size (), 6u);
    EXPECT_EQ (s.edits.Doc ().entities.size (), 6u);
}

TEST (HudLandscape, ControlClickOnlySelectsFacesAndDoesNotRunAnOperation)
{
    Drive d;
    KeepAll (d.S ());
    const auto face = eh::Landscape (d.S ()).faces.back ();
    d.input.ctrl = true;
    d.Click (face.seed);
    EXPECT_NEAR (Area (d.S ()), 384, 0.0001);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    EXPECT_EQ (d.S ().selection.Items ().size (), 1u);
    EXPECT_EQ (d.S ().edits.Doc ().landscape->sources.size (), 6u);
}

TEST (HudLandscape, SourcePointerDragAndHistoryKeysRebuildTheKeptDefinition)
{
    Drive d;
    KeepAll (d.S ());
    ASSERT_TRUE (
        eh::BooleanLandscape (d.S (), lg::OperationKind::Difference, d.S ().edits.Doc ().landscape->sources[2].id));
    const auto area = Area (d.S ());
    ASSERT_TRUE (eh::SetLandscapeMode (d.S (), eh::LandscapeMode::Select));
    d.S ().snapEnabled = false;
    d.At ({ 12, 8, 0 });
    d.input.buttons = { { 0, true } };
    d.Frame ();
    ASSERT_EQ (d.S ().drag, eh::Drag::Part);
    d.At ({ 13, 8, 0 });
    d.input.buttons = { { 0, false } };
    d.Frame ();
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_NEAR (Area (d.S ()), area + 8, 1e-4);
    EXPECT_EQ (d.S ().edits.Doc ().landscape->sources.size (), 6u);
    d.input.keys = { { 'Z', true, false, true } };
    d.Frame ();
    EXPECT_NEAR (Area (d.S ()), area, 1e-4);
    d.input.keys = { { 'Z', false, false, true }, { 'Y', true, false, true } };
    d.Frame ();
    EXPECT_NEAR (Area (d.S ()), area + 8, 1e-4);
}

TEST (HudLandscape, QueuedSourceDragRetainsPressPositionAndPublishesOneUndoStep)
{
    Drive d;
    KeepAll (d.S ());
    ASSERT_TRUE (eh::SetLandscapeMode (d.S (), eh::LandscapeMode::Select));
    d.S ().snapEnabled = false;
    d.At ({ 12, 8, 0 });
    const auto before = d.S ().edits.Doc ();
    const auto depth = d.S ().edits.UndoDepth ();
    // The native queue can hold an entire short drag between two layouts. Its
    // latest pointer is at release; the press belongs to the original vertex.
    d.input.x = 860;
    d.input.y = 320;
    d.input.buttons = { { 0, true, true, 840, 340 }, { 0, false, true, 860, 320 } };
    d.Frame ();
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
    EXPECT_FALSE (d.S ().edits.InGesture ());
    EXPECT_EQ (d.S ().edits.UndoDepth (), depth + 1);
    EXPECT_NEAR (d.S ().edits.Doc ().entities[0].vertices[2].p.x, 13, 1e-6);
    EXPECT_NEAR (d.S ().edits.Doc ().entities[0].vertices[2].p.y, 9, 1e-6);
    EXPECT_NEAR (Area (d.S ()), 404, 1e-4);
    ASSERT_TRUE (d.S ().edits.Undo ());
    EXPECT_EQ (d.S ().edits.Doc ().entities, before.entities);
    EXPECT_EQ (d.S ().edits.Doc ().landscape, before.landscape);
    ASSERT_TRUE (d.S ().edits.Redo ());
    EXPECT_NEAR (Area (d.S ()), 404, 1e-4);
}

TEST (HudLandscape, QueuedFacePressPicksItsOwnPositionRatherThanTheReleasePosition)
{
    Drive d;
    const auto first = *lg::LandscapeFaceAt (eh::Landscape (d.S ()), { 2, 1, 0 });
    const auto last = *lg::LandscapeFaceAt (eh::Landscape (d.S ()), { 2, -1, 0 });
    d.At (first.seed);
    const float fromX = d.input.x, fromY = d.input.y;
    d.input.x = float (600 + last.seed.x * 20);
    d.input.y = float (500 - last.seed.y * 20);
    d.input.buttons = { { 0, true, true, fromX, fromY }, { 0, false, true, d.input.x, d.input.y } };
    d.Frame ();
    ASSERT_EQ (d.S ().selection.Items ().size (), 1u);
    EXPECT_EQ (d.S ().selection.Items ()[0].item, first.faceId);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    EXPECT_EQ (d.S ().drag, eh::Drag::None);
}

TEST (HudLandscape, PreviewDatumChangeRepublishesWithoutReevaluatingGeometry)
{
    eh::Session s (Demo ());
    layers::Layer layer;
    ASSERT_TRUE (eh::Follow (s, layer));
    const auto builds = s.landscapeBuilds;
    ASSERT_TRUE (eh::SetWorkingPlane (s, ed::WorldXY (5)));
    ASSERT_TRUE (eh::Follow (s, layer));
    EXPECT_EQ (s.landscapeBuilds, builds);
    EXPECT_EQ (s.publishedLandscapeZ, 5);
    EXPECT_FALSE (eh::Follow (s, layer));
}

TEST (HudLandscape, CanvasCommandsCollectTwoOperandsAndPublishOneUndoStep)
{
    for (const auto kind :
         { lg::OperationKind::Union, lg::OperationKind::Difference, lg::OperationKind::Intersection }) {
        eh::Session s (Demo ());
        const auto faces = eh::Landscape (s).faces;
        const auto a = *lg::LandscapeFaceAt (s.landscape, { 2, 1, 0 });
        const auto b = *lg::LandscapeFaceAt (s.landscape, { 2, -1, 0 });
        const auto before = s.edits.Doc ();
        ASSERT_TRUE (eh::BeginLandscapeCommand (s, kind));
        EXPECT_FALSE (eh::AdvanceLandscapeCommand (s));
        EXPECT_FALSE (s.landscapeCommand->second);
        ASSERT_TRUE (eh::PickLandscapeFace (s, a.seed, true, true));
        ASSERT_TRUE (eh::PickLandscapeFace (s, b.seed, true, true));
        ASSERT_TRUE (eh::AdvanceLandscapeCommand (s));
        EXPECT_TRUE (s.selection.Items ().empty ());
        EXPECT_EQ (s.edits.Doc ().revision, before.revision);
        EXPECT_EQ (s.edits.Doc ().nextId, before.nextId);
        EXPECT_EQ (s.edits.UndoDepth (), 0u);
        EXPECT_FALSE (eh::AdvanceLandscapeCommand (s));
        ASSERT_TRUE (eh::PickLandscapeFace (s, b.seed, true));
        ASSERT_TRUE (eh::AdvanceLandscapeCommand (s)) << s.message;
        EXPECT_FALSE (s.landscapeCommand);
        EXPECT_EQ (s.edits.UndoDepth (), 1u);
        EXPECT_EQ (s.edits.Doc ().landscape->sources, before.landscape->sources);
        EXPECT_EQ (s.edits.Doc ().entities, before.entities);
        ASSERT_EQ (s.edits.Doc ().landscape->operations.size (), 3u);
        const double area = kind == lg::OperationKind::Union        ? a.area + b.area
                            : kind == lg::OperationKind::Difference ? a.area
                                                                    : b.area;
        EXPECT_NEAR (Area (s), area, 1e-5);
        std::string definition, error;
        ASSERT_TRUE (lg::WriteLandscape (s.edits.Doc (), lg::LandscapeStorage::Virtual, definition, error));
        ASSERT_TRUE (s.edits.Undo ());
        EXPECT_EQ (s.edits.Doc ().landscape, before.landscape);
        ASSERT_TRUE (s.edits.Redo ());
        EXPECT_NEAR (Area (s), area, 1e-5);
        ASSERT_TRUE (s.edits.Undo ());
        ASSERT_TRUE (eh::ReloadLandscape (s, definition)) << s.message;
        EXPECT_NEAR (Area (s), area, 1e-5);
    }
}

TEST (HudLandscape, CommandKeysToggleCanvasPicksAdvanceAndCancelWithoutPublication)
{
    Drive d;
    ASSERT_TRUE (eh::BeginLandscapeCommand (d.S (), lg::OperationKind::Difference));
    d.Click ({ 2, 1, 0 });
    ASSERT_EQ (d.S ().selection.Items ().size (), 1u);
    d.Click ({ 2, 1, 0 });
    EXPECT_TRUE (d.S ().selection.Items ().empty ());
    d.Click ({ 2, 1, 0 });
    d.Click ({ 2, -1, 0 });
    ASSERT_EQ (d.S ().selection.Items ().size (), 2u);
    d.input.keys = { { 0x0D, true } };
    d.Frame ();
    ASSERT_TRUE (d.S ().landscapeCommand);
    EXPECT_TRUE (d.S ().landscapeCommand->second);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
    d.input.keys = { { 'P', true } };
    d.Frame ();
    EXPECT_EQ (d.S ().tool, eh::Tool::Select);
    EXPECT_TRUE (d.S ().drawing.empty ());
    d.input.keys = { { 0x1B, true } };
    d.Frame ();
    EXPECT_FALSE (d.S ().landscapeCommand);
    EXPECT_FALSE (d.S ().close);
    EXPECT_EQ (d.S ().edits.Doc ().landscape->operations.size (), 0u);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 0u);
}

TEST (HudLandscape, ChangedDocumentAndViewCancelPendingCommandOperands)
{
    eh::Session s (Demo ());
    ASSERT_TRUE (eh::BeginLandscapeCommand (s, lg::OperationKind::Union));
    ASSERT_TRUE (eh::PickLandscapeFace (s, { 2, 1, 0 }, true));
    ASSERT_TRUE (eh::AdvanceLandscapeCommand (s));
    ed::Transform t;
    t.translate = { 1, 0, 0 };
    ASSERT_TRUE (s.edits.Apply (ed::TransformOperation ({ { s.edits.Doc ().entities[0].id, ed::Part::Entity } }, t)));
    EXPECT_FALSE (eh::AdvanceLandscapeCommand (s));
    EXPECT_FALSE (s.landscapeCommand);
    EXPECT_TRUE (s.edits.Doc ().landscape->operations.empty ());
    ASSERT_TRUE (eh::BeginLandscapeCommand (s, lg::OperationKind::Union));
    eh::detail::CancelViewEdit (s, "View changed.");
    EXPECT_FALSE (s.landscapeCommand);
    EXPECT_EQ (s.edits.UndoDepth (), 1u);
}

TEST (HudLandscape, CircleSourceIsPickedOnItsDrawnArcNotItsHiddenControlChord)
{
    const auto doc = Demo ();
    const auto circle = doc.landscape->sources[2].entityId;
    ed::PickView view;
    view.toScreen = [] (ed::Vec3 p, double& x, double& y) {
        x = p.x * 20;
        y = p.y * 20;
        return true;
    };
    const double diagonal = 3 / std::sqrt (2.0);
    const auto hits = ed::Pick (doc, view, (2 + diagonal) * 20, diagonal * 20, ed::kPickEntity, 2);
    EXPECT_TRUE (std::any_of (hits.begin (), hits.end (), [=] (const auto& hit) { return hit.ref.entity == circle; }));
}

TEST (HudLandscape, StaleFaceReloadCorruptionAndOwnedInputRefuseAtomically)
{
    eh::Session s (Demo ());
    KeepAll (s);
    const auto revision = s.edits.Doc ().revision;
    EXPECT_FALSE (eh::ReloadLandscape (s, "{}"));
    EXPECT_EQ (s.edits.Doc ().revision, revision);
    ASSERT_TRUE (s.edits.Begin ());
    EXPECT_FALSE (eh::BooleanLandscape (s, lg::OperationKind::Difference, s.edits.Doc ().landscape->sources[2].id));
    s.edits.Cancel ();
    s.selection.Set ({ { 0, ed::Part::LandscapeFace, 1, revision - 1 } });
    EXPECT_FALSE (eh::KeepLandscapeFaces (s));
    EXPECT_EQ (s.edits.Doc ().revision, revision);
    EXPECT_EQ (s.edits.UndoDepth (), 1u);
}

TEST (HudLandscape, LayerUsesDashedSourcesDistinctPropertyAndHoleAwareFill)
{
    eh::Session s (Demo ());
    KeepAll (s);
    ASSERT_TRUE (eh::BooleanLandscape (s, lg::OperationKind::Difference, s.edits.Doc ().landscape->sources[2].id));
    layers::Layer layer;
    ASSERT_TRUE (eh::Follow (s, layer));
    EXPECT_TRUE (layers::Validate (layer).empty ()) << layers::Validate (layer);
    const el::Style style;
    size_t dashed = 0, property = 0;
    for (const auto& line : layer.polylines) {
        dashed += !line.dashMetres.empty ();
        property += line.rgba == style.propertyLine && line.widthPixels == style.selectedWidth;
    }
    EXPECT_EQ (dashed, 6u);
    EXPECT_EQ (property, 1u);
    ASSERT_FALSE (layer.meshes.empty ());
    double area = 0;
    for (const auto& mesh : layer.meshes)
        for (size_t i = 0; i < mesh.indices.size (); i += 3) {
            const auto a = mesh.indices[i] * 3, b = mesh.indices[i + 1] * 3, c = mesh.indices[i + 2] * 3;
            const double signedArea = ((mesh.points[b] - mesh.points[a]) * (mesh.points[c + 1] - mesh.points[a + 1]) -
                                       (mesh.points[b + 1] - mesh.points[a + 1]) * (mesh.points[c] - mesh.points[a])) /
                                      2;
            EXPECT_GT (signedArea, 0);
            area += signedArea;
            const double x = (mesh.points[a] + mesh.points[b] + mesh.points[c]) / 3 - 2;
            const double y = (mesh.points[a + 1] + mesh.points[b + 1] + mesh.points[c + 1]) / 3;
            EXPECT_GT (std::hypot (x, y), 2.99); // No triangle paints across the circle hole.
        }
    EXPECT_NEAR (area, Area (s), 0.0001);
    EXPECT_NE (style.landscapeHover, style.landscapeSelected);
}

TEST (HudLandscape, DrawnPathFinishesWithEnterOrTheMenuAsACentrelineSource)
{
    // A path is registered as a centreline whatever role the panel holds; the
    // property-line role belongs to drawn curves. Clicks stay clear of the panel.
    Drive d;
    KeepAll (d.S ());
    const auto sources = d.S ().edits.Doc ().landscape->sources.size ();
    const auto property = d.S ().edits.Doc ().landscape->propertyLineSourceId;
    d.S ().landscapeRole = lg::SourceRole::PropertyLine;
    d.input.keys = { { 'P', true } };
    d.Frame ();
    ASSERT_EQ (d.S ().tool, eh::Tool::Path);
    d.S ().snapEnabled = false;
    d.Click ({ 13, -6, 0 });
    d.Click ({ 14, 6, 0 });
    ASSERT_EQ (d.S ().drawing.size (), 2u);
    d.input.keys = { { 0x0D, true } };
    d.Frame ();
    EXPECT_TRUE (d.S ().drawing.empty ()) << d.S ().message;
    ASSERT_EQ (d.S ().edits.Doc ().paths.size (), 1u) << d.S ().message;
    const auto& registered = d.S ().edits.Doc ().landscape->sources;
    ASSERT_EQ (registered.size (), sources + 1);
    EXPECT_EQ (registered.back ().pathId, d.S ().edits.Doc ().paths[0].id);
    EXPECT_EQ (registered.back ().role, lg::SourceRole::PathCenterline);
    EXPECT_EQ (d.S ().edits.Doc ().landscape->propertyLineSourceId, property);
    // The right-click menu's Finish ends a second path the same way.
    d.Click ({ 15, -6, 0 });
    d.Click ({ 16, 6, 0 });
    ASSERT_EQ (d.S ().drawing.size (), 2u);
    d.input.buttons = { { 1, true } };
    d.Frame ();
    d.input.buttons = { { 1, false } };
    d.Frame ();
    ASSERT_TRUE (d.S ().menu.open);
    EXPECT_EQ (d.S ().menu.target, eh::MenuTarget::Drawing);
    eh::RunMenuItem (d.S (), "draw.finish");
    EXPECT_TRUE (d.S ().drawing.empty ()) << d.S ().message;
    EXPECT_EQ (d.S ().edits.Doc ().paths.size (), 2u) << d.S ().message;
    EXPECT_EQ (d.S ().edits.Doc ().landscape->sources.size (), sources + 2);
    ASSERT_TRUE (d.S ().edits.Undo ());
    EXPECT_EQ (d.S ().edits.Doc ().paths.size (), 1u);
    EXPECT_EQ (d.S ().edits.Doc ().landscape->sources.size (), sources + 1);
}
