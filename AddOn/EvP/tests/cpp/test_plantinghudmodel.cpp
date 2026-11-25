// The Planting tab's model (#278 Stage 1C): seasons, layers, classes and the
// stepped tint's bands and stops.

#include "ArchViz/PlantingHudModel.hpp"
#include "ArchViz/SunStudyPreview.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::archviz;
using namespace geomsrv::archviz::planting;

TEST (PlantingHudModel, SeasonsAreTheProposedVilniusDefaults)
{
    const SeasonDates custom;
    const SeasonDates active = DatesOf (kActiveGrowth, custom);
    EXPECT_EQ (active.fromMonth, 4);
    EXPECT_EQ (active.fromDay, 28);
    EXPECT_EQ (active.toMonth, 10);
    EXPECT_EQ (active.toDay, 1);
    EXPECT_EQ (SeasonDays (active, 2026), 157);
    EXPECT_TRUE (DatesOf (kMidsummer, custom).singleDay);
    EXPECT_EQ (SeasonDays (DatesOf (kMidsummer, custom), 2026), 1);
    EXPECT_TRUE (DatesOf (kYear, custom).wholeYear);
    EXPECT_EQ (SeasonDays (DatesOf (kYear, custom), 2024), 366);
    EXPECT_EQ (SeasonDays (DatesOf (kExtended, custom), 2026), 202);
    for (int season = 0; season < kSeasonCount; ++season) {
        EXPECT_NE (SeasonName (season), nullptr);
        EXPECT_GT (std::string (SeasonTip (season)).size (), 5u);
    }
}

TEST (PlantingHudModel, ACustomRangeEndingBeforeItStartsHasNoDays)
{
    SeasonDates custom { 9, 1, 5, 1 };
    EXPECT_EQ (SeasonDays (DatesOf (kCustom, custom), 2026), 0);
    custom = { 7, 4, 7, 4 };
    EXPECT_TRUE (DatesOf (kCustom, custom).singleDay);
}

TEST (PlantingHudModel, ChosenHeightsAreTickedSortedAndUnique)
{
    PlantingHud hud;
    EXPECT_EQ (ChosenHeights (hud), (std::vector<float> { 0.15f, 1.5f })) << "ground cover and shrubs by default";
    hud.heightOn[3] = true;
    hud.heights[3] = 0.15f; // a duplicate of slot 0
    hud.heightOn[1] = true;
    hud.heights[1] = -1.0f; // out of range
    EXPECT_EQ (ChosenHeights (hud), (std::vector<float> { 0.15f, 1.5f }));
}

TEST (PlantingHudModel, ClassesAreExactAtTheirEdges)
{
    PlantingHud hud;
    EXPECT_EQ (ClassCount (hud), 3);
    EXPECT_EQ (ClassOf (hud, 2.999), 0);
    EXPECT_EQ (ClassOf (hud, 3.0), 1) << "an exact edge belongs to the upper class";
    EXPECT_EQ (ClassOf (hud, 6.0), 2);
    EXPECT_STREQ (ClassName (hud, 0), "Shade");
    EXPECT_STREQ (ClassName (hud, 2), "Full sun");
    hud.deepShade = true;
    EXPECT_EQ (ClassCount (hud), 4);
    EXPECT_STREQ (ClassName (hud, 0), "Deep shade");
    EXPECT_EQ (ClassOf (hud, 0.5), 0);
    EXPECT_EQ (ClassOf (hud, 1.0), 1);
}

TEST (PlantingHudModel, EdgesAreKeptInOrder)
{
    PlantingHud hud;
    hud.partialEdge = 8.0f;
    hud.sunEdge = 5.0f;
    hud.deepShadeEdge = 9.0f;
    ClampEdges (hud);
    EXPECT_LT (hud.deepShadeEdge, hud.partialEdge);
    EXPECT_LT (hud.partialEdge, hud.sunEdge);
}

TEST (PlantingHudModel, TheSteppedTintSamplesEachClassColourExactly)
{
    PlantingHud hud;
    int count = 0;
    float edges[kSunBandsMax - 1] = {};
    ClassBands (hud, kSunRampTop, count, edges, kSunBandsMax - 1);
    ASSERT_EQ (count, 3);
    EXPECT_FLOAT_EQ (edges[0], 3.0f);
    EXPECT_FLOAT_EQ (edges[1], 6.0f);
    EXPECT_FLOAT_EQ (edges[2], kSunRampTop);
    const auto stops = ClassStops (hud, kSunRampTop);
    ASSERT_EQ (stops.size (), 3u);
    // The tint draws a band in the ramp's colour at the band's centre.
    for (int band = 0; band < count; ++band) {
        const float centre = SunStudyBandCentre (band, count, edges);
        EXPECT_EQ (SunStudyRampColour (stops, centre), ClassRgba (hud, band)) << band;
    }
    hud.classRgba[3] = 0x123456FFu;
    EXPECT_EQ (ClassStops (hud, kSunRampTop).back ().rgba, 0x123456FFu);
}

TEST (PlantingHudModel, ALayerAtTheSameHeightIsReplaced)
{
    PlantingHud hud;
    KeepLayer (hud, 1.5f, "sun-2", false);
    KeepLayer (hud, 0.15f, "sun-1", false);
    ASSERT_EQ (hud.layers.size (), 2u);
    EXPECT_FLOAT_EQ (hud.layers[0].height, 0.15f) << "sorted by height";
    EXPECT_EQ (hud.shownLayer, 0);
    KeepLayer (hud, 1.5f, "sun-3", true);
    ASSERT_EQ (hud.layers.size (), 2u);
    EXPECT_EQ (hud.layers[1].studyId, "sun-3");
    EXPECT_TRUE (hud.layers[1].period);
    EXPECT_EQ (LayerAt (hud, 3.0f), -1);
}
