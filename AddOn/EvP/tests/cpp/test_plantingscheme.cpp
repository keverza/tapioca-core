// PlantingScheme / PlantingSchemeScene (#278): a mixed planting placed on a
// planting layer's samples, its community notes and its viewer geometry.

#include "ArchViz/PlantingHudModel.hpp"
#include "ArchViz/PlantingScheme.hpp"
#include "ArchViz/PlantingSchemeScene.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace geomsrv::archviz::planting;

namespace {

// 10 x 10 m at 0.5 m: x < 5 is shade (class 0), x >= 5 full sun (class 2).
SchemeSite Courtyard ()
{
    SchemeSite site;
    site.layerHeight = 0.5f;
    for (int i = 0; i < 20; ++i)
        for (int j = 0; j < 20; ++j) {
            const double x = 0.25 + 0.5 * i, y = 0.25 + 0.5 * j;
            site.x.push_back (x);
            site.y.push_back (y);
            site.groundZ.push_back (1.0);
            site.cls.push_back (x < 5 ? 0 : 2);
            site.host.push_back (0);
        }
    return site;
}

Catalogue Plants ()
{
    Catalogue c;
    ParseProblems problems;
    const bool ok = ParseCatalogue (
        R"({"schemaVersion": 1, "catalogueId": "t", "sources": [{"id": "s1", "title": "S"}], "plants": [
        {"id": "tree", "scientificName": "Tilia", "habit": "tree", "light": {"acceptedClasses": ["full"]},
         "matureSpreadM": {"min": 2, "max": 3}, "matureHeightM": {"min": 8, "max": null}, "soilTypes": ["B"]},
        {"id": "shrub", "scientificName": "Taxus", "habit": "shrub", "light": {"acceptedClasses": ["shade"]},
         "matureSpreadM": {"min": 1, "max": 1}, "moistureClasses": ["moist"], "soilTypes": ["C"]},
        {"id": "spreader", "scientificName": "Aster", "habit": "perennial", "growthIntensity": "aggressive",
         "light": {"acceptedClasses": ["shade", "partial"]}, "plantingDensityPerM2": {"min": 9, "max": 9},
         "moistureClasses": ["dry"], "plantingLayers": ["D"]},
        {"id": "slow", "scientificName": "Asarum", "habit": "perennial", "growthIntensity": "slow",
         "light": {"acceptedClasses": ["shade"]}, "plantingDensityPerM2": {"min": 9, "max": 9},
         "moistureClasses": ["moist"]},
        {"id": "unknown", "scientificName": "Nescio"},
        {"id": "banned", "scientificName": "Invasor", "light": {"acceptedClasses": ["full"]},
         "regulatory": {"status": "prohibited"}}]})",
        c, problems);
    EXPECT_TRUE (ok) << (problems.Empty () ? "" : problems.items[0]);
    return c;
}

PlantingHud Hud ()
{
    PlantingHud hud;
    hud.deepShade = false; // classes 0 shade, 1 partial, 2 full
    return hud;
}

bool Has (const std::vector<SchemeNote>& notes, const std::string& text)
{
    for (const auto& n : notes)
        if (n.text.find (text) != std::string::npos)
            return true;
    return false;
}

} // namespace

TEST (PlantingScheme, SizesComeFromTheCatalogueAndSayWhere)
{
    const Catalogue c = Plants ();
    const SchemeSpeciesSize tree = SizeOf (c.plants[0]);
    EXPECT_DOUBLE_EQ (tree.radius, 1.5) << "the larger stated spread: conservative";
    EXPECT_EQ (tree.radiusFrom, "spread");
    EXPECT_DOUBLE_EQ (tree.height, 8.0) << "16+ style open height: its stated end";
    const SchemeSpeciesSize spreader = SizeOf (c.plants[2]);
    EXPECT_NEAR (spreader.radius, std::sqrt (1.0 / (3.14159265358979 * 9)), 1e-9);
    EXPECT_EQ (spreader.radiusFrom, "density");
    EXPECT_EQ (spreader.heightFrom, "layer");
    EXPECT_FLOAT_EQ (float (spreader.height), 0.15f) << "a cover-layer perennial's receiver height";
    const SchemeSpeciesSize unknown = SizeOf (c.plants[4]);
    EXPECT_EQ (unknown.radiusFrom, "assumed");
    EXPECT_DOUBLE_EQ (unknown.radius, 0.25);
}

TEST (PlantingScheme, PlantsGoOnAcceptedLightAndCrownsDoNotOverlap)
{
    const Catalogue c = Plants ();
    const SchemeSite site = Courtyard ();
    const std::vector<SchemeSpecies> mix = { { "tree", 3 }, { "shrub", 4 }, { "slow", 10 } };
    const SchemeResult r = PlaceScheme (Hud (), c, mix, site);
    EXPECT_EQ (r.placed, (std::vector<int> { 3, 4, 10 }));
    for (const PlacedPlant& p : r.plants) {
        if (p.species == 0)
            EXPECT_GE (p.x, 5.0) << "the tree accepts only full sun";
        else
            EXPECT_LT (p.x, 5.0) << "shade plants stay in the shade";
        EXPECT_DOUBLE_EQ (p.z, 1.0);
    }
    for (size_t i = 0; i < r.plants.size (); ++i)
        for (size_t k = i + 1; k < r.plants.size (); ++k) {
            const PlacedPlant& a = r.plants[i];
            const PlacedPlant& b = r.plants[k];
            EXPECT_GE (std::hypot (a.x - b.x, a.y - b.y), a.radius + b.radius - 1e-9) << "crowns do not overlap";
        }
    // One spanning tree per species: n - 1 links, never across species.
    EXPECT_EQ (r.links.size (), size_t (2 + 3 + 9));
    for (const SchemeLink& l : r.links)
        EXPECT_EQ (r.plants[size_t (l.a)].species, r.plants[size_t (l.b)].species);

    const SchemeResult again = PlaceScheme (Hud (), c, mix, site);
    ASSERT_EQ (again.plants.size (), r.plants.size ());
    for (size_t i = 0; i < r.plants.size (); ++i)
        EXPECT_EQ (again.plants[i].x, r.plants[i].x) << "deterministic";
}

