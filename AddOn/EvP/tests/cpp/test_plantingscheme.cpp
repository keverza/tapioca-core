// PlantingScheme / PlantingSchemeScene (#278): a mixed planting placed on a
// planting layer's samples, its community notes and its viewer geometry.

#include "ArchViz/PlantingHudModel.hpp"
#include "ArchViz/PlantingScheme.hpp"
#include "ArchViz/PlantingSchemeScene.hpp"
#include "ArchViz/PlantHardiness.hpp"
#include "ArchViz/PlantingSizeFit.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
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
    EXPECT_TRUE (Has (notes, "Aster grows aggressively (the standard) and may overrun Asarum"));
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

TEST (PlantingZones, AreaAndContainmentHonourHoles)
{
    PlantingZone z;
    z.rings = { { 0, 0, 2, 0, 2, 2, 0, 2 }, { 0.5, 0.5, 1.5, 0.5, 1.5, 1.5, 0.5, 1.5 } };
    EXPECT_DOUBLE_EQ (z.PlanArea (), 3.0);
    EXPECT_TRUE (z.Contains (0.25, 0.25));
    EXPECT_FALSE (z.Contains (1.0, 1.0)) << "inside the hole";
    EXPECT_FALSE (z.Contains (3.0, 1.0));
    PlantingZone clockwise;
    clockwise.rings = { { 0, 0, 0, 2, 2, 2, 2, 0 } };
    EXPECT_DOUBLE_EQ (clockwise.PlanArea (), 4.0) << "ring direction does not matter";
}

TEST (PlantingZones, MasksAndClassAreasUseTheExactComparator)
{
    PlantingZone a, b;
    a.rings = { { 0, 0, 2, 0, 2, 2, 0, 2 } };
    b.rings = { { 1, 0, 3, 0, 3, 2, 1, 2 } };
    const std::vector<double> x = { 0.5, 1.5, 2.5, 5.0 }, y = { 1, 1, 1, 1 };
    const auto masks = ZoneMasks ({ a, b }, x, y);
    EXPECT_EQ (masks, (std::vector<uint32_t> { 1u, 3u, 2u, 0u })) << "the middle sample is in both";

    const std::vector<double> hours = { 2.99, 3.0, 6.0, 1.0 }, areas = { 1, 2, 4, 8 };
    const ZoneClassAreas za = ClassAreasInZone (masks, 1u, hours, areas, { 3.0, 6.0 });
    EXPECT_EQ (za.classAreas, (std::vector<double> { 1.0, 2.0, 0.0 })) << "3.00 h is partial shade";
    EXPECT_DOUBLE_EQ (za.measured, 3.0);
    EXPECT_EQ (za.samples, 2u);
    const ZoneClassAreas zb = ClassAreasInZone (masks, 2u, hours, areas, { 3.0, 6.0 });
    EXPECT_EQ (zb.classAreas, (std::vector<double> { 0.0, 2.0, 4.0 }));
    const ZoneClassAreas outside = ClassAreasInZone (masks, 0u, hours, areas, { 3.0, 6.0 });
    EXPECT_DOUBLE_EQ (outside.measured, 8.0) << "bit 0 is outside every zone";
}

TEST (PlantingScheme, AZoneLimitsWherePlantsGo)
{
    const Catalogue c = Plants ();
    PlantingZone shade;
    shade.rings = { { 0, 0, 4, 0, 4, 10, 0, 10 } };
    const SchemeSite inside = RestrictToZone (Courtyard (), shade);
    EXPECT_EQ (inside.Size (), size_t (8 * 20)) << "the 0.5 m samples with x < 4";
    SchemeResult r = PlaceScheme (Hud (), c, { { "shrub", 3 } }, inside);
    EXPECT_EQ (r.placed[0], 3);
    for (const PlacedPlant& p : r.plants)
        EXPECT_LT (p.x, 4.0);
    r = PlaceScheme (Hud (), c, { { "tree", 1 } }, inside);
    EXPECT_EQ (r.placed[0], 0) << "a full-sun tree has no accepted light in a shaded zone";
    EXPECT_TRUE (Has (r.notes, "Only 0 of 1 fit"));
}

