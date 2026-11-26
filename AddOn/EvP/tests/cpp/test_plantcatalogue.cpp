// The local plant catalogue (#278 Stage 3): schema, refusal with every
// problem, profiles, the seed, the files' lifecycle and the species fit.

#include "ArchViz/PlantCatalogue.hpp"
#include "ArchViz/PlantingFiles.hpp"
#include "ArchViz/PlantingHudModel.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace geomsrv::archviz::planting;
namespace fs = std::filesystem;

namespace {

std::string WithPlants (const std::string& plants)
{
    return R"({"schemaVersion": 1, "catalogueId": "t", "sources": [{"id": "s1", "title": "Source"}], "plants": [)" +
           plants + "]}";
}

bool Has (const ParseProblems& p, const std::string& text)
{
    for (const auto& item : p.items)
        if (item.find (text) != std::string::npos)
            return true;
    return false;
}

struct TempFolder {
    fs::path path;
    TempFolder ()
    {
        path = fs::temp_directory_path () / (std::string ("tapioca-catalogue-") +
                                             ::testing::UnitTest::GetInstance ()->current_test_info ()->name ());
        std::error_code ec;
        fs::remove_all (path, ec);
    }
    ~TempFolder ()
    {
        std::error_code ec;
        fs::remove_all (path, ec);
    }
};

void Write (const fs::path& path, const std::string& text)
{
    std::ofstream (path, std::ios::binary) << text;
}

std::string Read (const fs::path& path)
{
    std::ifstream f (path, std::ios::binary);
    return { std::istreambuf_iterator<char> (f), std::istreambuf_iterator<char> () };
}

} // namespace

TEST (PlantCatalogue, TheSeedParsesWithItsLithuanianNames)
{
    Catalogue c;
    ParseProblems problems;
    ASSERT_TRUE (ParseCatalogue (SeedPlantsJson (), c, problems)) << (problems.Empty () ? "" : problems.items[0]);
    ASSERT_EQ (c.plants.size (), 269u);
    const Plant& koreana = c.plants[0];
    ASSERT_EQ (koreana.scientificName, "Abies koreana");
    EXPECT_EQ (koreana.section, PlantSection::Trees);
    EXPECT_EQ (koreana.habit, Habit::Tree);
    EXPECT_EQ (koreana.localNames.at ("lt"), "Korėjinis kėnis");
    ASSERT_TRUE (koreana.matureHeightM.has_value ());
    EXPECT_DOUBLE_EQ (koreana.matureHeightM->min, 8.0);
    EXPECT_DOUBLE_EQ (koreana.matureHeightM->max, 15.0);
    EXPECT_TRUE (koreana.siteSuitable[size_t (Site::SquaresParks)]);
    EXPECT_EQ (koreana.siteSoil[size_t (Site::SquaresParks)], "B");
    EXPECT_FALSE (koreana.breeam);
    const auto achillea = std::find_if (c.plants.begin (), c.plants.end (), [] (const Plant& p) {
        return p.scientificName == "Achillea filipendulina Coronation Gold";
    });
    ASSERT_NE (achillea, c.plants.end ());
    EXPECT_EQ (achillea->localNames.at ("lt"), "Vingiorykštinė kraujažolė");
    ASSERT_TRUE (achillea->densityPerM2.has_value ());
    EXPECT_DOUBLE_EQ (achillea->densityPerM2->min, 7.0);
    EXPECT_EQ (achillea->plantingLayers, (std::vector<PlantingLayer> { PlantingLayer::Seasonal }));
    EXPECT_EQ (achillea->moistureClasses, (std::vector<MoistureClass> { MoistureClass::Dry }));
    EXPECT_EQ (achillea->floweringMonths, (std::vector<int> { 6 }));
    std::vector<Profile> profiles;
    ASSERT_TRUE (ParseProfiles (SeedProfilesJson (), profiles, problems));
    ASSERT_EQ (profiles.size (), 1u);
    EXPECT_FLOAT_EQ (profiles[0].partialEdgeH, 3.0f);
    EXPECT_FLOAT_EQ (profiles[0].fullEdgeH, 6.0f);
    EXPECT_TRUE (profiles[0].hasSeason);
}

