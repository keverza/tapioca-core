// tapioca-dev #130: the virtual editor's polar array draft (HudVirtualEditorArray.cpp): started from
// the menu about the gumball pivot, previewed without touching the document, applied as one undo
// step whose copies are where the preview drew them. The drive is the boolean tests' clone.
#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "Edit/EditArrange.hpp"
#include "Edit/EditPolyline.hpp"
#include "hud_fixture.hpp"
#include <gtest/gtest.h>
#include <cmath>

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
};

void Near (ed::Vec3 a, ed::Vec3 b, size_t i)
{
    EXPECT_NEAR (a.x, b.x, 1e-9) << i;
    EXPECT_NEAR (a.y, b.y, 1e-9) << i;
    EXPECT_NEAR (a.z, b.z, 1e-9) << i;
}

} // namespace

TEST (HudVirtualEditorPolar, ThePreviewIsWhatApplyMakesAboutTheGumballPivot130)
{
    ed::Document doc;
    const ed::Id source = ed::AddPolyline (doc, { { 10, 0, 1 }, { 12, 0, 1 } }, false, "bench");
    Drive d (std::move (doc));
    const std::vector<ed::Ref> whole { { source, ed::Part::Entity, ed::kNoId } };
    d.S ().selection.Set (whole);
    d.S ().pivot = ed::Vec3 { 0, 0, 0 }; // the gumball centre, repositioned onto the origin
    d.S ().pivotFor = d.S ().selection.Items ();
    ASSERT_TRUE (d.Offers ("array.polar"));
    eh::RunMenuItem (d.S (), "array.polar");
    ASSERT_TRUE (d.S ().rectangularArray.has_value ()) << d.S ().message;
    auto& draft = *d.S ().rectangularArray;
    EXPECT_TRUE (draft.polar);
    draft.count = 4;
    draft.sweepDegrees = 360.0;
    eh::detail::UpdateRectangularArrayPreview (d.S ());
    EXPECT_DOUBLE_EQ (d.S ().rectangularArray->centre.x, 0.0);
    EXPECT_DOUBLE_EQ (d.S ().rectangularArray->centre.y, 0.0);

    const auto before = d.D ().entities;
    const ed::Id nextId = d.D ().nextId;
    d.Frame ();
    ASSERT_TRUE (d.S ().rectangularArray.has_value ()) << d.S ().message;
    const auto preview = d.S ().rectangularArray->lines;
    ASSERT_EQ (preview.size (), 3u) << "three copies, none back on the source at 360";
    const uint64_t builds = d.S ().rectangularArray->builds;
    d.Frame ();
    EXPECT_EQ (d.S ().rectangularArray->builds, builds) << "a frame alone does not rebuild the preview";
    EXPECT_EQ (d.D ().entities, before);
    EXPECT_EQ (d.D ().nextId, nextId);

    ASSERT_TRUE (eh::detail::ApplyRectangularArray (d.S ())) << d.S ().message;
    ASSERT_EQ (d.D ().entities.size (), 4u);
    EXPECT_EQ (d.S ().edits.UndoDepth (), 1u);
    EXPECT_FALSE (d.S ().rectangularArray.has_value ());
    ASSERT_EQ (d.S ().selection.Items ().size (), 4u);
    for (size_t i = 0; i < preview.size (); ++i) {
        const auto& copy = d.D ().entities[i + 1];
        Near (copy.vertices[0].p, preview[i].first, i);
        Near (copy.vertices[1].p, preview[i].second, i);
        const double a = 3.14159265358979323846 / 2.0 * double (i + 1);
        Near (copy.vertices[0].p, { 10 * std::cos (a), 10 * std::sin (a), 1 }, i);
    }
    eh::RunMenuItem (d.S (), "undo");
    EXPECT_EQ (d.D ().entities, before);
}

TEST (HudVirtualEditorPolar, ATypedCentreAndUnrotatedCopiesAndTheKindSwitch130)
{
    ed::Document doc;
    const ed::Id source = ed::AddPolyline (doc, ed::Rectangle ({ 9, -1, 0 }, { 11, 1, 0 }), true);
    Drive d (std::move (doc));
    d.S ().selection.Set ({ { source, ed::Part::Entity, ed::kNoId } });
    ASSERT_TRUE (eh::detail::BeginPolarArray (d.S ())) << d.S ().message;
    auto& draft = *d.S ().rectangularArray;
    draft.centreAtPivot = false;
    draft.centre = { 0, 0, 0 };
    draft.count = 3;
    draft.sweepDegrees = 180.0;
    draft.rotate = false;
    eh::detail::UpdateRectangularArrayPreview (d.S ());
    EXPECT_TRUE (d.S ().rectangularArray->previewError.empty ()) << d.S ().rectangularArray->previewError;
    // An unrotated square stays axis-aligned: every preview segment is horizontal or vertical.
    for (const auto& [a, b] : d.S ().rectangularArray->lines)
        EXPECT_TRUE (std::abs (a.x - b.x) < 1e-9 || std::abs (a.y - b.y) < 1e-9);

    // An out-of-range angle is said before Apply, and Apply refuses with the document untouched.
    d.S ().rectangularArray->sweepDegrees = 400.0;
    eh::detail::UpdateRectangularArrayPreview (d.S ());
    EXPECT_FALSE (d.S ().rectangularArray->previewError.empty ());
    const auto before = d.D ().entities;
    EXPECT_FALSE (eh::detail::ApplyRectangularArray (d.S ()));
    EXPECT_EQ (d.D ().entities, before);
    ASSERT_TRUE (d.S ().rectangularArray.has_value ()) << "kept for correction";
    d.S ().rectangularArray->sweepDegrees = 180.0;
    ASSERT_TRUE (eh::detail::ApplyRectangularArray (d.S ())) << d.S ().message;
    ASSERT_EQ (d.D ().entities.size (), 3u);
    double cx = 0, cy = 0;
    for (const auto& v : d.D ().entities[2].vertices)
        cx += v.p.x / 4.0, cy += v.p.y / 4.0;
    EXPECT_NEAR (cx, -10.0, 1e-9) << "the last copy at the half turn's end";
    EXPECT_NEAR (cy, 0.0, 1e-9);

    // Asking for the other kind switches the open draft rather than starting a second one.
    d.S ().selection.Set ({ { source, ed::Part::Entity, ed::kNoId } });
    ASSERT_TRUE (eh::detail::BeginPolarArray (d.S ()));
    ASSERT_TRUE (eh::detail::BeginRectangularArray (d.S ()));
    EXPECT_FALSE (d.S ().rectangularArray->polar);
}
