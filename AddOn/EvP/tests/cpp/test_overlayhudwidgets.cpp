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

// An engine that keeps what the user did in a state the test can read, and hears every
// change.
struct Watched : Fresh {
    std::shared_ptr<hud::State> state = hud::NewState ();
    std::vector<hud::Change> heard;
    Watched ()
    {
        engine.UseState (state);
        engine.SetChangeSink ([this] (const hud::Change& change) { heard.push_back (change); });
    }
    double Value (const std::string& id) const
    {
        for (const auto& held : hud::Values (*state, "hud#0"))
            if (held.first == id)
                return held.second;
        return -1.0;
    }
};

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