TEST (PlantCatalogue, AFieldByFieldRecord)
{
    Catalogue c;
    ParseProblems problems;
    ASSERT_TRUE (ParseCatalogue (WithPlants (R"({"id": "a", "scientificName": "Aa bb", "cultivar": "Gold",
        "section": "shrubs", "sourceNumber": "7", "habit": "shrub",
        "breeam": true, "ornamentalFlowering": true, "ornithological": true,
        "light": {"acceptedClasses": ["partial", "full"]}, "saltTolerance": "high",
        "growthIntensity": "slow", "moistureClasses": ["dry", "moist"], "plantingLayers": ["D", "Str"],
        "matureHeightM": {"min": 1, "max": 1.5}, "matureSpreadM": {"min": null, "max": 0.8},
        "preferredReceiverHeightM": 0.8, "plantingDensityPerM2": {"min": 5, "max": 7},
        "floweringMonths": [6, 7], "deciduous": true, "soilTypes": ["A", "B"],
        "sites": ["greenStrip", "onSlabs"], "siteSoils": {"greenStrip": "B", "onSlabs": "D"},
        "sourceRefs": [{"sourceId": "s1", "page": 20}], "somethingNew": 3})"),
                                 c, problems))
        << (problems.Empty () ? "" : problems.items[0]);
    const Plant& p = c.plants[0];
    EXPECT_EQ (p.DisplayName (), "Aa bb 'Gold'");
    EXPECT_EQ (p.section, PlantSection::Shrubs);
    EXPECT_EQ (p.sourceNumber, "7");
    EXPECT_EQ (p.habit, Habit::Shrub);
    EXPECT_TRUE (p.breeam && p.ornamentalFlowering && p.ornithological);
    EXPECT_FALSE (p.light[0]);
    EXPECT_TRUE (p.light[1] && p.light[2]);
    EXPECT_EQ (p.saltTolerance, SaltTolerance::High);
    EXPECT_EQ (p.growthIntensity, GrowthIntensity::Slow);
    EXPECT_EQ (p.moistureClasses, (std::vector<MoistureClass> { MoistureClass::Dry, MoistureClass::Moist }));
    EXPECT_EQ (p.plantingLayers, (std::vector<PlantingLayer> { PlantingLayer::Cover, PlantingLayer::Structural }));
    EXPECT_DOUBLE_EQ (p.matureHeightM->max, 1.5);
    EXPECT_TRUE (p.matureSpreadM->openMin);
    EXPECT_DOUBLE_EQ (p.matureSpreadM->max, 0.8);
    EXPECT_DOUBLE_EQ (p.densityPerM2->min, 5.0);
    EXPECT_DOUBLE_EQ (p.densityPerM2->max, 7.0);
    EXPECT_FLOAT_EQ (ReceiverHeightOf (p), 0.8f);
    EXPECT_EQ (p.soilTypes, (std::vector<std::string> { "A", "B" }));
    EXPECT_TRUE (p.siteSuitable[size_t (Site::GreenStrip)]);
    EXPECT_FALSE (p.siteSuitable[size_t (Site::Roadside)]);
    EXPECT_EQ (p.siteSoil[size_t (Site::OnSlabs)], "D");
    EXPECT_EQ (p.sources[0].page, 20);
    EXPECT_EQ (p.deciduous, true);
}