namespace {

// The courtyard's room: a wall along its east edge (x = 10) at every level,
// and a balcony over y > 7 with its underside at 3 m.
evp::sunstudy::CrownClearance CourtyardRoom (const SchemeSite& site)
{
    evp::sunstudy::CrownClearance room;
    room.levels = evp::sunstudy::CrownClearanceOptions ().levels;
    room.reach = 12.0;
    room.directions = 32;
    for (size_t s = 0; s < site.Size (); ++s) {
        const bool balcony = site.y[s] > 7.0;
        room.overhead.push_back (balcony ? 3.0f : -1.0f);
        for (double level : room.levels)
            room.free.push_back (balcony && level >= 3.0 ? 0.0f : float (10.0 - site.x[s]));
    }
    return room;
}

} // namespace

TEST (PlantingScheme, FitAtReadsOverheadAndTheLevelsTheCrownReaches)
{
    evp::sunstudy::CrownClearance room;
    room.levels = { 0.5, 2.0, 6.0 };
    room.reach = 4.0;
    room.directions = 32;
    room.overhead = { -1.0f, 2.5f };
    room.free = { 3.0f, 3.0f, 0.5f, 4.0f, 4.0f, 0.0f };
    EXPECT_EQ (FitAt (room, 0, 1.0, 3.0, 0.0), SpaceFit::Fits) << "the 6 m level is above a 3 m crown";
    EXPECT_EQ (FitAt (room, 0, 1.0, 6.0, 0.0), SpaceFit::Narrow) << "a 6 m crown reaches the narrow level";
    EXPECT_EQ (FitAt (room, 0, 2.9, 1.0, 0.2), SpaceFit::Narrow) << "the margin counts sideways";
    EXPECT_EQ (FitAt (room, 1, 1.0, 2.0, 0.0), SpaceFit::Fits);
    EXPECT_EQ (FitAt (room, 1, 1.0, 2.4, 0.2), SpaceFit::Overhead) << "the margin counts above";
    EXPECT_EQ (FitAt (room, 1, 5.0, 1.0, 0.0), SpaceFit::Beyond) << "wider than the reach is not known";
    EXPECT_EQ (FitAt (room, 0, 0.1, 0.1, 0.0), SpaceFit::Fits) << "the lowest level always counts";
}

TEST (PlantingScheme, MeasuredRoomKeepsCrownsClearOfWallsAndOverhangs)
{
    const Catalogue c = Plants ();
    const PlantingHud hud = Hud ();
    SchemeSite site = Courtyard ();
    site.clearance = CourtyardRoom (site);
    const SchemeResult r = PlaceScheme (hud, c, { { "tree", 0 } }, site);
    ASSERT_TRUE (r.spaceChecked);
    ASSERT_GT (r.placed[0], 0);
    for (const auto& p : r.plants) {
        EXPECT_TRUE (p.spaceChecked);
        EXPECT_LE (p.x, 8.5 + 1e-9) << "the crown clears the east wall";
        EXPECT_LE (p.y, 7.0 + 1e-9) << "an 8 m tree never stands under the 3 m balcony";
    }
    ASSERT_EQ (r.room.size (), 1u);
    const SchemeRoom& room = r.room[0];
    EXPECT_EQ (room.lightSamples, 200u);
    EXPECT_EQ (room.overhead, 60u) << "y > 7 on the sunny half: 10 x 6 samples";
    EXPECT_EQ (room.narrow, 42u) << "x > 8.5 outside the balcony: 3 x 14";
    EXPECT_EQ (room.fits + room.overhead + room.narrow + room.beyond, room.lightSamples);
    EXPECT_TRUE (Has (r.notes, "too close to a wall"));

    // Switched off, the same room is ignored, and the notes say walls were not checked.
    SchemeOptions off;
    off.checkSpace = false;
    const SchemeResult loose = PlaceScheme (hud, c, { { "tree", 0 } }, site, off);
    EXPECT_FALSE (loose.spaceChecked);
    EXPECT_GT (loose.placed[0], r.placed[0]);
    EXPECT_TRUE (Has (loose.notes, "Walls and overhangs were not checked"));
}