TEST (PlantingScheme, TooManyIsReportedAndZeroFills)
{
    const Catalogue c = Plants ();
    const SchemeSite site = Courtyard ();
    SchemeResult r = PlaceScheme (Hud (), c, { { "tree", 40 } }, site);
    EXPECT_LT (r.placed[0], 40);
    EXPECT_GT (r.placed[0], 0);
    EXPECT_TRUE (Has (r.notes, "Only " + std::to_string (r.placed[0]) + " of 40 fit: Tilia"));

    SchemeOptions options;
    options.maxPlants = 25;
    r = PlaceScheme (Hud (), c, { { "slow", 0 } }, site, options);
    EXPECT_EQ (r.placed[0], 25) << "0 is as many as fit, up to the cap";
    EXPECT_TRUE (Has (r.notes, "Stopped at 25 plants"));

    r = PlaceScheme (Hud (), c, { { "unknown", 2 } }, site);
    EXPECT_EQ (r.placed[0], 2);
    EXPECT_FALSE (r.plants[0].lightChecked);
    EXPECT_TRUE (Has (r.notes, "placed without a light check"));
    EXPECT_TRUE (Has (r.notes, "assumed 0.25 m crown radius"));

    r = PlaceScheme (Hud (), c, { { "tree", 1 } }, SchemeSite {});
    EXPECT_TRUE (Has (r.notes, "No measured samples"));
}

TEST (PlantingScheme, CommunityNotesAreSiteFirst)
{
    const Catalogue c = Plants ();
    const auto notes = CheckCommunity (
        c, { { "shrub", 1 }, { "spreader", 1 }, { "slow", 1 }, { "banned", 1 }, { "unknown", 1 }, { "gone", 1 } });
    EXPECT_TRUE (Has (notes, "Water needs do not overlap"));
    EXPECT_TRUE (Has (notes, "not plant-to-plant conflict"));
    EXPECT_TRUE (Has (notes, "Aster spreads strongly and may suppress Asarum"));
    EXPECT_TRUE (Has (notes, "Invasor is prohibited"));
    EXPECT_TRUE (Has (notes, "Nescio: no light, water and ground type"));
    EXPECT_TRUE (Has (notes, "'gone' is no longer in the catalogue"));
    EXPECT_TRUE (Has (notes, "Bed layers: cover yes, seasonal no"));
    const auto same = CheckCommunity (c, { { "shrub", 1 }, { "slow", 1 } });
    EXPECT_FALSE (Has (same, "Water needs")) << "both moist";
}

TEST (PlantingSchemeScene, OneCylinderPerPlantAndOneLinePerLink)
{
    const Catalogue c = Plants ();
    const SchemeResult r = PlaceScheme (Hud (), c, { { "tree", 3 }, { "shrub", 2 } }, Courtyard ());
    ASSERT_EQ (r.plants.size (), 5u);
    PlantingSchemeGeometry g;
    BuildPlantingSchemeScene (r, SchemeMarker::Disc, -1, g);
    EXPECT_EQ (g.plants, 5u);
    // 24 side quads + a 24-triangle cap = 72 triangles a cylinder.
    EXPECT_EQ (g.crowns.translucent.meshIndices.size (), size_t (5 * (24 * 6 + 24 * 3)));
    // Six vertices a segment: two centre ticks per plant plus one per link.
    EXPECT_EQ (g.links.depthTested.lineVertices.size (), size_t ((5 * 2 + r.links.size ()) * 6));
    float topDisc = 0;
    for (const auto& v : g.crowns.translucent.meshVertices)
        topDisc = std::max (topDisc, v.z);
    EXPECT_NEAR (topDisc, 1.0 + 0.01 + kSchemeDiscHeight, 1e-5);

    BuildPlantingSchemeScene (r, SchemeMarker::Mature, 1, g);
    float topMature = 0;
    for (const auto& v : g.crowns.translucent.meshVertices)
        topMature = std::max (topMature, v.z);
    EXPECT_NEAR (topMature, 1.0 + 0.01 + 8.0, 1e-4) << "the tree's height";
    // The highlighted species keeps its alpha; the others are dimmed.
    uint32_t treeRgba = 0, shrubAlpha = 0;
    for (const auto& v : g.crowns.translucent.meshVertices)
        if (v.z > 3)
            treeRgba = v.rgba;
    for (const auto& v : g.crowns.translucent.meshVertices)
        if ((v.rgba >> 8) != (treeRgba >> 8))
            shrubAlpha = std::max (shrubAlpha, v.rgba & 0xffu);
    EXPECT_LT (treeRgba & 0xffu, shrubAlpha);
}
