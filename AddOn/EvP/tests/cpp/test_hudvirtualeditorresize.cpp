// The virtual editor's panel when the view changes size (HudVirtualEditorPanel.cpp `Panel`, HudShell
// `PageRoom`). The drive is the boolean tests' clone, its Frame handing back the layout.
#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "Edit/EditPolyline.hpp"
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
    hud::Layout Frame ()
    {
        hud::Layout out;
        std::string error;
        EXPECT_TRUE (
            view.engine.Build ({ &layer.panels[0] }, { std::string (el::kLayer) + "#0" }, 1, input, {}, out, error))
            << error;
        bool clear = false;
        if (hud::FollowVirtualEditor (*view.state, layer, clear) && !clear)
            layers::Set (layer);
        return out;
    }
    // A few layouts at this view size: the panel's parts are measured a frame before they are placed.
    float HostHeight (float height)
    {
        input.height = height;
        hud::Layout out;
        for (int i = 0; i < 4; ++i)
            out = Frame ();
        return out.host.height;
    }
};

} // namespace

// The user, 2026-11-20: "after resize the imGUI panel should also take more height if it needs it,
// because the panel remains the same smaller size". The Inspector was sized from the room its page
// took the frame before, so the page could never grow past it.
TEST (HudVirtualEditorResize, ThePanelGrowsWithTheView)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 2, -5, 0 }, { 3, -5, 0 } }, false);
    Drive d (std::move (doc));
    const float small = d.HostHeight (600.0f);
    ASSERT_GT (small, 0.0f);
    EXPECT_LE (small, 600.0f);
    const float large = d.HostHeight (1100.0f);
    EXPECT_GT (large, small + 300.0f) << "the panel takes the room the larger view gives it";
    EXPECT_LE (large, 1100.0f);
    EXPECT_LE (d.HostHeight (600.0f), 600.0f) << "and fits again when the view shrinks";
}
