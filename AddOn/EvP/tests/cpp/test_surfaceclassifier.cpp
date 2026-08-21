// ArchViz/SurfaceClassifier — run against the REAL template, not against
// invented materials.
//
// ⚠️ WHY OFFLINE, AND WHY OVER THE DUMP. A classifier is exactly the kind of
// code that passes any test you write for it, because the same intuition writes
// the rule and the example. The only test with any force is the one that runs it
// over the 212 surfaces a real Archicad template actually contains, and checks
// the verdicts against what those surfaces are. So the fixture is the committed
// dump, and the check is a confusion count — not six hand-built structs.
//
// ⚠️ NAMES ARE USED IN THIS FILE AND MUST NEVER BE USED IN THE CLASSIFIER. The
// distinction is the whole point and is not a loophole:
//   * the CLASSIFIER never receives a name. It is handed a `SurfaceMaterial`
//     whose `name` field it does not read, and it ships inside the .apx;
//   * this TEST reads the name to say "the thing the numbers called a metal is
//     one", which is how a threshold gets checked at all. It runs offline,
//     decides nothing at runtime, and ships nowhere.
// If a future template is authored in a third language, this file's checks go
// stale and the classifier does not. That asymmetry is the design.

#include "ArchViz/SurfaceClassifier.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace geomsrv::archviz;

namespace {

struct DumpedSurface {
    std::string name;
    std::string materialType;
    bool usedByModel = false;
    SurfaceMaterial material;
};

// The fixture's columns are the OFFICIAL API's integers. ⚠️ THIS CONVERSION IS
// `ExtractionEnvironment`'S, RESTATED: transparency flips to alpha, shine is the
// raw UMAT storage and needs BOTH divisions (by 100 to the factor, by 100 again
// to the stored 0..1), and specular/diffuse are already percentages. Restating
// it here rather than calling the extraction is deliberate — the extraction
// needs the DevKit, and a test that needed the DevKit could not run offline.
SurfaceMaterial FromOfficialApi (int transparency, int shine, int specular, int diffuse, float specR, float specG,
                                 float specB)
{
    SurfaceMaterial m;
    m.alpha = 1.0f - float (transparency) / 100.0f;
    m.shininess = float (shine) / 10000.0f;
    m.specular = float (specular) / 100.0f;
    m.diffuse = float (diffuse) / 100.0f;
    m.specularR = specR;
    m.specularG = specG;
    m.specularB = specB;
    return m;
}

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

// ⚠️ SKIPS RATHER THAN FAILS WHEN THE FIXTURE IS ABSENT, the same contract
// test_environmentlighting.cpp uses for the sky fixture: a fresh clone without
// the dump must still get a green suite.
std::vector<DumpedSurface> LoadDump ()
{
    std::vector<DumpedSurface> rows;
#ifdef EVP_TEST_FIXTURE_DIR
    const std::string path = std::string (EVP_TEST_FIXTURE_DIR) + "/surface_template_20260821.tsv";
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
            if (f.size () >= 11) {
                DumpedSurface s;
                s.name = f[0];
                s.materialType = f[1];
                s.usedByModel = std::atoi (f[10].c_str ()) != 0;
                s.material =
                    FromOfficialApi (std::atoi (f[2].c_str ()), std::atoi (f[3].c_str ()), std::atoi (f[4].c_str ()),
                                     std::atoi (f[5].c_str ()), float (std::atof (f[6].c_str ())),
                                     float (std::atof (f[7].c_str ())), float (std::atof (f[8].c_str ())));
                rows.push_back (s);
            }
        }
        line.clear ();
    }
    std::fclose (file);
#endif
    return rows;
}

// VALIDATION ONLY — see this file's header. Returns "" when the name carries no
// family, which is most of them.
std::string FamilyFromName (const std::string& name)
{
    static const std::map<std::string, std::string> kFamilies = {
        { "Stiklas", "glass" },
        { "Glass", "glass" },
        { "Metal", "metal" },
        { "Metalas", "metal" },
    };
    const size_t space = name.find (' ');
    const std::string head = space == std::string::npos ? name : name.substr (0, space);
    const auto it = kFamilies.find (head);
    return it == kFamilies.end () ? std::string () : it->second;
}

} // namespace