TEST (PlantingScheme, AZoneKeepsTheRoomOfItsOwnSamples)
{
    SchemeSite site = Courtyard ();
    site.clearance = CourtyardRoom (site);
    PlantingZone zone;
    zone.rings = { { 6, 6, 10, 6, 10, 10, 6, 10 } };
    const SchemeSite inside = RestrictToZone (site, zone);
    ASSERT_EQ (inside.clearance.Count (), inside.Size ());
    for (size_t s = 0; s < inside.Size (); ++s) {
        EXPECT_EQ (inside.clearance.overhead[s], inside.y[s] > 7.0 ? 3.0f : -1.0f);
        EXPECT_FLOAT_EQ (inside.clearance.FreeAt (s, 0), float (10.0 - inside.x[s]));
    }
}

TEST (PlantingZones, PlanDistanceAndNoGoDistances)
{
    PlantingZone path;
    path.role = ZoneRole::NoGo;
    path.rings = { { 0, 0, 4, 0, 4, 4, 0, 4 }, { 1, 1, 3, 1, 3, 3, 1, 3 } }; // a square with a square hole
    EXPECT_DOUBLE_EQ (path.PlanDistance (0.5, 2.0), 0.0) << "inside";
    EXPECT_DOUBLE_EQ (path.PlanDistance (7.0, 2.0), 3.0) << "to the outline";
    EXPECT_DOUBLE_EQ (path.PlanDistance (2.0, 2.0), 1.0) << "in the hole: to the hole's edge";
    PlantingZone bed;
    bed.rings = { { 10, 10, 11, 10, 11, 11 } };
    std::vector<float> distance, allowance;
    NoGoDistances ({ bed, path }, { 0.5, 7.0 }, { 2.0, 2.0 }, distance, allowance);
    ASSERT_EQ (allowance.size (), 1u) << "a bed is not a no-go zone";
    EXPECT_EQ (distance, (std::vector<float> { 0.0f, 3.0f }));
    NoGoDistances ({ bed }, { 0.5 }, { 2.0 }, distance, allowance);
    EXPECT_TRUE (distance.empty () && allowance.empty ());
}

TEST (PlantingScheme, NoGoZonesKeepCrownsOutUnlessLowEnough)
{
    const Catalogue c = Plants ();
    const PlantingHud hud = Hud ();
    SchemeSite site = Courtyard ();
    PlantingZone path;
    path.role = ZoneRole::NoGo;
    path.rings = { { 6, 0, 7, 0, 7, 10, 6, 10 } }; // a 1 m path, north to south, in the sun
    NoGoDistances ({ path }, site.x, site.y, site.noGoDistance, site.noGoAllowance);
    const SchemeResult r = PlaceScheme (hud, c, { { "tree", 0 } }, site);
    ASSERT_GT (r.placed[0], 0);
    for (const auto& p : r.plants)
        EXPECT_TRUE (p.x <= 6.0 - 1.5 + 1e-9 || p.x >= 7.0 + 1.5 - 1e-9) << "crown clear of the path: x " << p.x;
    ASSERT_EQ (r.room.size (), 1u);
    EXPECT_GT (r.room[0].noGo, 0u);
    EXPECT_TRUE (Has (r.notes, "no-go"));
    EXPECT_TRUE (Has (r.notes, "Walls and overhangs were not checked")) << "no room measured here";

    // A zone that allows 10 m does not stop an 8 m tree.
    site.noGoAllowance = { 10.0f };
    const SchemeResult low = PlaceScheme (hud, c, { { "tree", 0 } }, site);
    EXPECT_EQ (low.room[0].noGo, 0u);
    EXPECT_FALSE (KeptOut (site, 0, 1.5, 8.0, 0.0));
    EXPECT_TRUE (KeptOut (site, 0, 1.5, 12.0, 0.0) == (site.noGoDistance[0] < 1.5));
}