TEST (PlantCatalogue, EveryProblemIsListedAndNothingLoads)
{
    Catalogue c;
    c.catalogueId = "previous";
    ParseProblems problems;
    EXPECT_FALSE (
        ParseCatalogue (WithPlants (R"({"id": "a", "scientificName": "A", "light": {"acceptedClasses": ["sunny"]}},
        {"id": "a", "scientificName": "B", "habit": "bush"},
        {"id": "c", "scientificName": "C", "matureHeightM": {"min": 2, "max": 1}, "floweringMonths": [13],
         "plantingDensityPerM2": {"min": 5, "max": null}, "sourceRefs": [{"sourceId": "nowhere"}]},
        {"id": "d", "scientificName": "D", "section": "vines", "saltTolerance": "salty",
         "growthIntensity": "quick", "moistureClasses": ["damp"], "plantingLayers": ["X"],
         "sites": ["moon"], "siteSoils": {"moon": "B"}, "matureSpreadM": {"min": null, "max": null}},
        {"scientificName": "E"})"),
                        c, problems));
    EXPECT_EQ (c.catalogueId, "previous") << "a refused file changes nothing";
    EXPECT_TRUE (Has (problems, "plants[0] 'a': light.acceptedClasses[0]: 'sunny' is not shade, partial or full"));
    EXPECT_TRUE (Has (problems, "plants[1] 'a': id: is used twice"));
    EXPECT_TRUE (Has (problems, "'bush' is not one of"));
    EXPECT_TRUE (Has (problems, "plants[2] 'c': matureHeightM: needs min <= max"));
    EXPECT_TRUE (Has (problems, "plantingDensityPerM2: needs both min and max"));
    EXPECT_TRUE (Has (problems, "floweringMonths[0]: must be a month 1-12"));
    EXPECT_TRUE (Has (problems, "sourceId must name an entry of sources"));
    EXPECT_TRUE (Has (problems, "'vines' is not trees, shrubs or perennials"));
    EXPECT_TRUE (Has (problems, "'salty' is not none, low or high"));
    EXPECT_TRUE (Has (problems, "'quick' is not slow, fast or aggressive"));
    EXPECT_TRUE (Has (problems, "'damp' is not dry, moist or wet"));
    EXPECT_TRUE (Has (problems, "'X' is not D, S or Str"));
    EXPECT_TRUE (Has (problems, "'moon' is not a known planting site"));
    EXPECT_TRUE (Has (problems, "matureSpreadM: needs a min, a max, or both"));
    EXPECT_TRUE (Has (problems, "plants[4] id: is required"));
}

TEST (PlantTraits, TheSeedShipsEveryTraitEmpty)
{
    Catalogue c;
    ParseProblems problems;
    ASSERT_TRUE (ParseCatalogue (SeedPlantsJson (), c, problems)) << (problems.Empty () ? "" : problems.items[0]);
    for (const Plant& p : c.plants)
        EXPECT_FALSE (p.traits.AnyGiven ()) << p.id << ": the standard has no traits; nothing is invented";
    // The seed carries every block, so the lines to fill are visible.
    for (int t = 0; t < kTraitCount; ++t) {
        const TraitField& f = TraitFieldOf (Trait (t));
        const std::string key = std::string ("\"") + f.key + "\":";
        EXPECT_NE (SeedPlantsJson ().find (key), std::string::npos) << TraitPath (Trait (t));
    }
}

