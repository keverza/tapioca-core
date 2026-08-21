// ArchViz/BuildingMaterialSignal — run against TWO real projects, because the
// central claim cannot be tested against one.
//
// ⚠️ WHY TWO FIXTURES. The design rests on two measured facts that a single
// project is incapable of showing:
//   * connectionPriority is a STANDARD scale, not this user's convention. Only
//     a second, unrelated project can demonstrate that -- and it does: 58 of the
//     59 material names present in both carry an identical priority.
//   * cutFillId is NOT transferable. The two projects share no fill guid at all,
//     which is why the fill->substance map is derived per project instead of
//     tabulated.
//
// ⚠️ NAMES ARE READ HERE AND NEVER IN THE CLASSIFIER. `BuildingMaterialRow` has
// no name field at all, so the shipped code cannot read one even by accident.
// This file reads names to check the verdicts, which is the one legitimate use:
// it runs offline, decides nothing at runtime and ships nowhere.

#include "ArchViz/BuildingMaterialSignal.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace geomsrv::archviz;

namespace {

struct DumpedMaterial {
    std::string name;
    BuildingMaterialRow row;
};

std::vector<std::string> SplitTabs (const std::string& line)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= line.size (); ++i) {
        if (i == line.size () || line[i] == '\t') {
            out.push_back (line.substr (start, i - start));
            start = i + 1;
        }
    }
    return out;
}

// Skips rather than fails when the fixture is absent -- the same contract
// test_environmentlighting.cpp uses for the sky fixture.
std::vector<DumpedMaterial> LoadDump (const char* which)
{
    std::vector<DumpedMaterial> rows;
#ifdef EVP_TEST_FIXTURE_DIR
    const std::string path = std::string (EVP_TEST_FIXTURE_DIR) + "/building_materials_20260821_" + which + ".tsv";
    std::FILE* file = std::fopen (path.c_str (), "rb");
    if (file == nullptr)
        return rows;

    std::string line;
    int c = 0;
    while ((c = std::fgetc (file)) != EOF) {
        if (c != '\n') {
            if (c != '\r')
                line.push_back (char (c));
            continue;
        }
        // ⚠️ A COMMENT IS "# " -- HASH THEN SPACE. Testing line[0] alone ate
        // every row of the large project whose material is named
        // "## CONCEPT - ...", which is precisely the user-authored set this
        // fixture exists to exercise. Testing for "no tab" instead then let the
        // COLUMN-HEADER line through as data, which is worse: its cutFillId
        // field is the literal string "cutFillId" in both fixtures, so the two
        // projects appeared to share a fill guid. Every comment this fixture
        // writes begins "# "; no material name can.
        const bool comment = line.size () >= 2 && line[0] == '#' && line[1] == ' ';
        if (!line.empty () && !comment) {
            const std::vector<std::string> f = SplitTabs (line);
            if (f.size () >= 3) {
                DumpedMaterial m;
                m.name = f[0];
                m.row.connectionPriority = std::atoi (f[1].c_str ());
                m.row.cutFillId = f[2];
                rows.push_back (m);
            }
        }
        line.clear ();
    }
    std::fclose (file);
#endif
    return rows;
}

std::vector<BuildingMaterialRow> RowsOf (const std::vector<DumpedMaterial>& dumped)
{
    std::vector<BuildingMaterialRow> rows;
    for (const DumpedMaterial& m : dumped)
        rows.push_back (m.row);
    return rows;
}

// VALIDATION ONLY. Returns the substance a human reads off the name, or Unknown
// when the name carries no substance this file is prepared to judge.
Substance ExpectedFromName (const std::string& n)
{
    auto has = [&n] (const char* needle) { return n.find (needle) != std::string::npos; };

    if (has ("Steel") || has ("PLIENAS") || has ("Iron") || has ("GELE") || has ("Titanium Zinc") ||
        has ("Aluminium") || has ("ALIUMINIS") || has ("METAL"))
        return Substance::Metal;
    if (has ("Concrete") || has ("Betonas"))
        return Substance::Concrete;
    if (has ("Timber") || has ("Plywood") || has ("Fiberboard") || has ("Medis") || has ("WOOD"))
        return Substance::Wood;
    if (has ("Glass") || has ("GLASS") || has ("Stiklas"))
        return Substance::Glass;
    if (has ("Plastic"))
        return Substance::Plastic;
    if (has ("Soil") || has ("Gravel") || has ("Sand"))
        return Substance::Earth;
    return Substance::Unknown;
}

} // namespace