TEST (PlantingSizeFit, EachClassIsCheckedOnItsOwnAndTheLargestWins)
{
    const PlantingHud hud = Hud ();
    SchemeSite site = Courtyard ();
    site.clearance = CourtyardRoom (site); // east wall at x = 10, a 3 m balcony over y > 7
    site.area.assign (site.Size (), 0.25);
    std::vector<SizeClass> classes = DefaultSizeClasses (); // Small 1.5/1.5, Medium 4/6, Large 10/15
    SizeClass narrow;
    narrow.name = "Columnar";
    narrow.spread = 0.5f;
    narrow.height = 20.0f;
    classes.push_back (narrow);
    const SizeFitResult fit = EvaluateSizeClasses (hud, site, classes, 0.0);
    ASSERT_TRUE (fit.spaceChecked);
    EXPECT_EQ (fit.order, (std::vector<int> { 2, 1, 0, 3 })) << "by spread, largest first";
    auto at = [&] (double x, double y) {
        for (size_t s = 0; s < site.Size (); ++s)
            if (std::abs (site.x[s] - x) < 1e-9 && std::abs (site.y[s] - y) < 1e-9)
                return s;
        return site.Size ();
    };
    const size_t k = classes.size ();
    const size_t open = at (5.25, 5.25); // 4.75 m from the wall
    EXPECT_EQ (fit.ReasonAt (open, 2, k), SizeReason::Narrow) << "Large needs 5 m";
    EXPECT_EQ (fit.ReasonAt (open, 1, k), SizeReason::Fits);
    EXPECT_EQ (fit.largest[open], 1) << "Medium";
    const size_t under = at (5.25, 8.25);
    EXPECT_EQ (fit.ReasonAt (under, 1, k), SizeReason::Overhead);
    EXPECT_EQ (fit.ReasonAt (under, 3, k), SizeReason::Overhead);
    EXPECT_EQ (fit.largest[under], 0) << "only Small goes under the balcony";
    const size_t edge = at (9.75, 2.25); // 0.25 m from the wall
    EXPECT_EQ (fit.ReasonAt (edge, 0, k), SizeReason::Narrow);
    EXPECT_EQ (fit.largest[edge], 3) << "not nested: the columnar class fits where Small does not";

    // Light is its own reason, and every class's row sums to the evaluated area.
    classes[0].light = { false, false, true };
    const SizeFitResult lit = EvaluateSizeClasses (hud, site, classes, 0.0);
    EXPECT_EQ (lit.ReasonAt (at (1.25, 1.25), 0, k), SizeReason::Light);
    EXPECT_NEAR (lit.measured, 100.0, 1e-9);
    for (int c : lit.order) {
        double sum = 0;
        for (double v : lit.area[size_t (c)])
            sum += v;
        EXPECT_NEAR (sum, 100.0, 1e-9);
    }

    // Unmeasured room is not a pass.
    site.clearance = {};
    const SizeFitResult unknown = EvaluateSizeClasses (hud, site, classes, 0.0);
    EXPECT_FALSE (unknown.spaceChecked);
    EXPECT_EQ (unknown.largest[open], -1);
    EXPECT_EQ (unknown.anyUnknown[open], 1);
    EXPECT_EQ (unknown.ReasonAt (open, 1, k), SizeReason::Unknown);
}

TEST (PlantingSizeFit, NoGoAndOffAreReasonsAndTheMapHasACellPerSample)
{
    const PlantingHud hud = Hud ();
    SchemeSite site = Courtyard ();
    site.clearance = CourtyardRoom (site);
    PlantingZone path;
    path.role = ZoneRole::NoGo;
    path.rings = { { 6, 0, 7, 0, 7, 10, 6, 10 } };
    NoGoDistances ({ path }, site.x, site.y, site.noGoDistance, site.noGoAllowance);
    std::vector<SizeClass> classes = DefaultSizeClasses ();
    classes[2].on = false;
    const SizeFitResult fit = EvaluateSizeClasses (hud, site, classes, 0.0);
    EXPECT_EQ (fit.order, (std::vector<int> { 1, 0 }));
    EXPECT_EQ (fit.ReasonAt (0, 2, 3), SizeReason::Off);
    size_t onPath = site.Size ();
    for (size_t s = 0; s < site.Size (); ++s)
        if (site.x[s] == 6.25 && site.y[s] == 2.25)
            onPath = s;
    ASSERT_LT (onPath, site.Size ());
    EXPECT_EQ (fit.ReasonAt (onPath, 0, 3), SizeReason::NoGo);
    EXPECT_EQ (fit.largest[onPath], -1);

    PlantingSchemeGeometry geometry;
    AppendSizeFitMap (site, fit, geometry);
    EXPECT_EQ (geometry.crowns.translucent.meshVertices.size (), site.Size () * 4);
    EXPECT_EQ (geometry.crowns.translucent.meshIndices.size (), site.Size () * 6);
}