TEST (PlantTraits, AFilledRecord)
{
    Catalogue c;
    ParseProblems problems;
    ASSERT_TRUE (ParseCatalogue (WithPlants (R"({"id": "a", "scientificName": "Aa",
        "ecology": {"ellenbergLight": 4, "ellenbergMoisture": 6, "pHMin": 5.5, "pHMax": 7.5,
                    "nutrientPreference": "medium", "drainage": ["freelyDrained", "moderatelyDrained"],
                    "hardiness": "USDA 4"},
        "competition": {"establishmentSpeed": "fast", "competitiveVigour": "high", "spreadMode": ["longRhizome"],
                        "lateralSpread": "high", "selfSeeding": "none"},
        "roots": {"depthClass": "shallow", "architecture": "fibrous", "competitiveIntensity": "medium"},
        "canopy": {"density": "dense", "transmittance": 0.2},
        "phenology": {"emergenceMonths": [3, 4], "senescenceMonths": [10], "leafOnMonths": []},
        "lifecycle": {"lifespan": "long", "maintenanceSensitivity": null},
        "regulatory": {"status": "allowed", "scope": "LT", "note": null},
        "evidence": [{"field": "ecology.ellenbergLight", "sourceId": "s1", "confidence": "high", "method": "dataset"},
                     {"field": "light", "sourceId": "s1"}]})"),
                                 c, problems))
        << (problems.Empty () ? "" : problems.items[0]);
    const PlantTraits& t = c.plants[0].traits;
    EXPECT_DOUBLE_EQ (*t[Trait::EllenbergLight].number, 4.0);
    EXPECT_DOUBLE_EQ (*t[Trait::PhMax].number, 7.5);
    EXPECT_STREQ (t.ChoiceKey (Trait::CompetitiveVigour), "high");
    EXPECT_STREQ (t.ChoiceKey (Trait::SelfSeeding), "none");
    EXPECT_EQ (t[Trait::Drainage].choices, (std::vector<int> { 0, 1 }));
    EXPECT_EQ (t[Trait::EmergenceMonths].months, (std::vector<int> { 3, 4 }));
    EXPECT_FALSE (t[Trait::LeafOnMonths].Given ());
    EXPECT_EQ (t.ChoiceKey (Trait::MaintenanceSensitivity), nullptr) << "null is not given";
    EXPECT_EQ (t[Trait::Hardiness].text, "USDA 4");
    ASSERT_EQ (t.evidence.size (), 2u);
    EXPECT_EQ (t.evidence[0].confidence, 2);
    EXPECT_EQ (t.evidence[1].field, "light") << "evidence may name the standard's own fields";
}

TEST (PlantTraits, EveryTraitProblemIsListed)
{
    Catalogue c;
    ParseProblems problems;
    EXPECT_FALSE (ParseCatalogue (WithPlants (R"({"id": "a", "scientificName": "Aa",
        "ecology": {"ellenbergLight": 10, "pHMin": 7, "pHMax": 6, "drainage": ["boggy"], "ellenbergMosture": 3},
        "competition": {"competitiveVigour": "huge", "spreadMode": "clump"},
        "canopy": "dense",
        "phenology": {"emergenceMonths": [0]},
        "evidence": [{"field": "colour", "sourceId": "s1"}, {"field": "light", "sourceId": "nowhere"},
                     {"field": "light", "sourceId": "s1", "confidence": "certain"}]})"),
                                  c, problems));
    EXPECT_TRUE (Has (problems, "ecology.ellenbergLight: must be 1 to 9"));
    EXPECT_TRUE (Has (problems, "ecology.pHMax: must not be below pHMin"));
    EXPECT_TRUE (Has (problems, "ecology.drainage[0]: 'boggy' is not freelyDrained"));
    EXPECT_TRUE (Has (problems, "ecology.ellenbergMosture: is not a known trait")) << "a typo is not silence";
    EXPECT_TRUE (Has (problems, "competition.competitiveVigour: 'huge' is not low, medium or high"));
    EXPECT_TRUE (Has (problems, "competition.spreadMode: must be a list"));
    EXPECT_TRUE (Has (problems, "canopy: must be an object"));
    EXPECT_TRUE (Has (problems, "phenology.emergenceMonths[0]: must be a month 1-12"));
    EXPECT_TRUE (Has (problems, "evidence[0]: field must name a trait path or a record field"));
    EXPECT_TRUE (Has (problems, "evidence[1]: sourceId must name an entry of sources"));
    EXPECT_TRUE (Has (problems, "evidence[2]: confidence must be low, medium or high"));
}

