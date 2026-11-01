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
