// ArchViz/GradientLibrary: the overlay's named colour ramps (#102). A wrong stop is a
// wrong colour on someone's analysis, and a renamed id is a script that stops working,
// so the table's shape, the original ten names and exact sample stops are pinned here.
// tests/test_gradient_presets.py pins every preset's first stop, last stop and count
// against GradientPresets.json.

#include "ArchViz/GradientLibrary.hpp"
#include "ArchViz/OverlayLayers.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

namespace layers = geomsrv::archviz::overlaylayers;

TEST (GradientLibrary, EveryPresetIsAValidRamp)
{
    std::set<std::string> ids;
    ASSERT_EQ (layers::GradientPresets ().size (), 84u) << "ten originals and seventy-four library presets";
    for (const layers::GradientPreset& preset : layers::GradientPresets ()) {
        EXPECT_TRUE (ids.insert (preset.id).second) << preset.id << " is listed twice";
        ASSERT_GE (preset.count, 2) << preset.id;
        ASSERT_LE (preset.count, layers::kMaxStops) << preset.id;
        EXPECT_FLOAT_EQ (preset.stops[0].at, 0.0f) << preset.id;
        EXPECT_FLOAT_EQ (preset.stops[preset.count - 1].at, 1.0f) << preset.id;
        for (size_t i = 1; i < preset.count; ++i)
            EXPECT_LE (preset.stops[i - 1].at, preset.stops[i].at) << preset.id << " stop " << i;
        EXPECT_LT (preset.defaultMin, preset.defaultMax) << preset.id;
        EXPECT_STRNE (layers::GradientCategoryLabel (preset.category), "") << preset.id;
    }
}

TEST (GradientLibrary, TheOriginalTenNamesStillResolveUnchanged)
{
    for (const char* name :
         { "viridis", "inferno", "magma", "plasma", "turbo", "coolwarm", "greys", "sunhours", "slope", "clearance" }) {
        std::vector<layers::ColourStop> stops;
        ASSERT_TRUE (layers::PresetStops (name, stops)) << name;
        ASSERT_EQ (stops.size (), 9u) << name;
        for (size_t i = 0; i < 9; ++i)
            EXPECT_FLOAT_EQ (stops[i].at, float (i) / 8.0f) << name;
    }
    std::vector<layers::ColourStop> stops;
    ASSERT_TRUE (layers::PresetStops ("sunhours", stops));
    EXPECT_EQ (stops.front ().rgba, 0x081D58FFu);
    EXPECT_EQ (stops.back ().rgba, 0xFEC44FFFu);
}

TEST (GradientLibrary, UnknownNamesFailAndLeaveTheStopsAlone)
{
    std::vector<layers::ColourStop> stops { { 0.0f, 1u }, { 1.0f, 2u } };
    EXPECT_FALSE (layers::PresetStops ("no-such-ramp", stops));
    EXPECT_FALSE (layers::PresetStops ("", stops));
    EXPECT_EQ (stops.size (), 2u);
    EXPECT_EQ (layers::FindGradientPreset ("Viridis"), nullptr) << "ids are case-sensitive";
}

TEST (GradientLibrary, ExactStopsSurviveTheTable)
{
    // Uneven positions are kept, not respaced.
    const layers::GradientPreset* warm = layers::FindGradientPreset ("sunlight-warm");
    ASSERT_NE (warm, nullptr);
    EXPECT_EQ (warm->category, layers::GradientCategory::Sun);
    EXPECT_TRUE (warm->stepped);
    EXPECT_DOUBLE_EQ (warm->defaultMax, 11.0);
    ASSERT_EQ (warm->count, 12);
    EXPECT_EQ (warm->stops[0].rgba, 0x643C28FFu);
    EXPECT_NEAR (warm->stops[1].at, 1.0f / 11.0f, 1e-6f);
    EXPECT_EQ (warm->stops[11].rgba, 0xFFF5C8FFu);

    const layers::GradientPreset* cutFill = layers::FindGradientPreset ("cut-fill");
    ASSERT_NE (cutFill, nullptr);
    EXPECT_DOUBLE_EQ (cutFill->defaultMin, -2.0);
    EXPECT_FLOAT_EQ (cutFill->stops[3].at, 0.5f);
    EXPECT_EQ (cutFill->stops[3].rgba, 0xF7F7F7FFu) << "the cut/fill zero is white at the middle";

    const layers::GradientPreset* utci = layers::FindGradientPreset ("utci");
    ASSERT_NE (utci, nullptr);
    EXPECT_DOUBLE_EQ (utci->defaultMin, -40.0);
    EXPECT_DOUBLE_EQ (utci->defaultMax, 55.0);

    // The one stop with alpha keeps it.
    const layers::GradientPreset* spaces = layers::FindGradientPreset ("space-types");
    ASSERT_NE (spaces, nullptr);
    EXPECT_EQ (spaces->stops[9].rgba, 0x40B4FF7Fu);

    const layers::GradientPreset* view = layers::FindGradientPreset ("view-study");
    ASSERT_NE (view, nullptr);
    EXPECT_EQ (view->stops[0].rgba, 0xFF1493FFu);
    EXPECT_EQ (view->stops[view->count - 1].rgba, 0x00BFFFFFu);
}