TEST (SurfaceClassifier, TheDumpFixtureIsTheRealTemplate)
{
    const std::vector<DumpedSurface> rows = LoadDump ();
    if (rows.empty ())
        GTEST_SKIP () << "surface_template_20260821.tsv is not present";

    EXPECT_EQ (rows.size (), 212u);

    // ⚠️ THE REASON THIS CLASSIFIER EXISTS AT ALL, pinned so a template that
    // DOES author materialType is noticed instead of being classified the hard
    // way. If this ever fails, read the field and delete most of the classifier.
    for (const DumpedSurface& s : rows)
        EXPECT_EQ (s.materialType, "General") << s.name;
}

TEST (SurfaceClassifier, NoMetalOrGlassVerdictIsWrongOnTheRealTemplate)
{
    const std::vector<DumpedSurface> rows = LoadDump ();
    if (rows.empty ())
        GTEST_SKIP () << "surface_template_20260821.tsv is not present";

    // ⚠️ THE ONE CHECK THAT MATTERS. Glass and Metal are the verdicts that move
    // F0 and roughness far enough to ruin a surface, so a false positive on
    // either is the failure this whole design is arranged to avoid. Every
    // surface the numbers call glass or metal must be one.
    //
    // "Glass - Mirror" is the interesting pass: transparency 0, specular 100,
    // shine 4624. The numbers call it a METAL and that is the right answer for a
    // renderer — a mirror is a specular reflector with no transmission, which is
    // a conductor's behaviour, whatever the surface is called. It is also a neat
    // demonstration of why the name is not consulted.
    for (const DumpedSurface& s : rows) {
        const SurfaceVerdict v = ClassifySurface (s.material);
        const std::string family = FamilyFromName (s.name);
        if (family.empty ())
            continue;

        if (v.cls == SurfaceClass::Metal) {
            const bool metallic = family == "metal" || s.name == "Glass - Mirror";
            EXPECT_TRUE (metallic) << "called metal, is not: " << s.name;
        }
        if (v.cls == SurfaceClass::Glass)
            EXPECT_EQ (family, "glass") << "called glass, is not: " << s.name;
    }
}

TEST (SurfaceClassifier, AmbiguousSurfacesAreReportedRatherThanGuessed)
{
    const std::vector<DumpedSurface> rows = LoadDump ();
    if (rows.empty ())
        GTEST_SKIP () << "surface_template_20260821.tsv is not present";

    std::vector<std::string> unclassified;
    for (const DumpedSurface& s : rows) {
        const SurfaceVerdict v = ClassifySurface (s.material);
        if (v.cls == SurfaceClass::Unclassified) {
            unclassified.push_back (s.name);
            EXPECT_FLOAT_EQ (v.confidence, 0.0f) << s.name;
        }
    }

    // ⚠️ THE EXACT LIST, NOT A COUNT, and every entry is a genuine ambiguity
    // rather than a gap to be closed later:
    //   Metal - Steel (specular 75) and the two gloss whites (69) are the
    //     metal-versus-paint band with no channel left to separate them;
    //   Foliage and Bubble Wrap are transparent for reasons that are not glass.
    // A future rule that classifies one of these must justify itself against
    // this list, and a rule that removes one by GUESSING will change it here
    // where the change is visible in review.
    const std::vector<std::string> expected = {
        "Metal - Steel",        "Foliage - Leaves Tree Small", "Insulation - Bubble Wrap",
        "caparol - PALAZZO 45", "Paint - Glossy White",
    };
    EXPECT_EQ (unclassified.size (), expected.size ());
    for (const std::string& name : expected)
        EXPECT_NE (std::find (unclassified.begin (), unclassified.end (), name), unclassified.end ())
            << name << " should still be reported as unclassified";
}

