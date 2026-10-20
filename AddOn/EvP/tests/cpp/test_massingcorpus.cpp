#include "massing_corpus.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
namespace corpus = tapioca::testing::massingcorpus;
namespace js = evp::nodegraph::json;
using V = js::JsonValue;

const V& Required (const V& value, const std::string& key)
{
    const auto* field = value.Find (key);
    if (!field)
        throw std::runtime_error ("missing corpus field: " + key);
    return *field;
}
std::string Text (const V& value)
{
    std::string text;
    if (!value.AsString (text))
        throw std::runtime_error ("expected corpus string");
    return text;
}
const V& Fixture ()
{
    static const V fixture = [] () {
        std::ifstream file (std::string (EVP_TEST_FIXTURE_DIR) + "/massing-parity-v1.json");
        if (!file)
            throw std::runtime_error ("required committed massing corpus is absent");
        const std::string bytes { std::istreambuf_iterator<char> (file), std::istreambuf_iterator<char> () };
        if (bytes.size () > 1024 * 1024)
            throw std::runtime_error ("massing corpus exceeds 1 MiB test budget");
        auto parsed = js::Parse (bytes);
        int64_t version = 0;
        if (!parsed.ok || !Required (parsed.value, "schemaVersion").AsInteger (version) || version != 1)
            throw std::runtime_error ("invalid/unsupported massing corpus: " + parsed.error);
        if (!Required (parsed.value, "cases").AsArray () || !Required (parsed.value, "meshes").AsObject ())
            throw std::runtime_error ("invalid corpus cases/meshes containers");
        return parsed.value;
    }();
    return fixture;
}

const V& Goldens ()
{
    static const V goldens = [] () {
        std::ifstream file (std::string (EVP_TEST_FIXTURE_DIR) + "/massing-parity-v1.expected.json");
        if (!file)
            throw std::runtime_error ("required committed massing expectations are absent");
        const std::string bytes { std::istreambuf_iterator<char> (file), std::istreambuf_iterator<char> () };
        const auto parsed = js::Parse (bytes);
        int64_t version = 0;
        if (bytes.size () > 1024 * 1024 || !parsed.ok ||
            !Required (parsed.value, "schemaVersion").AsInteger (version) || version != 1 ||
            !Required (parsed.value, "cases").AsObject ())
            throw std::runtime_error ("invalid/unsupported massing expectations");
        return parsed.value;
    }();
    return Required (goldens, "cases");
}

const V& WorkflowShells ()
{
    static const V shells = [] () {
        std::ifstream file (std::string (EVP_TEST_FIXTURE_DIR) + "/massing-shells.json");
        if (!file)
            throw std::runtime_error ("required committed workflow shells are absent");
        const std::string bytes { std::istreambuf_iterator<char> (file), std::istreambuf_iterator<char> () };
        const auto parsed = js::Parse (bytes);
        if (bytes.size () > 1024 * 1024 || !parsed.ok || !parsed.value.AsArray () ||
            parsed.value.AsArray ()->size () != 2)
            throw std::runtime_error ("invalid required workflow shells");
        return parsed.value;
    }();
    return shells;
}

// Unlike the production JSON writer, diagnostics retain the integer/double
// distinction at whole-valued coordinates (3.0 versus authored story/colour 3).
// This emits text only; replay has no golden-file update path.
std::string SnapshotText (const V& value)
{
    if (const auto* array = value.AsArray ()) {
        std::string text = "[";
        for (const auto& item : *array) {
            if (text.size () > 1)
                text += ',';
            text += SnapshotText (item);
        }
        return text + ']';
    }
    if (const auto* object = value.AsObject ()) {
        std::string text = "{";
        for (const auto& [key, item] : *object) {
            if (text.size () > 1)
                text += ',';
            text += js::Write (V::String (key), 0) + ':' + SnapshotText (item);
        }
        return text + '}';
    }
    auto text = js::Write (value, 0);
    if (value.Kind () == js::JsonKind::Number && !value.IsIntegral () &&
        text.find_first_of (".eE") == std::string::npos)
        text += ".0";
    return text;
}

