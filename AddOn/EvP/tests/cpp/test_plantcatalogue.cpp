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
    ASSERT_EQ (c.plants.size (), 8u);
    EXPECT_EQ (c.plants[0].localNames.at ("lt"), "Siboldo melsvė");
    EXPECT_EQ (c.plants[5].localNames.at ("lt"), "Aitrusis šilokas");
    EXPECT_FALSE (c.plants[0].matureHeightM.has_value ()) << "the example invents no heights";
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
        "habit": "shrub", "light": {"acceptedClasses": ["partial", "full"]},
        "matureHeightM": {"min": 1, "max": 1.5}, "matureSpreadM": null, "preferredReceiverHeightM": 0.8,
        "floweringMonths": [6, 7], "deciduous": true, "sourceRefs": [{"sourceId": "s1", "page": 20}],
        "somethingNew": 3})"),
                                 c, problems))
        << (problems.Empty () ? "" : problems.items[0]);
    const Plant& p = c.plants[0];
    EXPECT_EQ (p.DisplayName (), "Aa bb 'Gold'");
    EXPECT_EQ (p.habit, Habit::Shrub);
    EXPECT_FALSE (p.light[0]);
    EXPECT_TRUE (p.light[1] && p.light[2]);
    EXPECT_DOUBLE_EQ (p.matureHeightM->max, 1.5);
    EXPECT_FALSE (p.matureSpreadM.has_value ());
    EXPECT_FLOAT_EQ (ReceiverHeightOf (p), 0.8f);
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
         "sourceRefs": [{"sourceId": "nowhere"}]},
        {"scientificName": "D"})"),
                        c, problems));
    EXPECT_EQ (c.catalogueId, "previous") << "a refused file changes nothing";
    EXPECT_TRUE (Has (problems, "plants[0] 'a': light.acceptedClasses[0]: 'sunny' is not shade, partial or full"));
    EXPECT_TRUE (Has (problems, "plants[1] 'a': id: is used twice"));
    EXPECT_TRUE (Has (problems, "'bush' is not one of"));
    EXPECT_TRUE (Has (problems, "plants[2] 'c': matureHeightM: needs 0 <= min <= max"));
    EXPECT_TRUE (Has (problems, "floweringMonths[0]: must be a month 1-12"));
    EXPECT_TRUE (Has (problems, "sourceId must name an entry of sources"));
    EXPECT_TRUE (Has (problems, "plants[3] id: is required"));
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
    EXPECT_EQ (count ({}), 8);
    EXPECT_EQ (count ({ "levanda", -1, -1 }), 1) << "a local name";
    EXPECT_EQ (count ({ "HOSTA", -1, -1 }), 1);
    EXPECT_EQ (count ({ "", int (Habit::Shrub), -1 }), 3);
    EXPECT_EQ (count ({ "", -1, int (LightClass::Shade) }), 4);
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