TEST (SurfaceClassifier, EveryGlassInTheTemplateIsFound)
{
    const std::vector<DumpedSurface> rows = LoadDump ();
    if (rows.empty ())
        GTEST_SKIP () << "surface_template_20260821.tsv is not present";

    // Recall, not just precision: a classifier that returns Unclassified for
    // everything would pass the false-positive test above and be useless.
    int glassFamily = 0, glassFound = 0;
    for (const DumpedSurface& s : rows) {
        if (FamilyFromName (s.name) != "glass")
            continue;
        ++glassFamily;
        if (ClassifySurface (s.material).cls == SurfaceClass::Glass)
            ++glassFound;
    }
    EXPECT_EQ (glassFamily, 11);
    // 11 minus "Glass - Mirror", which is opaque and correctly reads as metal.
    EXPECT_EQ (glassFound, 10);
}

TEST (SurfaceClassifier, TheChannelsThatDecideAreTheOnesArchicadDoesSupply)
{
    // ⚠️ A UNIT TEST FOR THE RULES THEMSELVES, so a threshold cannot be moved
    // without a deliberate edit here. The values are the measured surfaces named
    // in SurfaceClassifier.hpp, in official-API units.

    // "Air": transparency 100. A modelling helper, not a pane.
    EXPECT_EQ (ClassifySurface (FromOfficialApi (100, 0, 0, 0, 1, 1, 1)).cls, SurfaceClass::Invisible);

    // "Stiklas - SKAIDRUS": transparency 69, shine 7952, specular 100.
    EXPECT_EQ (ClassifySurface (FromOfficialApi (69, 7952, 100, 60, 1, 1, 1)).cls, SurfaceClass::Glass);

    // "Glass - Satin": transparency 25, specular 40, diffuse 29 — the dull
    // branch, which only holds because the diffuse is low.
    EXPECT_EQ (ClassifySurface (FromOfficialApi (25, 700, 40, 29, 1, 1, 1)).cls, SurfaceClass::Glass);

    // "Foliage - Leaves Tree Small": transparency 23, specular 56, diffuse 91.
    // Same transparency band, and it must NOT come out as glass.
    EXPECT_EQ (ClassifySurface (FromOfficialApi (23, 300, 56, 91, 1, 1, 1)).cls, SurfaceClass::Unclassified);

    // "Metal - Chrome 01": opaque, specular 96, shine 5392. Neutral tint, so it
    // is the high-specular rule that catches it.
    EXPECT_EQ (ClassifySurface (FromOfficialApi (0, 5392, 96, 30, 1, 1, 1)).cls, SurfaceClass::Metal);

    // "Metal - Copper New": specular 67 is below the neutral rule; the COLOURED
    // highlight (0.98, 0.87, 0.53) is what identifies it.
    EXPECT_EQ (ClassifySurface (FromOfficialApi (0, 820, 67, 44, 0.98f, 0.87f, 0.53f)).cls, SurfaceClass::Metal);

    // ⚠️ THE SAME TINT WITHOUT THE HIGHLIGHT IS NOT A METAL. "Water - Pond":
    // tint 0.451, specular 72, but shine 64 — a factor of 0.64, far below
    // kMetalMinShineFactor. Tint alone must never carry a metal verdict.
    EXPECT_NE (ClassifySurface (FromOfficialApi (0, 64, 72, 63, 0.92f, 0.73f, 0.47f)).cls, SurfaceClass::Metal);

    // "Water - Wavy": transparency 1, specular 100, shine 700. Strongly specular
    // and opaque enough to reach the metal branch — the transparency is what
    // keeps it out. This is the single case that made metal require opacity.
    EXPECT_NE (ClassifySurface (FromOfficialApi (1, 700, 100, 38, 1, 1, 1)).cls, SurfaceClass::Metal);

    // "Plastikas - BALTAS BLIZGUS": shine 2818, specular 62 — below the
    // ambiguous band, so it is safely a polished dielectric.
    EXPECT_EQ (ClassifySurface (FromOfficialApi (0, 2818, 62, 80, 1, 1, 1)).cls, SurfaceClass::Polished);

    // "Betonas - PILKAS ŠVIESUS": shine 100, a factor of 1.0. Matte.
    EXPECT_EQ (ClassifySurface (FromOfficialApi (0, 100, 25, 62, 1, 1, 1)).cls, SurfaceClass::Matte);
}

