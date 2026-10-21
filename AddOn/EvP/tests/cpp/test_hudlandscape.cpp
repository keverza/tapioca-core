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