namespace {

// A heatmap's legend and the panel ramp beside it, both drawn from `preset`, with an
// explicit range and a cutoff the picker must not move; and a custom ramp it must not touch.
layers::Layer Study (const std::string& preset)
{
    layers::Layer layer;
    layer.name = "gradient-study";
    layers::Legend legend;
    EXPECT_TRUE (layers::PresetStops (preset, legend.colormap.stops));
    legend.colormap.presetId = preset;
    legend.colormap.autoRange = false;
    legend.colormap.min = 0.0;
    legend.colormap.max = 8.0;
    legend.colormap.hasCutoff = true;
    legend.colormap.cutoff = 2.5;
    legend.colormap.hasGreen = true;
    legend.colormap.green = 6.0;
    layers::Legend custom = legend;
    custom.colormap.presetId.clear ();
    custom.colormap.stops = { { 0.0f, 0x000000FFu }, { 1.0f, 0xFFFFFFFFu } };
    layer.legends = { legend, custom };
    layers::Panel panel;
    layers::PanelItem ramp;
    ramp.kind = layers::ItemKind::Ramp;
    ramp.colormap = legend.colormap;
    panel.items = { ramp };
    layer.panels = { panel };
    return layer;
}

const layers::Layer& Stored (const std::string& name)
{
    for (const auto& layer : layers::Layers ())
        if (layer->name == name)
            return *layer;
    static const layers::Layer none;
    ADD_FAILURE () << name << " is not stored";
    return none;
}

} // namespace

TEST (GradientLibrary, AColormapNamesOnlyAPresetTheLibraryHas)
{
    layers::Layer layer = Study ("sunhours");
    EXPECT_EQ (layers::Validate (layer), "");
    layer.legends[0].colormap.presetId = "no-such-ramp";
    EXPECT_NE (layers::Validate (layer).find ("no-such-ramp"), std::string::npos);
}

TEST (GradientLibrary, PickingAPresetRecoloursTheLayerAndKeepsItsRangeAndAnchors)
{
    layers::ClearAll ();
    layers::Set (Study ("sunhours"));
    ASSERT_TRUE (layers::SetRampPreset ("gradient-study", "sunhours", "sunlight-warm"));
    const layers::GradientPreset* warm = layers::FindGradientPreset ("sunlight-warm");
    for (const layers::Colormap* map :
         { &Stored ("gradient-study").legends[0].colormap, &Stored ("gradient-study").panels[0].items[0].colormap }) {
        EXPECT_EQ (map->presetId, "sunlight-warm");
        ASSERT_EQ (map->stops.size (), warm->count);
        EXPECT_EQ (map->stops.front ().rgba, warm->stops[0].rgba);
        EXPECT_FALSE (map->autoRange);
        EXPECT_DOUBLE_EQ (map->max, 8.0) << "the explicit range wins over the preset's 0..11";
        EXPECT_DOUBLE_EQ (map->cutoff, 2.5);
        EXPECT_DOUBLE_EQ (map->green, 6.0);
    }
    const layers::Colormap& custom = Stored ("gradient-study").legends[1].colormap;
    EXPECT_TRUE (custom.presetId.empty ());
    EXPECT_EQ (custom.stops.back ().rgba, 0xFFFFFFFFu) << "explicit stops are the script's own";

    EXPECT_FALSE (layers::SetRampPreset ("gradient-study", "sunlight-warm", "no-such-ramp"));
    EXPECT_FALSE (layers::SetRampPreset ("gradient-study", "viridis", "magma")) << "nothing is drawn from viridis";
    EXPECT_FALSE (layers::SetRampPreset ("no-such-layer", "sunhours", "magma"));
    layers::ClearAll ();
}

TEST (GradientLibrary, AChoiceOutlivesTheScriptRepublishingItsLayerUntilTheSessionEnds)
{
    layers::ClearAll ();
    layers::Set (Study ("sunhours"));
    ASSERT_TRUE (layers::SetRampPreset ("gradient-study", "sunhours", "sunlight"));
    ASSERT_TRUE (layers::SetRampPreset ("gradient-study", "sunlight", "sunlight-warm"));
    // The script sends its layer again, with its own preset: the user's colours stay.
    layers::Set (Study ("sunhours"));
    EXPECT_EQ (Stored ("gradient-study").legends[0].colormap.presetId, "sunlight-warm");
    EXPECT_EQ (Stored ("gradient-study").panels[0].items[0].colormap.presetId, "sunlight-warm");
    // Choosing the script's own preset again forgets the choice.
    ASSERT_TRUE (layers::SetRampPreset ("gradient-study", "sunlight-warm", "sunhours"));
    layers::Set (Study ("sunhours"));
    EXPECT_EQ (Stored ("gradient-study").legends[0].colormap.presetId, "sunhours");
    // And the end of the session forgets every choice (OVERLAY-INVARIANTS.md §8).
    ASSERT_TRUE (layers::SetRampPreset ("gradient-study", "sunhours", "magma"));
    layers::ClearAll ();
    layers::Set (Study ("sunhours"));
    EXPECT_EQ (Stored ("gradient-study").legends[0].colormap.presetId, "sunhours");
    layers::ClearAll ();
}