TEST (PlantHardiness, ZonesFromTemperatureAndRanges)
{
    EXPECT_EQ (UsdaName (UsdaHalfZoneOf (-23.0)), "6a") << "the Vilnius typical year's coldest hour";
    EXPECT_EQ (UsdaName (UsdaHalfZoneOf (-23.0 - 1.0 / 3.0)), "6a") << "-10 F exactly is the edge of 6a";
    EXPECT_EQ (UsdaName (UsdaHalfZoneOf (-23.4)), "5b");
    EXPECT_EQ (UsdaName (UsdaHalfZoneOf (-6.1)), "9a");
    EXPECT_EQ (UsdaName (UsdaHalfZoneOf (-80.0)), "1a") << "clamped";
    EXPECT_EQ (UsdaName (-1), "");
    int lo = 0, hi = 0;
    ASSERT_TRUE (ParseUsdaRange ("USDA 4a-8b", lo, hi));
    EXPECT_EQ (UsdaName (lo) + UsdaName (hi), "4a8b");
    ASSERT_TRUE (ParseUsdaRange ("USDA 3a-7a", lo, hi));
    EXPECT_EQ (UsdaName (hi), "7a");
    ASSERT_TRUE (ParseUsdaRange ("usda 4-8", lo, hi));
    EXPECT_EQ (UsdaName (lo) + UsdaName (hi), "4a8b") << "whole zones";
    ASSERT_TRUE (ParseUsdaRange ("4a\xE2\x80\x93"
                                 "8b",
                                 lo, hi))
        << "an en dash";
    ASSERT_TRUE (ParseUsdaRange ("USDA 4", lo, hi));
    EXPECT_EQ (UsdaName (lo) + UsdaName (hi), "4a13b") << "hardy down to 4, no warm end";
    ASSERT_TRUE (ParseUsdaRange ("RHS H6", lo, hi));
    EXPECT_EQ (UsdaName (lo), "6b") << "H6's cold end, -20 C";
    EXPECT_FALSE (ParseUsdaRange ("USDA 8a-4b", lo, hi));
    EXPECT_FALSE (ParseUsdaRange ("hardy", lo, hi));
    EXPECT_EQ (lo, -1);

    const int zone6a = UsdaHalfZoneOf (-23.0);
    EXPECT_EQ (HardinessAt ("USDA 4a-8b", zone6a), Hardiness::Hardy);
    EXPECT_EQ (HardinessAt ("USDA 7a-10b", zone6a), Hardiness::TooCold);
    EXPECT_EQ (HardinessAt ("RHS H6", zone6a), Hardiness::TooCold) << "H6 holds to -20 C, 6b";
    EXPECT_EQ (HardinessAt ("USDA 2a-5b", zone6a), Hardiness::Warmer);
    EXPECT_EQ (HardinessAt ("", zone6a), Hardiness::Unknown);
    EXPECT_EQ (HardinessAt ("USDA 4", -1), Hardiness::Unknown) << "no design zone";
}

