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
    EXPECT_NE (csv.find ("Korėjinis kėnis"), std::string::npos);
    Catalogue back;
    ParseProblems problems;
    ASSERT_TRUE (CatalogueFromCsv (csv, seed, back, problems)) << (problems.Empty () ? "" : problems.items[0]);
    ASSERT_EQ (back.plants.size (), seed.plants.size ());
    for (size_t i = 0; i < seed.plants.size (); ++i) {
        const Plant& a = seed.plants[i];
        const Plant& b = back.plants[i];
        EXPECT_EQ (b.id, a.id);
        EXPECT_EQ (b.light, a.light);
        EXPECT_EQ (b.localNames, a.localNames);
        EXPECT_EQ (b.habit, a.habit);
        EXPECT_EQ (b.deciduous, a.deciduous);
        EXPECT_EQ (b.section, a.section) << a.id;
        EXPECT_EQ (b.sourceNumber, a.sourceNumber) << a.id;
        EXPECT_EQ (b.breeam, a.breeam) << a.id;
        EXPECT_EQ (b.ornamentalFlowering, a.ornamentalFlowering) << a.id;
        EXPECT_EQ (b.ornithological, a.ornithological) << a.id;
        EXPECT_EQ (b.saltTolerance, a.saltTolerance) << a.id;
        EXPECT_EQ (b.growthIntensity, a.growthIntensity) << a.id;
        EXPECT_EQ (b.moistureClasses, a.moistureClasses) << a.id;
        EXPECT_EQ (b.plantingLayers, a.plantingLayers) << a.id;
        EXPECT_EQ (b.soilTypes, a.soilTypes) << a.id;
        EXPECT_EQ (b.siteSuitable, a.siteSuitable) << a.id;
        EXPECT_EQ (b.siteSoil, a.siteSoil) << a.id;
        ASSERT_EQ (a.matureHeightM.has_value (), b.matureHeightM.has_value ()) << a.id;
        if (a.matureHeightM) {
            EXPECT_EQ (b.matureHeightM->openMax, a.matureHeightM->openMax) << a.id;
            EXPECT_EQ (b.matureHeightM->openMin, a.matureHeightM->openMin) << a.id;
        }
    }
    EXPECT_EQ (back.sources[0].note, seed.sources[0].note) << "a known source keeps its title and note";
    Catalogue again;
    ASSERT_TRUE (ParseCatalogue (CatalogueToJson (back), again, problems));
    EXPECT_EQ (again.plants.size (), seed.plants.size ());
}

TEST (PlantCatalogueCsv, RangesDensityAndMonthsImport)
{
    const std::string csv =
        Header () + "astilbe-x,Astilbe x arendsii,Fanal,Astilbė,,perennials,7,perennial,true,,,shade|partial,low,slow,"
                    "dry|moist,D|Str,0.4,0.6,0.3,0.5,,7,9,7|8,true,A|B,greenStrip|onSlabs,greenStrip:B|onSlabs:D,"
                    "vilnius,24\n";
    Catalogue c;
    ParseProblems problems;
    ASSERT_TRUE (CatalogueFromCsv (csv, Catalogue {}, c, problems)) << (problems.Empty () ? "" : problems.items[0]);
    const Plant& p = c.plants[0];
    EXPECT_EQ (p.DisplayName (), "Astilbe x arendsii 'Fanal'");
    EXPECT_EQ (p.section, PlantSection::Perennials);
    EXPECT_EQ (p.sourceNumber, "7");
    EXPECT_TRUE (p.breeam);
    EXPECT_EQ (p.saltTolerance, SaltTolerance::Low);
    EXPECT_EQ (p.growthIntensity, GrowthIntensity::Slow);
    EXPECT_EQ (p.moistureClasses, (std::vector<MoistureClass> { MoistureClass::Dry, MoistureClass::Moist }));
    EXPECT_DOUBLE_EQ (p.matureHeightM->max, 0.6);
    EXPECT_DOUBLE_EQ (p.densityPerM2->min, 7.0);
    EXPECT_EQ (p.floweringMonths, (std::vector<int> { 7, 8 }));
    EXPECT_EQ (p.plantingLayers, (std::vector<PlantingLayer> { PlantingLayer::Cover, PlantingLayer::Structural }));
    EXPECT_EQ (p.soilTypes, (std::vector<std::string> { "A", "B" }));
    EXPECT_TRUE (p.siteSuitable[size_t (Site::GreenStrip)]);
    EXPECT_EQ (p.siteSoil[size_t (Site::OnSlabs)], "D");
    EXPECT_EQ (p.sources[0].page, 24);
    EXPECT_EQ (c.sources[0].id, "vilnius") << "a source named only in the CSV is added";
}

