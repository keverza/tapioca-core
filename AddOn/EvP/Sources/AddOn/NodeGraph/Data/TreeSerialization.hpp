#ifndef EVP_NODEGRAPH_DATA_TREESERIALIZATION_HPP
#define EVP_NODEGRAPH_DATA_TREESERIALIZATION_HPP

// A data tree as JSON, and back (HANDOFF 7.8, 34).
//
// The split follows 7.8 exactly: THIS file owns the structure - the declared
// item type, the paths in canonical order, every list including the empty ones,
// item order, nullness and metadata - and the registered ValueTypeAssistant
// owns the encoding of one item. So a new item type becomes persistable by
// gaining an assistant, without this file learning about it.
//
// Failure is per-site and named. A tree that cannot be written (a mesh, a
// metadata value with no encoding) reports which path and index refused, rather
// than writing a document that reads back as a different tree. 7.5's four
// states - empty tree, empty list, null item, absent - survive the round trip;
// collapsing any of them here would be silent data loss on save.

#include "NodeGraph/Data/DataTree.hpp"
#include "NodeGraph/Json.hpp"

#include <string>

namespace evp::nodegraph::data {

bool SerializeTree (const IDataTree& tree, json::JsonValue& result, std::string& error);

// Rebuilds the typed tree the document names. The item type comes from the
// document, not from the caller: a tree is only meaningful with the type it was
// written as.
bool DeserializeTree (const json::JsonValue& encoded, TreeValue& result, std::string& error);

// The metadata half, exposed because node parameters and diagnostics carry
// metadata maps without a tree around them.
bool SerializeMetadata (const MetadataMap& metadata, json::JsonValue& result, std::string& error);
bool DeserializeMetadata (const json::JsonValue& encoded, SharedMetadata& result, std::string& error);

} // namespace evp::nodegraph::data

#endif
