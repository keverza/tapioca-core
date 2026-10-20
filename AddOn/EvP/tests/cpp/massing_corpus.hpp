#ifndef TAPIOCA_TEST_MASSING_CORPUS_HPP
#define TAPIOCA_TEST_MASSING_CORPUS_HPP

#include "NodeGraph/Json.hpp"

namespace tapioca::testing::massingcorpus {
using Value = evp::nodegraph::json::JsonValue;

// Test-only adapters. Malformed fixtures throw; domain refusals are captured as
// data. No host access, backend flip, golden-file writer or production serializer.
Value Evaluate (const Value& input, const Value& meshes);
// Objects/array lengths/order, strings, flags and integral IDs are exact. Only
// floating values use the explicit absolute tolerance; no relative/world-scale slack.
bool Match (const Value& expected, const Value& actual, double tolerance, std::string& mismatch,
            const std::string& path = "$", size_t depth = 0);
} // namespace tapioca::testing::massingcorpus

#endif