TEST (BuildingMaterialSignal, ConnectionPriorityIsAStandardScaleAcrossUnrelatedProjects)
{
    const std::vector<DumpedMaterial> small = LoadDump ("small");
    const std::vector<DumpedMaterial> large = LoadDump ("large");
    if (small.empty () || large.empty ())
        GTEST_SKIP () << "the building-material fixtures are not present";

    EXPECT_EQ (small.size (), 87u);
    EXPECT_EQ (large.size (), 70u);

    // ⚠️ THE CLAIM THE WHOLE FILE RESTS ON. If this ever fails, the stock
    // priority table is this user's convention rather than Archicad's, and
    // ClassifyBuildingMaterial has to be rebuilt on something else.
    int common = 0, identical = 0;
    for (const DumpedMaterial& a : small) {
        for (const DumpedMaterial& b : large) {
            if (a.name != b.name)
                continue;
            ++common;
            if (a.row.connectionPriority == b.row.connectionPriority)
                ++identical;
        }
    }
    EXPECT_EQ (common, 59);
    // 58, not 59: "Tile - Floor" is 230 in one project and 542 in the other.
    // That single edit is the proof the number is user-editable, and the reason
    // the fill has to confirm it.
    EXPECT_EQ (identical, 58);
}

TEST (BuildingMaterialSignal, CutFillIdsAreNotTransferableBetweenProjects)
{
    const std::vector<DumpedMaterial> small = LoadDump ("small");
    const std::vector<DumpedMaterial> large = LoadDump ("large");
    if (small.empty () || large.empty ())
        GTEST_SKIP () << "the building-material fixtures are not present";

    // ⚠️ WHY THE FILL MAP IS DERIVED AND NOT TABULATED. Attribute guids are
    // per-project. A hard-coded fill table would work on the project it was
    // written against and silently classify nothing anywhere else.
    int shared = 0;
    for (const DumpedMaterial& a : small)
        for (const DumpedMaterial& b : large)
            if (!a.row.cutFillId.empty () && a.row.cutFillId == b.row.cutFillId)
                ++shared;
    EXPECT_EQ (shared, 0);
}

TEST (BuildingMaterialSignal, NoConfidentVerdictIsWrongInEitherProject)
{
    // ⚠️ THE CHECK THAT MATTERS, and the reason the two signals are required to
    // agree. Every verdict at the confident tier must match what a human reads
    // off the name -- across 157 building materials in two projects.
    for (const char* which : { "small", "large" }) {
        const std::vector<DumpedMaterial> dumped = LoadDump (which);
        if (dumped.empty ())
            GTEST_SKIP () << "the building-material fixtures are not present";

        const std::map<std::string, Substance> fills = DeriveFillSubstances (RowsOf (dumped));
        int confident = 0;
        for (const DumpedMaterial& m : dumped) {
            const SubstanceVerdict v = ClassifyBuildingMaterial (m.row, fills);
            if (v.confidence < 0.90f)
                continue;
            ++confident;
            EXPECT_EQ (v.substance, ExpectedFromName (m.name))
                << which << ": " << m.name << " was called " << SubstanceName (v.substance);
        }
        // And it must actually classify things -- a function that refused
        // everything would pass the loop above and be useless.
        EXPECT_GE (confident, 20) << which;
    }
}

TEST (BuildingMaterialSignal, UserAuthoredMaterialsAreClassifiedWithoutReadingTheirNames)
{
    const std::vector<DumpedMaterial> dumped = LoadDump ("large");
    if (dumped.empty ())
        GTEST_SKIP () << "the building-material fixtures are not present";

    const std::map<std::string, Substance> fills = DeriveFillSubstances (RowsOf (dumped));
    auto verdictFor = [&] (const std::string& name) {
        for (const DumpedMaterial& m : dumped)
            if (m.name == name)
                return ClassifyBuildingMaterial (m.row, fills);
        ADD_FAILURE () << "fixture is missing " << name;
        return SubstanceVerdict {};
    };

    // ⚠️ THE PAYOFF. These four are the project author's own materials, not
    // Archicad stock. Three are identified correctly from a priority and a
    // hatch; the fourth is refused. No name was consulted for any of them --
    // which matters most for "## CONCEPT - MARBLE", whose name would have been
    // the only way to notice it is not wood.
    EXPECT_EQ (verdictFor ("## CONCEPT - METAL").substance, Substance::Metal);
    EXPECT_EQ (verdictFor ("## CONCEPT - WOOD").substance, Substance::Wood);
    EXPECT_EQ (verdictFor ("## CONCEPT - GLASS").substance, Substance::Glass);

    // Authored at 350, which is Plywood's slot -- so the priority says wood. Its
    // hatch is the stone fill, so the two disagree and it is refused instead.
    const SubstanceVerdict marble = verdictFor ("## CONCEPT - MARBLE");
    EXPECT_LT (marble.confidence, 0.90f) << "marble must never reach the confident tier";
}