TEST (PlantCatalogueCsv, TraitsRoundTripAndAreChecked)
{
    Catalogue base;
    base.sources.push_back ({ "ellenberg", "Ellenberg values", "", "", 2023 });
    Catalogue c;
    ParseProblems problems;
    ASSERT_TRUE (CatalogueFromCsv ("id,scientificName,ecology.ellenbergLight,ecology.drainage,"
                                   "competition.competitiveVigour,phenology.emergenceMonths,regulatory.note,evidence\n"
                                   "a,Aa,4,freelyDrained|poorlyDrained,high,3|4,check locally,"
                                   "ecology.ellenbergLight:ellenberg:high:dataset\n",
                                   base, c, problems))
        << (problems.Empty () ? "" : problems.items[0]);
    const PlantTraits& t = c.plants[0].traits;
    EXPECT_DOUBLE_EQ (*t[Trait::EllenbergLight].number, 4.0);
    EXPECT_EQ (t[Trait::Drainage].choices, (std::vector<int> { 0, 2 }));
    EXPECT_STREQ (t.ChoiceKey (Trait::CompetitiveVigour), "high");
    EXPECT_EQ (t[Trait::RegulatoryNote].text, "check locally");
    ASSERT_EQ (t.evidence.size (), 1u);
    EXPECT_EQ (c.sources.size (), 1u) << "a source named only by evidence keeps its entry";

    // Export and import again: every trait survives, so an import never wipes them.
    Catalogue again;
    ASSERT_TRUE (CatalogueFromCsv (CatalogueToCsv (c), c, again, problems))
        << (problems.Empty () ? "" : problems.items[0]);
    for (int i = 0; i < kTraitCount; ++i)
        EXPECT_EQ (TraitCsvCell (again.plants[0].traits, Trait (i)), TraitCsvCell (t, Trait (i))) << i;
    EXPECT_EQ (EvidenceCsvCell (again.plants[0].traits), EvidenceCsvCell (t));

    EXPECT_FALSE (CatalogueFromCsv ("id,scientificName,competition.competitiveVigour,ecology.ellenbergLight,evidence\n"
                                    "a,Aa,huge,12,light\n",
                                    base, c, problems));
    EXPECT_TRUE (Has (problems, "CSV row 2: competition.competitiveVigour: 'huge' is not low, medium or high"));
    EXPECT_TRUE (Has (problems, "CSV row 2: evidence: 'light' is not field:sourceId"));
    EXPECT_FALSE (CatalogueFromCsv ("id,scientificName,ecology.ellenbergLight\na,Aa,12\n", base, c, problems));
    EXPECT_TRUE (Has (problems, "ecology.ellenbergLight: must be 1 to 9")) << "the range comes from the JSON reader";
}

TEST (PlantCatalogueCsv, OneSidedRangesAreOpen)
{
    Catalogue c;
    ParseProblems problems;
    ASSERT_TRUE (CatalogueFromCsv ("id,scientificName,matureHeightMinM,matureHeightMaxM\n"
                                   "a,Tall,16,\n"
                                   "b,Low,,8\n",
                                   Catalogue {}, c, problems))
        << (problems.Empty () ? "" : problems.items[0]);
    ASSERT_EQ (c.plants.size (), 2u);
    EXPECT_TRUE (c.plants[0].matureHeightM->openMax);
    EXPECT_DOUBLE_EQ (c.plants[0].matureHeightM->min, 16.0);
    EXPECT_TRUE (c.plants[1].matureHeightM->openMin);
    EXPECT_DOUBLE_EQ (c.plants[1].matureHeightM->max, 8.0);
}

TEST (PlantCatalogueCsv, ProblemsNameTheRowAndNothingIsImported)
{
    Catalogue c;
    ParseProblems problems;
    EXPECT_FALSE (CatalogueFromCsv ("id,scientificName,Light\nx,X,full\n", Catalogue {}, c, problems));
    EXPECT_TRUE (Has (problems, "unknown column 'Light'")) << "a misspelt header is not dropped silently";
    EXPECT_FALSE (CatalogueFromCsv ("id,light\nx,full\n", Catalogue {}, c, problems));
    EXPECT_TRUE (Has (problems, "column 'scientificName' is required"));
    EXPECT_FALSE (CatalogueFromCsv ("id,scientificName,habit,light,section,moistureClasses,plantingLayers,sites\n"
                                    "a,A,bush,sunny,vines,damp,X,moon\n",
                                    Catalogue {}, c, problems));
    EXPECT_TRUE (Has (problems, "CSV row 2: habit: 'bush'"));
    EXPECT_TRUE (Has (problems, "CSV row 2: light: 'sunny'"));
    EXPECT_TRUE (Has (problems, "CSV row 2: section: 'vines'"));
    EXPECT_TRUE (Has (problems, "CSV row 2: moistureClasses: 'damp'"));
    EXPECT_TRUE (Has (problems, "CSV row 2: plantingLayers: 'X'"));
    EXPECT_TRUE (Has (problems, "CSV row 2: sites: 'moon'"));
    // Rules only the JSON reader knows still apply: a month 13 passes the CSV
    // step and is refused by ParseCatalogue.
    EXPECT_FALSE (
        CatalogueFromCsv ("id,scientificName,light,floweringMonths\na,A,full,13\n", Catalogue {}, c, problems));
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
    ASSERT_EQ (state.plants.plants.size (), 269u);
    std::string backup;
    EXPECT_FALSE (ImportCatalogueCsv (folder, state, backup)) << "no plants.csv yet";
    ASSERT_TRUE (ExportCatalogueCsv (folder, state.plants, error)) << error;
    EXPECT_TRUE (fs::exists (folder / kCsvExportFile));

    std::ofstream (folder / kCsvImportFile, std::ios::binary) << "id,scientificName,light\nx,X,sunny\n";
    EXPECT_FALSE (ImportCatalogueCsv (folder, state, backup));
    EXPECT_EQ (state.plants.plants.size (), 269u);
    EXPECT_FALSE (state.problems.empty ());

    std::ofstream (folder / kCsvImportFile, std::ios::binary) << "id,scientificName,light\nx,Xus yus,full\n";
    ASSERT_TRUE (ImportCatalogueCsv (folder, state, backup)) << (state.problems.empty () ? "" : state.problems[0]);
    EXPECT_EQ (state.plants.plants.size (), 1u);
    fs::path copy = folder / "plants";
    copy += backup;
    EXPECT_TRUE (fs::exists (copy)) << "the previous plants.json is kept";
    fs::remove_all (folder, ec);
}
