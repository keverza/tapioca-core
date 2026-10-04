// ArchViz/OverlayHud, stage 2's controls: checkboxes, sliders, dropdowns, tabs and
// buttons, their values held by the add-on and every change said for Python. Pressed as
// the input layer hands the presses over, laid out by the vendored Dear ImGui.
//
// ⚠️ THE USER, 2026-09-29: sliders, checkboxes, tabs and dropdowns; the add-on holds their
// values and sends change events to Python.

#include "hud_fixture.hpp"

#include "ArchViz/OverlayHitMap.hpp"
#include "ArchViz/OverlayScene.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

using namespace hudtest;

namespace input = geomsrv::archviz::overlayinput;
namespace scene = geomsrv::archviz::overlayscene;

namespace {

layers::PanelItem Control (layers::ItemKind kind, const std::string& id, const std::string& text)
{
    layers::PanelItem item = Item (kind, text);
    item.id = id;
    return item;
}

// The panel's content starts a padding in from its top-left at (16, 16).
constexpr float kLeft = 16.0f + 10.0f;
constexpr float kTop = 16.0f + 10.0f;

} // namespace

// A checkbox flips on a press and says so; the add-on keeps what the user set when the
// layer is set again saying what it said before, and takes the layer's when it says
// something new.
TEST (OverlayHudWidgets, ACheckboxHoldsTheUsersValueUntilTheLayerSaysANewOne)
{
    Watched hud;
    layers::Panel panel;
    panel.items.push_back (Control (layers::ItemKind::Checkbox, "grid", "Show grid"));
    layers::Layer layer;
    layer.name = "study";
    layer.panels = { panel };
    EXPECT_EQ (layers::Validate (layer), "");
    // The box is the first item: a frame's height square.
    const float x = kLeft + 10.0f, y = kTop + 10.0f;
    hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_DOUBLE_EQ (hud.Value ("grid"), 0.0);
    EXPECT_TRUE (hud.Lay ({ &panel }, At (x, y)).hand);
    hud.Click ({ &panel }, x, y);
    ASSERT_EQ (hud.heard.size (), 1u);
    EXPECT_EQ (hud.heard[0].kind, "checkbox");
    EXPECT_EQ (hud.heard[0].id, "grid");
    EXPECT_EQ (hud.heard[0].item, 0);
    EXPECT_DOUBLE_EQ (hud.heard[0].value, 1.0);
    EXPECT_EQ (hud.heard[0].text, "on");
    EXPECT_DOUBLE_EQ (hud.Value ("grid"), 1.0);
    // Set again as before: the user's value stands.
    const layers::Panel same = panel;
    hud.Lay ({ &same }, At (600.0f, 600.0f));
    EXPECT_DOUBLE_EQ (hud.Value ("grid"), 1.0);
    // Set again saying something new: the layer's.
    layers::Panel unchecked = panel;
    unchecked.items[0].checked = true;
    hud.Lay ({ &unchecked }, At (600.0f, 600.0f));
    EXPECT_DOUBLE_EQ (hud.Value ("grid"), 1.0);
    hud.Click ({ &unchecked }, x, y);
    EXPECT_DOUBLE_EQ (hud.Value ("grid"), 0.0);
    hud.Lay ({ &unchecked }, At (600.0f, 600.0f));
    EXPECT_DOUBLE_EQ (hud.Value ("grid"), 0.0) << "checked again as before: the user's off stands";
    EXPECT_EQ (hud.heard.size (), 2u) << "a layer's value is not the user's change";
}

