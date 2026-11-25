// The Layout Builder's Planting Suitability page (#278): composition of the
// layer tiles, the area table and the legend strip, and the capture frame's
// planting page beside the three seasonal ones.

#include "ArchViz/DiligentHudCaptureFrame.hpp"
#include "ArchViz/LayoutReportPage.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::archviz;

namespace {

LayoutReportPage PlantingPage (size_t tiles)
{
    LayoutReportPage page;
    page.type = ReportTemplate::Planting;
    page.format = 0;
    page.subtitle = "Light by layer height";
    page.classNames = { "Shade", "Partial shade", "Full sun" };
    page.notes = { "Classes from each sample's own hours." };
    for (size_t i = 0; i < tiles; ++i) {
        ReportPanel panel;
        panel.date = "0.15 m Ground cover";
        panel.metric = "Full sun 35%";
        panel.width = 30;
        panel.height = 20;
        panel.rgba.assign (size_t (30) * 20 * 4, 0);
        for (size_t k = 0; k < panel.rgba.size (); k += 4) {
            panel.rgba[k] = 200; // red
            panel.rgba[k + 3] = 255;
        }
        page.panels.push_back (panel);
        page.areaRows.push_back ({ panel.date, { 122.0, 202.0, 172.0 }, 5.0, 496.0 });
    }
    return page;
}

} // namespace

TEST (LayoutReportPlanting, ComposesOneTilePerLayerAndTheTable)
{
    std::vector<uint8_t> rgba;
    std::vector<PaperViewportRect> tiles;
    ASSERT_TRUE (ComposeReportPage (PlantingPage (3), rgba, &tiles));
    const auto page = ReportPageFormat (0);
    ASSERT_EQ (rgba.size (), size_t (page.width) * page.height * 4);
    ASSERT_EQ (tiles.size (), 3u);
    for (const auto& t : tiles) {
        EXPECT_GT (t.w, 0.0f);
        EXPECT_LE (t.x + t.w, 1.0f);
        // The tile's own pixels, red, at its centre.
        const size_t x = size_t ((t.x + t.w / 2) * page.width), y = size_t ((t.y + t.h / 2) * page.height);
        const uint8_t* px = &rgba[(y * page.width + x) * 4];
        EXPECT_GT (px[0], 150);
        EXPECT_LT (px[1], 60);
    }
    EXPECT_LT (tiles[0].x, tiles[1].x) << "one row, lowest layer first";
    EXPECT_FLOAT_EQ (tiles[0].y, tiles[2].y);
}

TEST (LayoutReportPlanting, RefusesMoreTilesThanThePageHolds)
{
    std::vector<uint8_t> rgba;
    EXPECT_FALSE (ComposeReportPage (PlantingPage (kPlantingMaxTiles + 1), rgba));
    EXPECT_FALSE (ComposeReportPage (PlantingPage (0), rgba));
    EXPECT_STREQ (ReportTitle (ReportTemplate::Planting), "Planting Suitability");
}

TEST (LayoutReportPlanting, ThePlantingPageIsNotPartOfTheSeasonalExport)
{
    viewerhud::CaptureFrame frame;
    frame.builder = true;
    frame.plantingPayloads.resize (2);
    EXPECT_TRUE (frame.PlantingBuilder ());
    frame.SelectWorkflow (int (ReportTemplate::Planting));
    EXPECT_EQ (frame.panelSteps.size (), 2u) << "a tile per layer";
    // "Capture all 3" walks the three sun pages and never reaches planting.
    frame.SelectWorkflow (0);
    frame.BeginPageExport ();
    int pages = 1;
    while (frame.AdvancePageExport ())
        ++pages;
    EXPECT_EQ (pages, kSunReportPages);
    EXPECT_EQ (frame.reportType, 0);
}