TEST (PlantCatalogue, SyntaxErrorsAndVersionsAreActionable)
{
    Catalogue c;
    ParseProblems problems;
    EXPECT_FALSE (ParseCatalogue ("{\n  \"schemaVersion\": 1,\n  \"plants\": [ oops ]\n}", c, problems));
    ASSERT_FALSE (problems.Empty ());
    EXPECT_TRUE (Has (problems, "line 3, column")) << problems.items[0];
    EXPECT_FALSE (ParseCatalogue (R"({"schemaVersion": 2, "plants": []})", c, problems));
    EXPECT_TRUE (Has (problems, "schemaVersion 2 is not supported"));
    std::vector<Profile> profiles;
    EXPECT_FALSE (ParseProfiles (
        R"({"schemaVersion": 1, "profiles": [{"id": "x", "name": "X", "classEdgesH": {"partial": 6, "full": 3},
           "season": {"from": "2/30", "to": "10/1"}}]})",
        profiles, problems));
    EXPECT_TRUE (Has (problems, "needs 0 < partial < full <= 24"));
    EXPECT_TRUE (Has (problems, "from and to must be month/day"));
}

TEST (PlantCatalogue, AProfileSetsEdgesSeasonAndHeights)
{
    std::vector<Profile> profiles;
    ParseProblems problems;
    ASSERT_TRUE (ParseProfiles (
        R"({"schemaVersion": 1, "profiles": [{"id": "x", "name": "X", "classEdgesH": {"partial": 2, "full": 5,
           "deepShade": 1}, "season": {"from": "5/1", "to": "9/15"}, "receiverHeightsM": [0.3, 1.2]}]})",
        profiles, problems))
        << (problems.Empty () ? "" : problems.items[0]);
    PlantingHud hud;
    ApplyProfile (hud, profiles[0]);
    EXPECT_FLOAT_EQ (hud.partialEdge, 2.0f);
    EXPECT_FLOAT_EQ (hud.sunEdge, 5.0f);
    EXPECT_TRUE (hud.deepShade);
    EXPECT_EQ (hud.season, kCustom);
    EXPECT_EQ (hud.custom.toDay, 15);
    EXPECT_EQ (ChosenHeights (hud), (std::vector<float> { 0.3f, 1.2f }));
    EXPECT_EQ (hud.profileId, "x");
}

TEST (PlantCatalogue, TheFitReadsTheNearestMeasuredLayer)
{
    PlantingHud hud;
    Plant shrub;
    shrub.habit = Habit::Shrub;
    EXPECT_EQ (FitOf (hud, shrub).reason, "no light classes in the catalogue");
    shrub.light = { false, true, true }; // partial, full
    EXPECT_EQ (FitOf (hud, shrub).status, PlantFit::Status::Unknown);
    KeepLayer (hud, 0.15f, "s1", true);
    KeepLayer (hud, 1.5f, "s2", true);
    const auto edges = AreaEdges (hud);
    KeepLayerAreas (hud, "s1", edges, { 10.0, 0.0, 0.0 }, 10.0);
    KeepLayerAreas (hud, "s2", edges, { 2.0, 3.0, 5.0 }, 10.0);
    const PlantFit fit = FitOf (hud, shrub);
    EXPECT_EQ (fit.status, PlantFit::Status::Fits);
    EXPECT_FLOAT_EQ (fit.layerHeight, 1.5f) << "a shrub reads the 1.5 m layer";
    EXPECT_DOUBLE_EQ (fit.compatibleArea, 8.0);
    EXPECT_DOUBLE_EQ (fit.Share (), 0.8);
    Plant ground;
    ground.habit = Habit::Groundcover;
    ground.light = { false, false, true };
    EXPECT_EQ (FitOf (hud, ground).status, PlantFit::Status::None) << "all shade at 0.15 m";
    hud.sunEdge = 7.0f; // other edges: the areas must be read again
    EXPECT_EQ (FitOf (hud, shrub).reason, "no layer read at these class edges");
    KeepLayer (hud, 1.5f, "s3", true);
    EXPECT_TRUE (hud.layers[1].classAreas.empty ()) << "another study forgets the old areas";
}