TEST (BuildingMaterialSignal, EitherSignalAloneIsConfidentlyWrongSomewhere)
{
    // ⚠️ THE NEGATIVE TEST FOR THE DESIGN ITSELF. It is not enough that the
    // combined rule works; the record has to show that the two cheaper rules
    // that anyone would try first do NOT, or someone will simplify this away.
    const std::vector<DumpedMaterial> dumped = LoadDump ("small");
    if (dumped.empty ())
        GTEST_SKIP () << "the building-material fixtures are not present";

    const std::map<std::string, Substance> fills = DeriveFillSubstances (RowsOf (dumped));

    // FILL ALONE: the template's default fill carries glass and every GENERIC
    // material together, so a fill-only rule calls the environment glass.
    std::string genericFill, glassFill;
    for (const DumpedMaterial& m : dumped) {
        if (m.name == "GENERIC - ENVIRONMENT")
            genericFill = m.row.cutFillId;
        if (m.name == "Glass")
            glassFill = m.row.cutFillId;
    }
    EXPECT_FALSE (genericFill.empty ());
    EXPECT_EQ (genericFill, glassFill) << "the premise of the fill-only failure";

    // Yet the combined rule refuses it, because its priority is not a stock one.
    for (const DumpedMaterial& m : dumped) {
        if (m.name != "GENERIC - ENVIRONMENT")
            continue;
        const SubstanceVerdict v = ClassifyBuildingMaterial (m.row, fills);
        EXPECT_EQ (v.substance, Substance::Unknown);
    }
}

TEST (BuildingMaterialSignal, AFillNeedsTwoIndependentWitnesses)
{
    // A fill whose only stock-priority member is the material being classified
    // proves nothing -- it is that material confirming itself. Two rows sharing
    // one fill, one of them stock, must leave the fill without an opinion.
    std::vector<BuildingMaterialRow> rows;
    rows.push_back ({ 910, "timber-fill" }); // Timber - Structural
    rows.push_back ({ 171, "timber-fill" }); // something authored, not stock
    const std::map<std::string, Substance> one = DeriveFillSubstances (rows);
    EXPECT_EQ (one.find ("timber-fill"), one.end ());

    // A second stock witness that agrees turns the fill into a real signal.
    rows.push_back ({ 810, "timber-fill" }); // Timber - Roof
    const std::map<std::string, Substance> two = DeriveFillSubstances (rows);
    ASSERT_NE (two.find ("timber-fill"), two.end ());
    EXPECT_EQ (two.at ("timber-fill"), Substance::Wood);

    // Two witnesses that DISAGREE mean the fill is shared across substances --
    // the measured case is one fill carrying air space, iron and zinc.
    rows.push_back ({ 550, "timber-fill" }); // Glass, into the same fill
    const std::map<std::string, Substance> mixed = DeriveFillSubstances (rows);
    EXPECT_EQ (mixed.find ("timber-fill"), mixed.end ());
}

TEST (BuildingMaterialSignal, ContradictingSignalsRefuseRatherThanPickOne)
{
    std::vector<BuildingMaterialRow> rows;
    rows.push_back ({ 910, "wood-fill" });
    rows.push_back ({ 810, "wood-fill" });
    rows.push_back ({ 740, "concrete-fill" });
    rows.push_back ({ 760, "concrete-fill" });
    const std::map<std::string, Substance> fills = DeriveFillSubstances (rows);

    // Timber's priority, concrete's hatch. Neither wins.
    const SubstanceVerdict v = ClassifyBuildingMaterial ({ 910, "concrete-fill" }, fills);
    EXPECT_EQ (v.substance, Substance::Unknown);
    EXPECT_FLOAT_EQ (v.confidence, 0.0f);
}

TEST (BuildingMaterialSignal, EverySubstanceHasAName)
{
    EXPECT_STREQ (SubstanceName (Substance::Unknown), "unknown");
    EXPECT_STREQ (SubstanceName (Substance::Earth), "earth");
    EXPECT_STREQ (SubstanceName (Substance::Concrete), "concrete");
    EXPECT_STREQ (SubstanceName (Substance::Metal), "metal");
    EXPECT_STREQ (SubstanceName (Substance::Plastic), "plastic");
    EXPECT_STREQ (SubstanceName (Substance::Glass), "glass");
    EXPECT_STREQ (SubstanceName (Substance::Wood), "wood");
}