class MassingCorpus : public ::testing::TestWithParam<const char*> {};

TEST_P (MassingCorpus, ReplayCommittedCppBaseline)
{
    const auto& fixture = Fixture ();
    const auto& cases = *Required (fixture, "cases").AsArray ();
    const V* found = nullptr;
    for (const auto& item : cases)
        if (Text (Required (item, "name")) == GetParam ())
            found = &item;
    ASSERT_NE (found, nullptr);
    const auto& input = Required (*found, "input");
    const auto& meshes = Required (fixture, "meshes");
    const auto& shells = WorkflowShells ();
    const auto before = SnapshotText (input) + SnapshotText (meshes) + SnapshotText (shells);
    const auto actual = corpus::Evaluate (input, meshes, shells);
    EXPECT_EQ (SnapshotText (input) + SnapshotText (meshes) + SnapshotText (shells), before);
    std::string mismatch;
    ASSERT_TRUE (corpus::Match (actual, actual, 0, mismatch)) << "non-finite output at " << mismatch;
    const auto repeated = corpus::Evaluate (input, meshes, shells);
    EXPECT_TRUE (corpus::Match (actual, repeated, 0, mismatch)) << "nondeterministic output at " << mismatch;
    double tolerance = 0, anchorTolerance = 0;
    ASSERT_TRUE (Required (fixture, "absoluteTolerance").AsDouble (tolerance));
    ASSERT_TRUE (std::isfinite (tolerance) && tolerance > 0 && tolerance <= 1e-5);
    ASSERT_TRUE (Required (fixture, "anchorTolerance").AsDouble (anchorTolerance));
    ASSERT_TRUE (std::isfinite (anchorTolerance) && anchorTolerance > 0 && anchorTolerance <= 2e-5);
    if (const auto* specific = found->Find ("anchorTolerance")) {
        ASSERT_TRUE (specific->AsDouble (anchorTolerance));
        ASSERT_TRUE (std::isfinite (anchorTolerance) && anchorTolerance > 0 && anchorTolerance <= 1e-4);
    }
    for (const auto& [key, anchor] : *Required (*found, "analytic").AsObject ())
        EXPECT_TRUE (corpus::Match (anchor, Required (actual, key), anchorTolerance, mismatch))
            << "independent analytic anchor " << key << " differs";
    const auto* expected = Goldens ().Find (GetParam ());
    if (!expected || !corpus::Match (*expected, actual, tolerance, mismatch)) {
        // An explicit mismatch dump is diagnostic, never an automatic blessing.
        // Curated updates require analytic-anchor review and a committed diff.
        std::cout << "CORPUS_ACTUAL " << GetParam () << ' ' << SnapshotText (actual) << '\n';
        ADD_FAILURE () << "saved C++ baseline mismatch at " << mismatch;
    }
}

INSTANTIATE_TEST_SUITE_P (V1, MassingCorpus,
                          ::testing::Values ("RectangularPrism", "CourtyardPrism", "ThinExcludedFloor", "HalfEnvelope",
                                             "GroupedBuildings", "TaperedBody", "OpenBodyRefusal", "SlopedHeadroom",
                                             "StoreySlicesWithHole", "MissingStoreysRefusal", "LoggedBakeCornerCleanup",
                                             "ArcCleanupRefusal", "FacadeSharedWalls", "SurveyPrism",
                                             "BodyCourtyardIsland", "BodyRemovedTopFloor", "HeadroomHole",
                                             "HeadroomDisconnected", "RidgeHeadroom", "NearestLowExit",
                                             "ExactHeadroomThreshold", "BelowHeadroomThreshold",
                                             "ExplicitLevelsSorting", "AboveTopStorey", "WorkflowConvexShell",
                                             "WorkflowConcaveShell"),
                          [] (const auto& parameter) { return parameter.param; });