TEST (SurfaceClassifier, AConfidentVerdictNeverCarriesZeroConfidence)
{
    const std::vector<DumpedSurface> rows = LoadDump ();
    if (rows.empty ())
        GTEST_SKIP () << "surface_template_20260821.tsv is not present";

    for (const DumpedSurface& s : rows) {
        const SurfaceVerdict v = ClassifySurface (s.material);
        if (v.cls == SurfaceClass::Unclassified)
            EXPECT_FLOAT_EQ (v.confidence, 0.0f) << s.name;
        else
            EXPECT_GT (v.confidence, 0.0f) << s.name;
        EXPECT_LE (v.confidence, 1.0f) << s.name;
    }
}

TEST (SurfaceClassifier, EveryClassHasAName)
{
    // The log and the dump probe print these; an unnamed class would surface as
    // a silent "unclassified" in evidence that is meant to be read.
    EXPECT_STREQ (SurfaceClassName (SurfaceClass::Unclassified), "unclassified");
    EXPECT_STREQ (SurfaceClassName (SurfaceClass::Invisible), "invisible");
    EXPECT_STREQ (SurfaceClassName (SurfaceClass::Glass), "glass");
    EXPECT_STREQ (SurfaceClassName (SurfaceClass::Metal), "metal");
    EXPECT_STREQ (SurfaceClassName (SurfaceClass::Polished), "polished");
    EXPECT_STREQ (SurfaceClassName (SurfaceClass::Matte), "matte");
}

// ---- RE51.B4: the preset the verdict earns ---------------------------------

TEST (SurfacePresets, OnlyAConductorEverGetsMetalness)
{
    const std::vector<DumpedSurface> rows = LoadDump ();
    if (rows.empty ())
        GTEST_SKIP () << "surface_template_20260821.tsv is not present";

    // ⚠️ THE STANDING RULE, PINNED AS A TEST. Metalness must come from the
    // classifier and from nothing else -- never from shininess, which would make
    // every gloss-white wall and every pane of glass a conductor. If this ever
    // fails, something started inferring it again.
    for (const DumpedSurface& s : rows) {
        const SurfacePreset preset = PresetFor (s.material);
        const bool isMetal = ClassifySurface (s.material).cls == SurfaceClass::Metal;
        EXPECT_FLOAT_EQ (preset.metallic, isMetal ? 1.0f : 0.0f) << s.name;
    }
}

TEST (SurfacePresets, GlassGetsPhysicalDielectricReflectanceNotItsAuthoredValue)
{
    // "Stiklas - SKAIDRUS": authored at specular 100, which the shader would map
    // to F0 = 0.08 -- twice the reflectance of real glass. The preset replaces it
    // with the IOR-1.5 dielectric value, F0 = 0.04, which the shader reaches
    // through 0.08 * 0.5.
    const SurfaceMaterial glass = FromOfficialApi (69, 7952, 100, 60, 1, 1, 1);
    ASSERT_EQ (ClassifySurface (glass).cls, SurfaceClass::Glass);

    const SurfacePreset preset = PresetFor (glass);
    EXPECT_FLOAT_EQ (preset.metallic, 0.0f);
    EXPECT_FLOAT_EQ (preset.reflectance, 0.5f);
    EXPECT_FLOAT_EQ (preset.reflectance * 0.08f, 0.04f);

    // And it stays smooth: shine 7952 is a factor of 79.52, so the measured
    // roughness is already glass-like and the transparent floor never engages.
    EXPECT_LT (preset.roughness, 0.20f);
}

// ⚠️ THIS TEST CHANGED ITS MIND ON 2026-08-21, AND THE LIVE RUN IS WHY. It used
// to assert that a metal keeps its measured roughness EXACTLY -- shine 1800 ->
// 0.316 -- on the principle that a measurement beats an invention. The run said
// the measurement is not one: "HDR does not land on object, they do not reflect
// hdr", and in the same breath, "if roughness bias is set to -1.00 scene becomes
// reflective". So the prefiltered environment was arriving correctly and 0.316
// was simply selecting a mip with nothing recognisable left in it. See
// kMetalRoughness for what a bare architectural metal actually is.
TEST (SurfacePresets, AMetalIsCappedAtAPolishedRoughness)
{
    // "Metalas - PLIENAS NERŪDIJANTIS": shine 1800, a factor of 18, whose
    // measured roughness is 0.316 -- rougher than any bare metal reads.
    const SurfaceMaterial steel = FromOfficialApi (0, 1800, 95, 34, 1, 1, 1);
    ASSERT_EQ (ClassifySurface (steel).cls, SurfaceClass::Metal);

    const SurfacePreset preset = PresetFor (steel);
    EXPECT_FLOAT_EQ (preset.metallic, 1.0f);
    EXPECT_FLOAT_EQ (preset.roughness, kMetalRoughness);
}