namespace {

Catalogue TraitPlants ()
{
    Catalogue c;
    ParseProblems problems;
    const bool ok = ParseCatalogue (
        R"({"schemaVersion": 1, "catalogueId": "t", "sources": [{"id": "s1", "title": "S"}], "plants": [
        {"id": "bog", "scientificName": "Vaccinium", "light": {"acceptedClasses": ["full"]},
         "ecology": {"ellenbergMoisture": 8, "ellenbergNutrients": 2, "pHMin": 4.0, "pHMax": 5.5,
                     "hardiness": "USDA 3a-7b"},
         "competition": {"establishmentSpeed": "slow"}},
        {"id": "chalk", "scientificName": "Sesleria", "light": {"acceptedClasses": ["full"]},
         "ecology": {"ellenbergMoisture": 3, "ellenbergNutrients": 8, "pHMin": 7.0, "pHMax": 8.5,
                     "hardiness": "USDA 7a-9b"},
         "competition": {"spreadMode": ["clump", "longRhizome"]}},
        {"id": "plain", "scientificName": "Planta", "light": {"acceptedClasses": ["full"]}}]})",
        c, problems);
    EXPECT_TRUE (ok) << (problems.Empty () ? "" : problems.items[0]);
    return c;
}

} // namespace

TEST (PlantHardiness, FilterAndSchemeUseTheDesignZone)
{
    const Catalogue c = TraitPlants ();
    PlantFilter f;
    f.hardyZone = UsdaHalfZoneOf (-23.0); // 6a
    EXPECT_TRUE (Matches (c.plants[0], f));
    EXPECT_FALSE (Matches (c.plants[1], f)) << "7a is too warm-limited for 6a";
    EXPECT_FALSE (Matches (c.plants[2], f)) << "no rating is not hardy";
    EXPECT_TRUE (f.Any ());

    PlantingHud hud = Hud ();
    const SchemeSite site = Courtyard ();
    const std::vector<SchemeSpecies> mix { { "bog", 1 }, { "chalk", 1 }, { "plain", 1 } };
    EXPECT_FALSE (Has (PlaceScheme (hud, c, mix, site).notes, "hardy only")) << "no design zone, no check";
    hud.climateState.designZone = f.hardyZone;
    const SchemeResult r = PlaceScheme (hud, c, mix, site);
    EXPECT_TRUE (Has (r.notes, "Sesleria is hardy only to USDA 7a-9b; the design zone is USDA 6a."));
    EXPECT_FALSE (Has (r.notes, "Vaccinium is hardy only"));
    EXPECT_TRUE (Has (r.notes, "Winter hardiness not checked (no USDA or RHS rating): Planta."));
}

TEST (PlantingScheme, FilledTraitsAddSiteAndCompetitionNotes)
{
    const Catalogue c = TraitPlants ();
    const std::vector<SchemeNote> notes = CheckCommunity (c, { { "bog", 1 }, { "chalk", 1 }, { "plain", 1 } });
    EXPECT_TRUE (Has (notes, "Soil pH ranges do not overlap (Vaccinium 4-5.5 and Sesleria 7-8.5)"));
    EXPECT_TRUE (Has (notes, "Ellenberg moisture values (F) are far apart: Sesleria 3, Vaccinium 8."));
    EXPECT_TRUE (Has (notes, "Ellenberg nutrient values (N) are far apart: Vaccinium 2, Sesleria 8."));
    EXPECT_TRUE (Has (notes, "Sesleria spreads by long rhizomes and may overrun Vaccinium (slow or weak)"));
    // A plant alone raises no envelope note.
    const std::vector<SchemeNote> one = CheckCommunity (c, { { "bog", 1 }, { "plain", 1 } });
    EXPECT_FALSE (Has (one, "pH"));
    EXPECT_FALSE (Has (one, "far apart"));
}