TEST (MassingCorpusContract, EveryVersionedCaseIsRegisteredAndProvenanced)
{
    const auto& fixture = Fixture ();
    const auto* cases = Required (fixture, "cases").AsArray ();
    ASSERT_NE (cases, nullptr);
    std::set<std::string> names;
    for (const auto& item : *cases) {
        ASSERT_TRUE (names.insert (Text (Required (item, "name"))).second);
        EXPECT_FALSE (Text (Required (item, "basis")).empty ());
        EXPECT_FALSE (Text (Required (item, "sourceTest")).empty ());
        ASSERT_NE (Required (item, "analytic").AsObject (), nullptr);
        EXPECT_FALSE (Required (item, "analytic").AsObject ()->empty ());
        const auto* baseline = item.Find ("baselineCommit");
        if (!baseline)
            baseline = &Required (fixture, "baselineCommit");
        const auto commit = Text (*baseline);
        EXPECT_GE (commit.size (), 7u);
        EXPECT_LE (commit.size (), 40u);
        EXPECT_EQ (commit.find_first_not_of ("0123456789abcdef"), std::string::npos);
        EXPECT_EQ (Text (Required (item, "expected")),
                   "massing-parity-v1.expected.json#" + Text (Required (item, "name")));
        ASSERT_NE (Goldens ().Find (Text (Required (item, "name"))), nullptr);
    }
    EXPECT_EQ (names, (std::set<std::string> { "RectangularPrism",
                                               "CourtyardPrism",
                                               "ThinExcludedFloor",
                                               "HalfEnvelope",
                                               "GroupedBuildings",
                                               "TaperedBody",
                                               "OpenBodyRefusal",
                                               "SlopedHeadroom",
                                               "StoreySlicesWithHole",
                                               "MissingStoreysRefusal",
                                               "LoggedBakeCornerCleanup",
                                               "ArcCleanupRefusal",
                                               "FacadeSharedWalls",
                                               "SurveyPrism",
                                               "BodyCourtyardIsland",
                                               "BodyRemovedTopFloor",
                                               "HeadroomHole",
                                               "HeadroomDisconnected",
                                               "RidgeHeadroom",
                                               "NearestLowExit",
                                               "ExactHeadroomThreshold",
                                               "BelowHeadroomThreshold",
                                               "ExplicitLevelsSorting",
                                               "AboveTopStorey",
                                               "WorkflowConvexShell",
                                               "WorkflowConcaveShell" }));
    EXPECT_EQ (Goldens ().AsObject ()->size (), names.size ());
}

TEST (MassingCorpusContract, ComparisonRejectsStructuralIdentityAndNumericalDrift)
{
    std::string mismatch;
    EXPECT_TRUE (corpus::Match (V::Double (7000000.0), V::Double (7000000.0000001), 1e-6, mismatch));
    EXPECT_FALSE (corpus::Match (V::Double (7000000.0), V::Double (7000000.001), 1e-6, mismatch));
    EXPECT_FALSE (corpus::Match (V::Integer (9007199254740993LL), V::Integer (9007199254740992LL), 1, mismatch));
    EXPECT_FALSE (corpus::Match (V::Integer (3), V::Double (3), 1, mismatch));
    EXPECT_FALSE (corpus::Match (V::String ("Tower"), V::String ("tower"), 1, mismatch));
    EXPECT_FALSE (corpus::Match (V::Bool (true), V::Bool (false), 1, mismatch));
    const auto a = V::Array ({ V::Integer (1), V::Integer (2) });
    EXPECT_FALSE (corpus::Match (a, V::Array ({ V::Integer (2), V::Integer (1) }), 1, mismatch));
    EXPECT_FALSE (corpus::Match (a, V::Array ({ V::Integer (1) }), 1, mismatch));
    EXPECT_FALSE (corpus::Match (V::Object ({}), V::Object ({ { "extra", V () } }), 1, mismatch));
    EXPECT_FALSE (corpus::Match (V::Double (0), V::Double (std::numeric_limits<double>::quiet_NaN ()), 1, mismatch));
    EXPECT_FALSE (corpus::Match (V (), V (), std::numeric_limits<double>::infinity (), mismatch));
    EXPECT_FALSE (corpus::Match (V (), V (), -1, mismatch));
}