TEST (PlantCatalogue, DeepShadeCountsAsShadeForTheHighlight)
{
    PlantingHud hud;
    Plant p;
    p.light = { true, false, false };
    EXPECT_EQ (AcceptedClassMask (hud, p), 0b001u);
    hud.deepShade = true;
    EXPECT_EQ (AcceptedClassMask (hud, p), 0b011u);
    EXPECT_NE (DimmedRgba (0xF2C230FFu), 0xF2C230FFu);
    EXPECT_EQ (DimmedRgba (0xF2C230FFu) & 0xFFu, 0x80u);
}

TEST (PlantCatalogue, SearchAndFilters)
{
    Catalogue c;
    ParseProblems problems;
    ASSERT_TRUE (ParseCatalogue (SeedPlantsJson (), c, problems));
    auto count = [&] (const PlantFilter& f) {
        return std::count_if (c.plants.begin (), c.plants.end (), [&] (const Plant& p) { return Matches (p, f); });
    };
    EXPECT_EQ (count ({}), 269);
    EXPECT_EQ (count ({ "klevas", -1, -1 }), 7) << "a local name";
    EXPECT_EQ (count ({ "HOSTA", -1, -1 }), 3);
    EXPECT_EQ (count ({ "", int (Habit::Shrub), -1 }), 46);
    EXPECT_EQ (count ({ "", -1, int (LightClass::Shade) }), 19);

    // Lithuanian capitals fold: the names start with Š, the search is lower case.
    EXPECT_EQ (count ({ "švedinis", -1, -1 }), 2);
    EXPECT_EQ (count ({ "ŠVEDINIS", -1, -1 }), 2);
    EXPECT_EQ (count ({ "žilasis česnakas", -1, -1 }), 1);

    // The standard's columns; a blank cell is not a match.
    PlantFilter f;
    f.section = int (PlantSection::Trees);
    EXPECT_EQ (count (f), 114);
    f = {};
    f.site = int (Site::OnSlabs);
    EXPECT_EQ (count (f), 91);
    f = {};
    f.moisture = int (MoistureClass::Wet);
    EXPECT_EQ (count (f), 3);
    f = {};
    f.breeamOnly = true;
    EXPECT_EQ (count (f), 70);
    f.section = int (PlantSection::Shrubs);
    f.site = int (Site::Streets);
    EXPECT_EQ (count (f), 3) << "BREEAM shrubs recommended for streets: filters combine";
    EXPECT_TRUE (f.Any ());
    EXPECT_FALSE (PlantFilter {}.Any ());
}

TEST (PlantCatalogue, APointsClassIsTheCataloguesLightClass)
{
    PlantingHud hud;
    hud.deepShade = false;
    EXPECT_EQ (LightOfClass (hud, 0), LightClass::Shade);
    EXPECT_EQ (LightOfClass (hud, 1), LightClass::Partial);
    EXPECT_EQ (LightOfClass (hud, 2), LightClass::Full);
    hud.deepShade = true;
    EXPECT_EQ (LightOfClass (hud, 0), LightClass::Shade) << "deep shade counts as shade";
    EXPECT_EQ (LightOfClass (hud, 1), LightClass::Shade);
    EXPECT_EQ (LightOfClass (hud, 2), LightClass::Partial);
    EXPECT_EQ (LightOfClass (hud, 3), LightClass::Full);
}

TEST (PlantCatalogue, ACoverLayerPerennialReadsAtGroundCover)
{
    Plant p;
    p.habit = Habit::Perennial;
    EXPECT_FLOAT_EQ (ReceiverHeightOf (p), 0.5f) << "no layer: the perennial tier";
    p.plantingLayers = { PlantingLayer::Cover };
    EXPECT_FLOAT_EQ (ReceiverHeightOf (p), 0.15f);
    p.plantingLayers = { PlantingLayer::Cover, PlantingLayer::Seasonal };
    EXPECT_FLOAT_EQ (ReceiverHeightOf (p), 0.5f) << "also seasonal: the perennial tier";
    p.habit = Habit::Shrub;
    p.plantingLayers = { PlantingLayer::Cover };
    EXPECT_FLOAT_EQ (ReceiverHeightOf (p), 1.5f) << "the habit tier wins for shrubs";
    p.receiverHeightM = 0.3;
    EXPECT_FLOAT_EQ (ReceiverHeightOf (p), 0.3f) << "a stated height wins";
}