// A slider pressed and dragged says every value on the way, not final, and the last at
// the release, final -- each on its steps and inside its range.
TEST (OverlayHudWidgets, ASliderSaysItsValuesOnTheWayAndTheLastAtTheRelease)
{
    Watched hud;
    layers::Panel panel;
    panel.widthPixels = 300.0f;
    layers::PanelItem hour = Control (layers::ItemKind::Slider, "hour", "Hour");
    hour.min = 6.0;
    hour.max = 20.0;
    hour.autoRange = false;
    hour.number = 12.0;
    hour.step = 0.5;
    hour.unit = "h";
    panel.items.push_back (hour);
    // Its label's line, then the bar across the panel's width.
    const float y = kTop + 14.0f + 4.0f + 10.0f;
    const float from = kLeft + 280.0f * 0.3f, to = kLeft + 280.0f * 0.8f;
    hud.Lay ({ &panel }, At (from, y));
    EXPECT_DOUBLE_EQ (hud.Value ("hour"), 12.0);
    hud.Lay ({ &panel }, At (from, y, { { 0, true } }));
    hud.Lay ({ &panel }, At (to, y));
    hud.Lay ({ &panel }, At (to, y, { { 0, false } }));
    ASSERT_GE (hud.heard.size (), 3u);
    for (size_t k = 0; k + 1 < hud.heard.size (); ++k) {
        EXPECT_EQ (hud.heard[k].kind, "slider");
        EXPECT_FALSE (hud.heard[k].final);
    }
    const hud::Change& last = hud.heard.back ();
    EXPECT_TRUE (last.final);
    EXPECT_DOUBLE_EQ (last.value, hud.heard[hud.heard.size () - 2].value);
    EXPECT_GT (last.value, hud.heard.front ().value) << "dragged to the right";
    EXPECT_NEAR (hud.heard.front ().value, 6.0 + 14.0 * 0.3, 1.0);
    EXPECT_NEAR (last.value, 6.0 + 14.0 * 0.8, 1.0);
    EXPECT_DOUBLE_EQ (std::fmod (last.value - 6.0, 0.5), 0.0) << "on its steps";
    EXPECT_EQ (last.text.substr (last.text.size () - 2), " h");
    EXPECT_DOUBLE_EQ (hud.Value ("hour"), last.value);
}

// A dropdown opens a list over the view; while it is open the whole view is the HUD's, so
// the click that closes it never reaches Archicad. A choice says its index and option.
TEST (OverlayHudWidgets, ADropdownTakesTheViewWhileOpenAndSaysTheChoice)
{
    Watched hud;
    layers::Panel panel;
    panel.widthPixels = 240.0f;
    layers::PanelItem date = Control (layers::ItemKind::Combo, "date", "Date");
    date.labels = { "21 March", "21 June", "21 December" };
    panel.items.push_back (date);
    layers::Layer layer;
    layer.name = "study";
    layer.panels = { panel };
    ASSERT_EQ (layers::Validate (layer), "");
    const float y = kTop + 14.0f + 4.0f + 10.0f;
    EXPECT_FALSE (hud.Lay ({ &panel }, At (600.0f, 600.0f)).popup);
    const hud::Layout opened = hud.Click ({ &panel }, kLeft + 60.0f, y);
    ASSERT_TRUE (opened.popup);
    // The list floats over the view, in the panel's colours nearly opaque.
    float list[4] = {};
    ASSERT_TRUE (Box (opened.overlay, (panel.backgroundRgba & 0xFFFFFF00u) | 0xF6u, list));
    EXPECT_GT (list[1], y) << "under the dropdown";
    // The second option: the list's padding, one option's line and its spacing down.
    const float option = list[1] + 10.0f + 18.0f + 7.0f;
    const hud::Layout chosen = hud.Click ({ &panel }, list[0] + 40.0f, option);
    ASSERT_EQ (hud.heard.size (), 1u);
    EXPECT_EQ (hud.heard[0].kind, "combo");
    EXPECT_DOUBLE_EQ (hud.heard[0].value, 1.0);
    EXPECT_EQ (hud.heard[0].text, "21 June");
    EXPECT_FALSE (chosen.popup) << "a choice closes the list";
    EXPECT_DOUBLE_EQ (hud.Value ("date"), 1.0);
    // Open again and press the model: the list closes, nothing is chosen.
    hud.Click ({ &panel }, kLeft + 60.0f, y);
    const hud::Layout outside = hud.Click ({ &panel }, 800.0f, 500.0f);
    EXPECT_FALSE (outside.popup);
    EXPECT_EQ (hud.heard.size (), 1u);

    // Through the HUD stream: the open list is a region over the whole view, topmost.
    Watched streamed;
    const auto all =
        std::vector<std::shared_ptr<const layers::Layer>> { std::make_shared<const layers::Layer> (layer) };
    scene::PrepareSceneHud (all, &streamed.engine, 1.0f, At (kLeft + 60.0f, y));
    scene::PrepareSceneHud (all, &streamed.engine, 1.0f, At (kLeft + 60.0f, y, { { 0, true } }));
    const scene::Scene open =
        scene::PrepareSceneHud (all, &streamed.engine, 1.0f, At (kLeft + 60.0f, y, { { 0, false } }));
    ASSERT_FALSE (open.regions.empty ());
    EXPECT_EQ (open.regions.back ().kind, input::RegionKind::Popup);
    input::HitMap map;
    map.regions = open.regions;
    EXPECT_EQ (map.Hit (1100.0f, 700.0f, 1200.0f, 800.0f), int (open.regions.size ()) - 1);
}