// ⚠️ THE OTHER HALF OF THE SAME CONTRACT, AND THE REASON IT IS A CEILING RATHER
// THAN AN OVERRIDE. A surface authored GLOSSIER than its class ceiling keeps
// what its author gave it; the ceiling only stops a class from being rougher
// than the thing it is a class of can physically be. Without this test the
// distinction is invisible -- an override passes the test above just as well.
TEST (SurfacePresets, AMetalAuthoredGLOSSIERThanTheCeilingKeepsIt)
{
    // Shine 9000 -> factor 90 -> roughness sqrt(2/92) = 0.147, just inside the
    // 0.15 ceiling.
    const SurfaceMaterial polished = FromOfficialApi (0, 9000, 95, 34, 1, 1, 1);
    ASSERT_EQ (ClassifySurface (polished).cls, SurfaceClass::Metal);

    const SurfacePreset preset = PresetFor (polished);
    EXPECT_LT (preset.roughness, kMetalRoughness);
    EXPECT_NEAR (preset.roughness, 0.147442f, 1e-5f);
}

// ⚠️ GLASS NEEDED A CEILING OF ITS OWN, AND MaterialTable's 0.35 IS NOT IT.
// That one is a FLOOR ON GLOSS for every transparent range -- written before
// there was a classifier, and it also catches tinted plastics, water and
// cut-out foliage, so it cannot be lowered to what float glass actually is.
// This branch knows the range is glass.
TEST (SurfacePresets, GlassIsNearMirrorSmooth)
{
    // The same pane as the reflectance test above: 69% transparent, shine 7952.
    const SurfaceMaterial glass = FromOfficialApi (69, 7952, 100, 60, 1, 1, 1);
    ASSERT_EQ (ClassifySurface (glass).cls, SurfaceClass::Glass);
    EXPECT_FLOAT_EQ (PresetFor (glass).roughness, kGlassRoughness);

    // And a pane authored with NO shine at all -- which the transparent floor
    // would otherwise leave at 0.35 -- comes out as glass too. This is the case
    // that made the live run report no reflections: 0.35 selects a mip four
    // levels down a twelve-level chain, which is a 128-pixel-wide panorama.
    const SurfaceMaterial dullPane = FromOfficialApi (69, 0, 100, 60, 1, 1, 1);
    ASSERT_EQ (ClassifySurface (dullPane).cls, SurfaceClass::Glass);
    EXPECT_FLOAT_EQ (PresetFor (dullPane).roughness, kGlassRoughness);
}

TEST (SurfacePresets, AnUnclassifiedSurfaceIsLeftExactlyAsItWasMeasured)
{
    // ⚠️ THE CONTRACT FOR A REFUSAL. "Paint - Glossy White" is the surface the
    // classifier declines to name; it must be drawn precisely as the renderer
    // drew it before any classification existed -- measured roughness, measured
    // specular, no metalness. A refusal must never become a silent third look.
    const SurfaceMaterial glossyWhite = FromOfficialApi (0, 3856, 69, 30, 0.99f, 0.99f, 1.0f);
    ASSERT_EQ (ClassifySurface (glossyWhite).cls, SurfaceClass::Unclassified);

    const SurfacePreset preset = PresetFor (glossyWhite);
    EXPECT_FLOAT_EQ (preset.metallic, 0.0f);
    EXPECT_FLOAT_EQ (preset.reflectance, glossyWhite.specular);
    EXPECT_FLOAT_EQ (preset.roughness, SurfaceRoughness (glossyWhite));
}