// Opt-in: TAPIOCA_EPW_DIR names a folder of the reference weather files
// (tapioca-ref\EPW, zipped as downloaded or unzipped; never committed). The
// coldest hour and its zone were read independently from the files' text.
TEST (PlantHardiness, ReferenceWeatherFilesGiveTheirColdestHourZone)
{
    const char* dir = std::getenv ("TAPIOCA_EPW_DIR");
    if (dir == nullptr || *dir == 0)
        GTEST_SKIP () << "set TAPIOCA_EPW_DIR to a folder of .epw files";
    const struct {
        const char* wmo;
        double coldestC;
        const char* zone;
    } kExpected[] = { { "267300", -23.0, "6a" }, { "266290", -19.1, "6b" }, { "121950", -26.2, "5a" },
                      { "160760", -6.1, "9a" },  { "725016", -2.5, "9b" },  { "725145", -22.0, "6a" } };
    size_t checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator (dir)) {
        if (entry.path ().extension () != ".epw" && !evp::sunstudy::IsZipPath (entry.path ()))
            continue;
        evp::sunstudy::WeatherData w;
        std::string error;
        ASSERT_TRUE (evp::sunstudy::ReadWeatherFile (entry.path (), w, error)) << error;
        double coldest = 0;
        ASSERT_TRUE (ColdestHourC (w, coldest));
        for (const auto& e : kExpected)
            if (entry.path ().filename ().string ().find (e.wmo) != std::string::npos) {
                EXPECT_NEAR (coldest, e.coldestC, 1e-9) << entry.path ();
                EXPECT_EQ (UsdaName (UsdaHalfZoneOf (coldest)), e.zone) << entry.path ();
                ++checked;
            }
    }
    EXPECT_GT (checked, 0u);
}

TEST (PlantingZones, AFitNarrowsToAZoneOnThePlantsOwnLayer)
{
    const Catalogue c = TraitPlants (); // "bog" accepts full sun only
    PlantingHud hud = Hud ();
    Layer layer;
    layer.height = 0.5f;
    layer.studyId = "s";
    layer.areaEdges = AreaEdges (hud);
    layer.classAreas = { 10.0, 20.0, 30.0 }; // shade, partial, full
    layer.evaluatedArea = 60.0;
    hud.layers.push_back (layer);
    const PlantFit whole = FitOf (hud, c.plants[0]);
    ASSERT_EQ (whole.status, PlantFit::Status::Fits);
    EXPECT_DOUBLE_EQ (whole.compatibleArea, 30.0);

    ZoneClassAreas bed;
    bed.classAreas = { 1.0, 0.5, 2.5 };
    bed.measured = 4.0;
    bed.samples = 16;
    const PlantFit in = FitInZone (hud, c.plants[0], whole, bed);
    EXPECT_EQ (in.status, PlantFit::Status::Fits);
    EXPECT_DOUBLE_EQ (in.compatibleArea, 2.5);
    EXPECT_DOUBLE_EQ (in.evaluatedArea, 4.0);
    EXPECT_EQ (in.layer, whole.layer) << "the same layer as the whole-layer fit";

    bed.classAreas = { 4.0, 0.0, 0.0 };
    EXPECT_EQ (FitInZone (hud, c.plants[0], whole, bed).status, PlantFit::Status::None) << "all shade";
    bed.samples = 0;
    bed.measured = 0;
    EXPECT_EQ (FitInZone (hud, c.plants[0], whole, bed).status, PlantFit::Status::Unknown) << "nothing measured";
    PlantFit noLayer = whole;
    noLayer.layer = -1;
    noLayer.status = PlantFit::Status::Unknown;
    EXPECT_EQ (FitInZone (hud, c.plants[0], noLayer, bed).status, PlantFit::Status::Unknown)
        << "no layer stays unknown";
}

TEST (PlantingSizeFit, EnvelopePreviewDrawsEveryClassAtThePoint)
{
    const PlantingHud hud = Hud ();
    SchemeSite site = Courtyard ();
    site.clearance = CourtyardRoom (site);
    const std::vector<SizeClass> classes = DefaultSizeClasses ();
    const SizeFitResult fit = EvaluateSizeClasses (hud, site, classes, 0.0);
    PlantingSchemeGeometry geometry;
    AppendEnvelopePreview (site, 0, classes, fit, geometry);
    // Per class: a 24-sided wall (2 x 25 vertices) and a top fan (1 + 25).
    EXPECT_EQ (geometry.crowns.translucent.meshVertices.size (), 3u * 76u);
    EXPECT_FALSE (geometry.crowns.depthTested.lineVertices.empty ()) << "rims";
    PlantingSchemeGeometry none;
    AppendEnvelopePreview (site, site.Size (), classes, fit, none);
    EXPECT_TRUE (none.crowns.translucent.meshVertices.empty ()) << "no row, nothing drawn";
}