namespace {

// Three tabs, each page a swatch of its own colour.
layers::Panel Tabbed (uint32_t selected)
{
    layers::Panel panel;
    const char* titles[] = { "Area", "Sun", "Shade" };
    const uint32_t colours[] = { 0x11AA22FFu, 0x3355CCFFu, 0xCC5533FFu };
    for (int k = 0; k < 3; ++k) {
        layers::PanelItem tab = Item (layers::ItemKind::Tab, titles[k]);
        if (k == 0) {
            tab.id = "view";
            tab.selected = selected;
        }
        panel.items.push_back (tab);
        layers::PanelItem swatch = Item (layers::ItemKind::Swatch, titles[k]);
        swatch.rgba = colours[k];
        panel.items.push_back (swatch);
    }
    return panel;
}

bool Shows (const hud::Layout& out, uint32_t rgba)
{
    float box[4] = {};
    return Box (out.panels[0], rgba, box);
}

// Where the tabs of the given kind are drawn: a tab not shown in the panel's text colour
// faintly, the shown one in the accent faintly -- as is the line under the whole bar, so
// the shown one's box is the bar's width.
bool TabBox (const hud::Layout& out, const layers::Panel& panel, bool selected, float box[4])
{
    const uint32_t text = panel.textRgba;
    const uint32_t colour = selected ? (panel.accentRgba & 0xFFFFFF00u) | uint32_t (std::lround (255.0f * 0.20f))
                                     : (text & 0xFFFFFF00u) | uint32_t (std::lround (255.0f * 0.06f));
    return Box (out.panels[0], colour, box);
}

} // namespace

// A tab shows its page and hides the others'; a press on another tab shows its page and
// says so. The layer choosing another tab -- something new -- wins; saying what it said
// before, it does not.
TEST (OverlayHudWidgets, TabsShowTheirPagesAndThePressedTabSaysSo)
{
    Watched hud;
    layers::Panel panel = Tabbed (0);
    layers::Layer layer;
    layer.name = "study";
    layer.panels = { panel };
    ASSERT_EQ (layers::Validate (layer), "");
    const hud::Layout first = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_TRUE (Shows (first, 0x11AA22FFu));
    EXPECT_FALSE (Shows (first, 0x3355CCFFu));
    float shown[4] = {}, other[4] = {};
    ASSERT_TRUE (TabBox (first, panel, true, shown));
    ASSERT_TRUE (TabBox (first, panel, false, other)) << "the tabs not shown";
    // The tabs not shown are the second and third: the second is where their box starts.
    const float x = 16.0f + other[0] + 12.0f, y = 16.0f + (other[1] + other[3]) * 0.5f;
    hud.Click ({ &panel }, x, y);
    ASSERT_EQ (hud.heard.size (), 1u);
    EXPECT_EQ (hud.heard[0].kind, "tab");
    EXPECT_EQ (hud.heard[0].id, "view");
    EXPECT_DOUBLE_EQ (hud.heard[0].value, 1.0);
    EXPECT_EQ (hud.heard[0].text, "Sun");
    const hud::Layout second = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_TRUE (Shows (second, 0x3355CCFFu));
    EXPECT_FALSE (Shows (second, 0x11AA22FFu));
    // Set again as before: the user's tab stands.
    const layers::Panel again = Tabbed (0);
    EXPECT_TRUE (Shows (hud.Lay ({ &again }, At (600.0f, 600.0f)), 0x3355CCFFu));
    // The layer chooses the third: it is shown, and that is not the user's change.
    const layers::Panel third = Tabbed (2);
    EXPECT_TRUE (Shows (hud.Lay ({ &third }, At (600.0f, 600.0f)), 0xCC5533FFu));
    EXPECT_DOUBLE_EQ (hud.Value ("view"), 2.0);
    EXPECT_EQ (hud.heard.size (), 1u);
}

