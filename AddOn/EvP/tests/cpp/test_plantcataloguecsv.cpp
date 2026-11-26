// The plant catalogue as CSV (#278 Stage 3): export, RFC 4180 parsing, import
// through the JSON validator, the files' backup, and quantity estimates.

#include "ArchViz/PlantCatalogueCsv.hpp"
#include "ArchViz/PlantingFiles.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

using namespace geomsrv::archviz::planting;
namespace fs = std::filesystem;

namespace {

Catalogue Seed ()
{
    Catalogue c;
    ParseProblems problems;
    EXPECT_TRUE (ParseCatalogue (SeedPlantsJson (), c, problems));
    return c;
}

bool Has (const ParseProblems& p, const std::string& text)
{
    for (const auto& item : p.items)
        if (item.find (text) != std::string::npos)
            return true;
    return false;
}

std::string Header ()
{
    std::string h;
    for (const auto& c : CatalogueCsvColumns ())
        h += (h.empty () ? "" : ",") + c;
    return h + "\n";
}

} // namespace

TEST (PlantCatalogueCsv, QuotedFieldsSurvive)
{
    std::vector<std::vector<std::string>> rows;
    std::string error;
    ASSERT_TRUE (ParseCsv ("a,\"b, with comma\",\"say \"\"hi\"\"\"\r\n\"two\nlines\",,x\n\n", rows, error));
    ASSERT_EQ (rows.size (), 2u);
    EXPECT_EQ (rows[0], (std::vector<std::string> { "a", "b, with comma", "say \"hi\"" }));
    EXPECT_EQ (rows[1], (std::vector<std::string> { "two\nlines", "", "x" }));
    EXPECT_FALSE (ParseCsv ("a,\"open\nb\n", rows, error));
    EXPECT_EQ (error, "line 1: a quoted field is not closed");
}

TEST (PlantCatalogueCsv, TheSeedRoundTripsThroughCsvAndJson)
{
    const Catalogue seed = Seed ();
    const std::string csv = CatalogueToCsv (seed);
    EXPECT_EQ (csv.substr (0, 3), "\xEF\xBB\xBF") << "a byte-order mark, for spreadsheets";
    EXPECT_NE (csv.find ("Siboldo melsvė"), std::string::npos);
    Catalogue back;
    ParseProblems problems;
    ASSERT_TRUE (CatalogueFromCsv (csv, seed, back, problems)) << (problems.Empty () ? "" : problems.items[0]);
    ASSERT_EQ (back.plants.size (), seed.plants.size ());
    for (size_t i = 0; i < seed.plants.size (); ++i) {
        EXPECT_EQ (back.plants[i].id, seed.plants[i].id);
        EXPECT_EQ (back.plants[i].light, seed.plants[i].light);
        EXPECT_EQ (back.plants[i].localNames, seed.plants[i].localNames);
        EXPECT_EQ (back.plants[i].habit, seed.plants[i].habit);
        EXPECT_EQ (back.plants[i].deciduous, seed.plants[i].deciduous);
    }
    EXPECT_EQ (back.sources[0].note, seed.sources[0].note) << "a known source keeps its title and note";
    Catalogue again;
    ASSERT_TRUE (ParseCatalogue (CatalogueToJson (back), again, problems));
    EXPECT_EQ (again.plants.size (), seed.plants.size ());
}

TEST (PlantCatalogueCsv, RangesDensityAndMonthsImport)
{
    const std::string csv = Header () + "astilbe-x,Astilbe x arendsii,Fanal,Astilbė,,perennial,shade|partial,0.4,0.6,"
                                        "0.3,0.5,,7,9,7|8,true,moist,Str,vilnius,24\n";
    Catalogue c;
    ParseProblems problems;
    ASSERT_TRUE (CatalogueFromCsv (csv, Catalogue {}, c, problems)) << (problems.Empty () ? "" : problems.items[0]);
    const Plant& p = c.plants[0];
    EXPECT_EQ (p.DisplayName (), "Astilbe x arendsii 'Fanal'");
    EXPECT_DOUBLE_EQ (p.matureHeightM->max, 0.6);
    EXPECT_DOUBLE_EQ (p.densityPerM2->min, 7.0);
    EXPECT_EQ (p.floweringMonths, (std::vector<int> { 7, 8 }));
    EXPECT_EQ (p.plantingLayers, (std::vector<std::string> { "Str" }));
    EXPECT_EQ (p.sources[0].page, 24);
    EXPECT_EQ (c.sources[0].id, "vilnius") << "a source named only in the CSV is added";
}