TEST (PlantingFiles, SeedingNeverOverwritesAndRestoreBacksUp)
{
    TempFolder temp;
    std::vector<std::string> created;
    std::string error;
    ASSERT_TRUE (SeedCatalogueFolder (temp.path, created, error)) << error;
    EXPECT_EQ (created.size (), 2u);
    Write (temp.path / kPlantsFile, WithPlants (R"({"id": "mine", "scientificName": "Mine"})"));
    ASSERT_TRUE (SeedCatalogueFolder (temp.path, created, error));
    EXPECT_TRUE (created.empty ());
    EXPECT_NE (Read (temp.path / kPlantsFile).find ("mine"), std::string::npos) << "the user's file is kept";
    CatalogueState state;
    ReadCatalogueFolder (temp.path, state);
    ASSERT_TRUE (state.problems.empty ()) << state.problems[0];
    EXPECT_EQ (state.plants.plants.size (), 1u);
    EXPECT_EQ (state.status, "Read 1 plant and 1 profile.");

    // An invalid edit: problems are listed and the previous catalogue stays.
    Write (temp.path / kPlantsFile, "{ \"schemaVersion\": 1, \"plants\": [ { } ] ");
    ReadCatalogueFolder (temp.path, state);
    EXPECT_FALSE (state.problems.empty ());
    EXPECT_EQ (state.plants.plants.size (), 1u);
    EXPECT_NE (state.status.find ("previous catalogue stays in use"), std::string::npos);

    const std::string edited = Read (temp.path / kPlantsFile);
    std::string backup;
    ASSERT_TRUE (RestoreCatalogueDefaults (temp.path, backup, error)) << error;
    fs::path copy = temp.path / "plants";
    copy += backup;
    ASSERT_TRUE (fs::exists (copy)) << copy.string ();
    EXPECT_EQ (Read (copy), edited) << "the backup is the user's file";
    EXPECT_EQ (Read (temp.path / kPlantsFile), SeedPlantsJson ());
}

TEST (PlantingFiles, WeatherFilesAreListedByName)
{
    TempFolder temp;
    std::string error;
    EXPECT_TRUE (ListWeatherFiles (temp.path, error).empty ());
    EXPECT_TRUE (fs::exists (temp.path)) << "the folder is created";
    Write (temp.path / "b.EPW", "x");
    Write (temp.path / "a.epw", "x");
    Write (temp.path / "notes.txt", "x");
    EXPECT_EQ (ListWeatherFiles (temp.path, error), (std::vector<std::string> { "a.epw", "b.EPW" }));
}

TEST (PlantingHudModel, TheWeatherSeasonBecomesFormDates)
{
    std::vector<evp::sunstudy::WeatherDay> days (365);
    days[110] = { 4, 21, 6.0, 0, 0 };
    days[290] = { 10, 18, 6.0, 0, 0 };
    SeasonDates dates;
    ASSERT_TRUE (SeasonDatesFromWeather (days, { true, 110, 290 }, dates));
    EXPECT_EQ (dates.fromMonth, 4);
    EXPECT_EQ (dates.fromDay, 21);
    EXPECT_EQ (dates.toMonth, 10);
    EXPECT_EQ (dates.toDay, 18);
    EXPECT_FALSE (SeasonDatesFromWeather (days, { true, 290, 110 }, dates)) << "over New Year";
    EXPECT_FALSE (SeasonDatesFromWeather (days, { false, 0, 0 }, dates));
    EXPECT_EQ (MatchStation (4000, 10), StationMatch::Near);
    EXPECT_EQ (MatchStation (95000, 10), StationMatch::Warn);
    EXPECT_EQ (MatchStation (4000, 400), StationMatch::Far);
}