// ⚠️ ONE VALUE FOR BOTH VIEWS: a tab chosen in the 3D window is the plan's page too, and
// the plan's engine -- whose ImGui still shows the old tab -- does not mistake catching up
// for a press.
TEST (OverlayHudWidgets, BothViewsShowTheTabChosenInEither)
{
    const std::shared_ptr<hud::State> state = hud::NewState ();
    Watched threeD, plan;
    threeD.engine.UseState (state);
    plan.engine.UseState (state);
    threeD.state = plan.state = state;
    const layers::Panel panel = Tabbed (0);
    EXPECT_TRUE (Shows (plan.Lay ({ &panel }, At (600.0f, 600.0f)), 0x11AA22FFu));
    const hud::Layout first = threeD.Lay ({ &panel }, At (600.0f, 600.0f));
    float other[4] = {};
    ASSERT_TRUE (TabBox (first, panel, false, other));
    threeD.Click ({ &panel }, 16.0f + other[0] + 12.0f, 16.0f + (other[1] + other[3]) * 0.5f);
    ASSERT_EQ (threeD.heard.size (), 1u);
    EXPECT_TRUE (Shows (plan.Lay ({ &panel }, At (600.0f, 600.0f)), 0x3355CCFFu));
    EXPECT_TRUE (plan.heard.empty ()) << "catching up is not a press";
    EXPECT_DOUBLE_EQ (plan.Value ("view"), 1.0);
}

// A button says each press; it holds no value.
TEST (OverlayHudWidgets, AButtonSaysEachPress)
{
    Watched hud;
    layers::Panel panel;
    panel.items.push_back (Control (layers::ItemKind::Button, "export", "Export"));
    const float x = kLeft + 20.0f, y = kTop + 10.0f;
    EXPECT_TRUE (hud.Lay ({ &panel }, At (x, y)).hand);
    hud.Click ({ &panel }, x, y);
    hud.Click ({ &panel }, x, y);
    ASSERT_EQ (hud.heard.size (), 2u);
    EXPECT_EQ (hud.heard[1].kind, "button");
    EXPECT_EQ (hud.heard[1].id, "export");
    EXPECT_DOUBLE_EQ (hud.heard[1].value, 1.0);
    EXPECT_TRUE (hud::Values (*hud.state, "hud#0").empty ());
}

TEST (OverlayHudWidgets, TheControlsSayWhatTheyGotWrong)
{
    layers::Layer layer;
    layer.name = "study";
    layers::Panel panel;
    panel.items = { Item (layers::ItemKind::Checkbox, "Show grid") };
    layer.panels = { panel };
    EXPECT_NE (layers::Validate (layer).find ("needs its id"), std::string::npos);
    layer.panels[0].items = { Control (layers::ItemKind::Checkbox, "a", "One"),
                              Control (layers::ItemKind::Button, "a", "Two") };
    EXPECT_NE (layers::Validate (layer).find ("share the id \"a\""), std::string::npos);
    layers::PanelItem slider = Control (layers::ItemKind::Slider, "s", "S");
    layer.panels[0].items = { slider };
    EXPECT_NE (layers::Validate (layer).find ("a slider needs its min and max"), std::string::npos);
    layers::PanelItem combo = Control (layers::ItemKind::Combo, "c", "C");
    layer.panels[0].items = { combo };
    EXPECT_NE (layers::Validate (layer).find ("1 to 64 options"), std::string::npos);
    combo.labels = { "one" };
    combo.selected = 1;
    layer.panels[0].items = { combo };
    EXPECT_NE (layers::Validate (layer).find ("1 to 64 options"), std::string::npos);
    layer.panels[0] = Tabbed (3);
    EXPECT_NE (layers::Validate (layer).find ("starts on one of its 3 tabs"), std::string::npos);
    layer.panels[0].items.push_back (Item (layers::ItemKind::Tab));
    EXPECT_NE (layers::Validate (layer).find ("need their text"), std::string::npos);
}