TEST (PlantCatalogueCsv, ProblemsNameTheRowAndNothingIsImported)
{
    Catalogue c;
    ParseProblems problems;
    EXPECT_FALSE (CatalogueFromCsv ("id,scientificName,Light\nx,X,full\n", Catalogue {}, c, problems));
    EXPECT_TRUE (Has (problems, "unknown column 'Light'")) << "a misspelt header is not dropped silently";
    EXPECT_FALSE (CatalogueFromCsv ("id,light\nx,full\n", Catalogue {}, c, problems));
    EXPECT_TRUE (Has (problems, "column 'scientificName' is required"));
    EXPECT_FALSE (CatalogueFromCsv (Header () + "a,A,,,,bush,sunny,1,,,,,,,13,maybe,,,,\n", Catalogue {}, c, problems));
    EXPECT_TRUE (Has (problems, "CSV row 2: habit: 'bush'"));
    EXPECT_TRUE (Has (problems, "CSV row 2: light: 'sunny'"));
    EXPECT_TRUE (Has (problems, "matureHeightMinM and matureHeightMaxM are given together"));
    EXPECT_TRUE (Has (problems, "deciduous: 'maybe'"));
    // Rules only the JSON reader knows still apply: a month 13 passes the CSV
    // step and is refused by ParseCatalogue.
    EXPECT_FALSE (CatalogueFromCsv (Header () + "a,A,,,,,full,,,,,,,,13,,,,,\n", Catalogue {}, c, problems));
    EXPECT_TRUE (Has (problems, "floweringMonths[0]: must be a month 1-12"));
    EXPECT_TRUE (c.plants.empty ());
}

TEST (PlantCatalogueCsv, QuantityNeedsASourcedDensity)
{
    Plant p;
    PlantFit fit;
    fit.status = PlantFit::Status::Fits;
    fit.compatibleArea = 12.3;
    EXPECT_FALSE (PlantQuantity (p, fit).has_value ()) << "no density, no default";
    p.densityPerM2 = Range { 5, 7 };
    const auto q = PlantQuantity (p, fit);
    ASSERT_TRUE (q.has_value ());
    EXPECT_DOUBLE_EQ (q->min, 62.0);
    EXPECT_DOUBLE_EQ (q->max, 87.0);
    fit.status = PlantFit::Status::None;
    EXPECT_FALSE (PlantQuantity (p, fit).has_value ());
}

TEST (PlantingFiles, ImportBacksUpAndARefusedCsvChangesNothing)
{
    const fs::path folder = fs::temp_directory_path () / "tapioca-catalogue-csv-import";
    std::error_code ec;
    fs::remove_all (folder, ec);
    std::vector<std::string> created;
    std::string error;
    ASSERT_TRUE (SeedCatalogueFolder (folder, created, error)) << error;
    CatalogueState state;
    ReadCatalogueFolder (folder, state);
    ASSERT_EQ (state.plants.plants.size (), 8u);
    std::string backup;
    EXPECT_FALSE (ImportCatalogueCsv (folder, state, backup)) << "no plants.csv yet";
    ASSERT_TRUE (ExportCatalogueCsv (folder, state.plants, error)) << error;
    EXPECT_TRUE (fs::exists (folder / kCsvExportFile));

    std::ofstream (folder / kCsvImportFile, std::ios::binary) << "id,scientificName,light\nx,X,sunny\n";
    EXPECT_FALSE (ImportCatalogueCsv (folder, state, backup));
    EXPECT_EQ (state.plants.plants.size (), 8u);
    EXPECT_FALSE (state.problems.empty ());

    std::ofstream (folder / kCsvImportFile, std::ios::binary) << "id,scientificName,light\nx,Xus yus,full\n";
    ASSERT_TRUE (ImportCatalogueCsv (folder, state, backup)) << (state.problems.empty () ? "" : state.problems[0]);
    EXPECT_EQ (state.plants.plants.size (), 1u);
    fs::path copy = folder / "plants";
    copy += backup;
    EXPECT_TRUE (fs::exists (copy)) << "the previous plants.json is kept";
    fs::remove_all (folder, ec);
}