TEST (MassingCorpusContract, MalformedInputIsNotRecordedAsAnExpectedGeometryRefusal)
{
    EXPECT_THROW (corpus::Evaluate (V::Object ({}), V::Object ({})), std::runtime_error);
    EXPECT_THROW (corpus::Evaluate (V::Object ({ { "mode", V::String ("unknown") } }), V::Object ({})),
                  std::runtime_error);
    const auto bad = js::Parse (R"({"mode":"cleanRing","xy":["bad"],"arcs":[]})");
    ASSERT_TRUE (bad.ok);
    EXPECT_THROW (corpus::Evaluate (bad.value, V::Object ({})), std::runtime_error);
    const auto badMetadata = js::Parse (
        R"({"mode":"build","storeys":[],"slabs":[{"guid":"x","id":"x","bottom":0,"top":3,"outer":[],"holes":[],"metadata":null}]})");
    ASSERT_TRUE (badMetadata.ok);
    EXPECT_THROW (corpus::Evaluate (badMetadata.value, V::Object ({})), std::runtime_error);
}

TEST (MassingCorpusContract, SnapshotDiagnosticPreservesExactNumericKinds)
{
    const auto value = V::Object ({ { "coordinate", V::Double (3) },
                                    { "sourceStory", V::Integer (3) },
                                    { "label", V::String ("Tower\n\"A\"") } });
    const auto parsed = js::Parse (SnapshotText (value));
    ASSERT_TRUE (parsed.ok);
    std::string mismatch;
    EXPECT_TRUE (corpus::Match (value, parsed.value, 0, mismatch)) << mismatch;
    EXPECT_FALSE (Required (parsed.value, "coordinate").IsIntegral ());
    EXPECT_TRUE (Required (parsed.value, "sourceStory").IsIntegral ());
    EXPECT_FALSE (corpus::Match (value, value, 0, mismatch, "$", 65));
}

// A check of the literal inputs, independent of the slice/headroom/facade
// consumers: oriented edge incidence and the scalar-triple-product volume.
void CheckBoxShell (const V& mesh, size_t firstIndex, int64_t firstVertex, double expectedVolume)
{
    const auto& vertices = *Required (mesh, "vertices").AsArray ();
    const auto& triangles = *Required (mesh, "triangles").AsArray ();
    ASSERT_LE (firstIndex + 36, triangles.size ());
    std::map<std::pair<int64_t, int64_t>, std::pair<int, int>> edges;
    double volume = 0;
    for (size_t i = firstIndex; i < firstIndex + 36; i += 3) {
        std::array<int64_t, 3> indices {};
        std::array<std::array<double, 3>, 3> points {};
        for (size_t j = 0; j < 3; ++j) {
            ASSERT_TRUE (triangles[i + j].AsInteger (indices[j]));
            ASSERT_GE (indices[j], firstVertex);
            ASSERT_LT (indices[j], firstVertex + 8);
            const auto base = size_t (indices[j]) * 3;
            ASSERT_LE (base + 3, vertices.size ());
            for (size_t axis = 0; axis < 3; ++axis) {
                ASSERT_TRUE (vertices[base + axis].AsDouble (points[j][axis]));
                ASSERT_TRUE (std::isfinite (points[j][axis]));
            }
        }
        for (size_t j = 0; j < 3; ++j) {
            const auto a = indices[j], b = indices[(j + 1) % 3];
            auto& edge = edges[a < b ? std::pair { a, b } : std::pair { b, a }];
            ++edge.first;
            edge.second += a < b ? 1 : -1;
        }
        for (size_t axis = 0; axis < 3; ++axis)
            volume += points[0][axis] *
                      (points[1][(axis + 1) % 3] * points[2][(axis + 2) % 3] -
                       points[1][(axis + 2) % 3] * points[2][(axis + 1) % 3]) /
                      6;
    }
    EXPECT_NEAR (volume, expectedVolume, 1e-9);
    for (const auto& [key, counts] : edges) {
        EXPECT_EQ (counts.first, 2) << key.first << ':' << key.second;
        EXPECT_EQ (counts.second, 0) << "edge directions must oppose";
    }
}

