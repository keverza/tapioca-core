#include "massing_corpus.hpp"

#include <gtest/gtest.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>

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
    const auto before = SnapshotText (input) + SnapshotText (meshes);
    const auto actual = corpus::Evaluate (input, meshes);
    EXPECT_EQ (SnapshotText (input) + SnapshotText (meshes), before);
    std::string mismatch;
    ASSERT_TRUE (corpus::Match (actual, actual, 0, mismatch)) << "non-finite output at " << mismatch;
    const auto repeated = corpus::Evaluate (input, meshes);
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
                                             "ArcCleanupRefusal", "FacadeSharedWalls", "SurveyPrism"),
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
        EXPECT_EQ (Text (Required (item, "expected")),
                   "massing-parity-v1.expected.json#" + Text (Required (item, "name")));
        ASSERT_NE (Goldens ().Find (Text (Required (item, "name"))), nullptr);
    }
    EXPECT_EQ (names,
               (std::set<std::string> { "RectangularPrism", "CourtyardPrism", "ThinExcludedFloor", "HalfEnvelope",
                                        "GroupedBuildings", "TaperedBody", "OpenBodyRefusal", "SlopedHeadroom",
                                        "StoreySlicesWithHole", "MissingStoreysRefusal", "LoggedBakeCornerCleanup",
                                        "ArcCleanupRefusal", "FacadeSharedWalls", "SurveyPrism" }));
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
} // namespace