namespace {

layers::Panel ParcelPlan ()
{
    layers::Panel panel;
    panel.widthPixels = 260;
    layers::PanelItem plan = Control (layers::ItemKind::SitePlan, "site", "");
    plan.outlineXY = { 0, 0, 10, 0, 10, 10, 0, 10 };
    plan.offsetXY = { 3, 3, 7, 3, 7, 7, 3, 7 };
    plan.setbackDistances = { 3, 3, 3, 3 };
    plan.setbackModes = { 0, 0, 0, 0 };
    plan.referenceVertices = { 0, 2 };
    plan.heightPixels = 240;
    panel.items = { plan };
    return panel;
}

hud::Layout RightClick (Watched& hud, const layers::Panel& panel, float x, float y)
{
    hud.Lay ({ &panel }, At (x, y));
    hud.Lay ({ &panel }, At (x, y, { { 1, true } }));
    hud.Lay ({ &panel }, At (x, y, { { 1, false } }));
    return hud.Lay ({ &panel }, At (x, y)); // popup dimensions settle on their next frame
}

} // namespace

TEST (OverlayHudWidgets, SitePlanFitsBothRingsAndKeepsIndependentHeldValues)
{
    Watched hud;
    layers::Panel panel = ParcelPlan ();
    layers::Layer layer;
    layer.name = "parcel";
    layer.panels = { panel };
    ASSERT_EQ (layers::Validate (layer), "");
    const hud::Layout out = hud.Lay ({ &panel }, At (600, 600));
    EXPECT_TRUE (Drawn (out, 0xAA4465FFu));
    EXPECT_TRUE (Drawn (out, 0xA66226FFu));
    EXPECT_TRUE (Drawn (out, 0xDCF3FAB4u));
    EXPECT_DOUBLE_EQ (hud.Value ("site:point:0"), 1);
    EXPECT_DOUBLE_EQ (hud.Value ("site:point:1"), 0);
    EXPECT_DOUBLE_EQ (hud.Value ("site:mode:0"), 0);
    EXPECT_DOUBLE_EQ (hud.Value ("site:offset:0"), 3);
    panel.items[0].setbackModes[0] = 1;
    panel.items[0].setbackDistances[0] = 4.25;
    hud.Lay ({ &panel }, At (600, 600));
    EXPECT_DOUBLE_EQ (hud.Value ("site:mode:0"), 1);
    EXPECT_DOUBLE_EQ (hud.Value ("site:offset:0"), 4.25);
    EXPECT_DOUBLE_EQ (hud.Value ("site:offset:1"), 3);
    EXPECT_TRUE (hud.heard.empty ());
}

TEST (OverlayHudWidgets, SitePlanRightClickEdgeClaimsMenuAndReportsSelection)
{
    Watched hud;
    const layers::Panel panel = ParcelPlan ();
    // Content 240x240; square is 200x200 with 20 px padding. S02 is right.
    const float x = kLeft + 220, y = kTop + 120;
    EXPECT_TRUE (hud.Lay ({ &panel }, At (x, y)).hand);
    const hud::Layout opened = RightClick (hud, panel, x, y);
    ASSERT_TRUE (opened.popup);
    ASSERT_EQ (hud.heard.size (), 1u);
    EXPECT_EQ (hud.heard[0].id, "site:selected");
    EXPECT_DOUBLE_EQ (hud.heard[0].value, 1);
    EXPECT_DOUBLE_EQ (hud.Value ("site:targetPoint"), -1);
    EXPECT_DOUBLE_EQ (hud.Value ("site:targetEdge"), 1);
    EXPECT_FALSE (hud.Click ({ &panel }, 800, 600).popup);
}