TEST (MassingCorpusContract, LiteralDisconnectedShellsPreserveCavityWindingAndSeparateSolidVolumes)
{
    const auto& meshes = Required (Fixture (), "meshes");
    const auto& courtyard = Required (meshes, "courtyardIsland");
    CheckBoxShell (courtyard, 0, 0, 900);
    CheckBoxShell (courtyard, 36, 8, -36); // Inward-facing cavity, not positive fill.
    CheckBoxShell (courtyard, 72, 16, 144);
    const auto& stacked = Required (meshes, "stackedSolids");
    CheckBoxShell (stacked, 0, 0, 100);
    CheckBoxShell (stacked, 36, 8, 200);
}

TEST (MassingCorpusContract, WorkflowShellClearanceAreasHaveIndependentRequestBasedAnchors)
{
    const auto& shells = WorkflowShells ();
    for (const auto& item : *Required (Fixture (), "cases").AsArray ()) {
        const auto& input = Required (item, "input");
        const auto* selected = input.Find ("envelopeFixture");
        if (!selected)
            continue;
        int64_t index = -1;
        ASSERT_TRUE (selected->AsInteger (index));
        ASSERT_TRUE (index == 0 || index == 1);
        EXPECT_EQ (Text (Required (item, "name")), index == 0 ? "WorkflowConvexShell" : "WorkflowConcaveShell");
        const auto& request = Required ((*shells.AsArray ())[size_t (index)], "request");
        double eaves = 0, runPerRise = 0;
        ASSERT_TRUE (Required (request, "baseHeight").AsDouble (eaves));
        ASSERT_TRUE (Required (request, "runPerRise").AsDouble (runPerRise));
        ASSERT_DOUBLE_EQ (eaves, 8.5);
        ASSERT_DOUBLE_EQ (runPerRise, 0.5);
        const auto& edges = *Required (request, "edges").AsArray ();
        ASSERT_EQ (edges.size (), index == 0 ? 4u : 6u);
        const std::vector<double> parcel = index == 0
                                               ? std::vector<double> { 0, 0, 20, 0, 20, 20, 0, 20 }
                                               : std::vector<double> { 0, 0, 30, 0, 30, 12, 12, 12, 12, 30, 0, 30 };
        for (size_t i = 0; i < edges.size (); ++i) {
            const auto& edge = edges[i];
            double setback = 0, arc = 0;
            ASSERT_TRUE (Required (edge, "distance").AsDouble (setback));
            ASSERT_DOUBLE_EQ (setback, index == 0 ? 3.0 : 1.0);
            ASSERT_TRUE (Required (edge, "arc").AsDouble (arc));
            ASSERT_DOUBLE_EQ (arc, 0.0);
            bool regulated = false;
            ASSERT_TRUE (Required (edge, "regulated").AsBool (regulated));
            ASSERT_TRUE (regulated);
            for (const auto* key : { "a", "b" }) {
                const auto* point = Required (edge, key).AsArray ();
                ASSERT_NE (point, nullptr);
                ASSERT_EQ (point->size (), 2u);
                const auto vertex = key[0] == 'a' ? i : (i + 1) % edges.size ();
                for (size_t axis = 0; axis < 2; ++axis) {
                    double coordinate = 0;
                    ASSERT_TRUE ((*point)[axis].AsDouble (coordinate));
                    ASSERT_DOUBLE_EQ (coordinate, parcel[2 * vertex + axis]);
                }
            }
        }
        const auto actual = corpus::Evaluate (input, Required (Fixture (), "meshes"), shells);
        const auto& rows = *Required (actual, "rows").AsArray ();
        ASSERT_EQ (rows.size (), 10u);
        double previous = index == 0 ? 196.0 : 460.0;
        double expectedTotal = 0, actualTotal = 0;
        for (size_t i = 0; i < rows.size (); ++i) {
            double z = 0, area = 0;
            ASSERT_TRUE (Required (rows[i], "z").AsDouble (z));
            ASSERT_TRUE (Required (rows[i], "allowedArea").AsDouble (area));
            EXPECT_DOUBLE_EQ (z, 3.0 * double (i));
            if (index == 0) {
                // Geometry from the square request, not the payload's reported
                // area or the saved C++ output. Both sides retreat above eaves.
                const double side = std::max (0.0, 14.0 - 2 * runPerRise * std::max (0.0, z + 1.6 - eaves));
                EXPECT_NEAR (area, side * side, 2e-5);
                expectedTotal += side * side;
            }
            else if (i < 3) {
                EXPECT_NEAR (area, 2 * 28.0 * 10.0 - 10.0 * 10.0, 2e-5);
            }
            EXPECT_GE (area, 0.0);
            EXPECT_LE (area, previous + 2e-5);
            previous = area;
            actualTotal += area;
        }
        EXPECT_DOUBLE_EQ (previous, 0.0);
        if (index == 0) {
            EXPECT_NEAR (expectedTotal, 852.04, 1e-12);
            // Four nonintegral roof-section areas each have the existing 2e-5
            // anchor bound. Accumulated error is bounded by their sum, not by
            // pretending the full ten-floor total has one section's error.
            EXPECT_NEAR (actualTotal, expectedTotal, 4 * 2e-5);
        }
    }
}

TEST (MassingCorpusContract, InvalidWorkflowShellReferencesAndPayloadsFailRatherThanBecomeGoldens)
{
    const auto parsed = js::Parse (R"({"mode":"build","envelopeFixture":0,"storeys":[],"slabs":[]})");
    ASSERT_TRUE (parsed.ok);
    const auto& input = parsed.value;
    const auto& meshes = Required (Fixture (), "meshes");
    EXPECT_THROW (corpus::Evaluate (input, meshes), std::runtime_error);
    EXPECT_THROW (corpus::Evaluate (input, meshes, V::Array ({})), std::runtime_error);
    EXPECT_THROW (corpus::Evaluate (input, meshes, V::Array ({ V::Object ({}) })), std::runtime_error);
    for (const auto& payload : { V (), V::Object ({}), V::Object ({ { "hasEnvelope", V::Bool (false) } }) })
        EXPECT_THROW (corpus::Evaluate (input, meshes, V::Array ({ V::Object ({ { "payload", payload } }) })),
                      std::runtime_error);
    for (const auto& selector : { V::Integer (-1), V::Integer (2), V::Double (0.5), V::String ("0") }) {
        auto fields = *input.AsObject ();
        fields["envelopeFixture"] = selector;
        EXPECT_THROW (corpus::Evaluate (V::Object (std::move (fields)), meshes, WorkflowShells ()), std::runtime_error);
    }
    auto fields = *input.AsObject ();
    fields["envelope"] = V::String ("halfEnvelope");
    EXPECT_THROW (corpus::Evaluate (V::Object (std::move (fields)), meshes, WorkflowShells ()), std::runtime_error);
}
} // namespace