TEST (OverlayHudWidgets, SitePlanEndpointWinsOverAdjacentEdgeAndReadOnlyHasNoEditor)
{
    Watched hud;
    layers::Panel panel = ParcelPlan ();
    const float x = kLeft + 20, y = kTop + 220;
    EXPECT_TRUE (RightClick (hud, panel, x, y).popup);
    EXPECT_DOUBLE_EQ (hud.Value ("site:targetPoint"), 0);
    EXPECT_TRUE (hud.heard.empty ()) << "endpoint does not select a segment";
    hud.Click ({ &panel }, 800, 600);
    panel.items[0].editable = false;
    RightClick (hud, panel, kLeft + 220, kTop + 20);
    // Read-only canvas may open the HUD's generic menu, but no site editor state/event.
    EXPECT_TRUE (hud.heard.empty ());
    EXPECT_DOUBLE_EQ (hud.Value ("site:targetPoint"), 0) << "read-only click did not open endpoint 2";
}

TEST (OverlayHudWidgets, SitePlanRejectsMalformedPayloads)
{
    layers::Layer layer;
    layer.name = "parcel";
    layer.panels = { ParcelPlan () };
    auto& plan = layer.panels[0].items[0];
    plan.setbackDistances.pop_back ();
    EXPECT_NE (layers::Validate (layer).find ("sitePlan"), std::string::npos);
    plan.setbackDistances.push_back (3);
    plan.referenceVertices.push_back (4);
    EXPECT_NE (layers::Validate (layer).find ("out of range"), std::string::npos);
    plan.referenceVertices.pop_back ();
    plan.outlineXY.push_back (1);
    EXPECT_NE (layers::Validate (layer).find ("sitePlan"), std::string::npos);
}

TEST (OverlayHudWidgets, SitePlanEndpointCheckboxReportsMembership)
{
    Watched hud;
    const layers::Panel panel = ParcelPlan ();
    const hud::Layout opened = RightClick (hud, panel, kLeft + 20, kTop + 220);
    float popup[4] = {};
    ASSERT_TRUE (Box (opened.overlay, (panel.backgroundRgba & 0xFFFFFF00u) | 0xF6u, popup));
    hud.Click ({ &panel }, popup[0] + 16, popup[1] + 36);
    ASSERT_EQ (hud.heard.size (), 1u);
    EXPECT_EQ (hud.heard[0].kind, "checkbox");
    EXPECT_EQ (hud.heard[0].id, "site:point:0");
    EXPECT_DOUBLE_EQ (hud.Value ("site:point:0"), 0);
    EXPECT_DOUBLE_EQ (hud.Value ("site:point:2"), 1);
}

TEST (OverlayHudWidgets, SitePlanCustomDistanceIsMouseEditableWithoutKeyboardBackend)
{
    Watched hud;
    const layers::Panel panel = ParcelPlan ();
    const hud::Layout opened = RightClick (hud, panel, kLeft + 220, kTop + 120);
    float popup[4] = {};
    ASSERT_TRUE (Box (opened.overlay, (panel.backgroundRgba & 0xFFFFFF00u) | 0xF6u, popup));
    // Header, then three radio rows. Custom is the second radio.
    hud.Click ({ &panel }, popup[0] + 16, popup[1] + 58);
    ASSERT_DOUBLE_EQ (hud.Value ("site:mode:1"), 1);
    const float x = popup[0] + 40, y = popup[1] + 100;
    hud.Lay ({ &panel }, At (x, y, { { 0, true } }));
    hud.Lay ({ &panel }, At (x + 50, y));
    hud.Lay ({ &panel }, At (x + 50, y, { { 0, false } }));
    EXPECT_GT (hud.Value ("site:offset:1"), 3);
    EXPECT_DOUBLE_EQ (hud.Value ("site:offset:0"), 3);
    EXPECT_EQ (hud.heard.back ().kind, "slider");
    EXPECT_EQ (hud.heard.back ().id, "site:offset:1");
}
