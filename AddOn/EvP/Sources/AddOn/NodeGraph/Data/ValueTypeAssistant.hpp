#ifndef EVP_NODEGRAPH_DATA_VALUETYPEASSISTANT_HPP
#define EVP_NODEGRAPH_DATA_VALUETYPEASSISTANT_HPP

// Per-item-type behaviour, registered rather than inherited (HANDOFF 7.8,
// evidence in 56.10).
//
// The tree owns paths, order, nullness and metadata; it does NOT own what an
// item means. Hashing a point, printing an element reference, writing a
// polyline to a file - all of that lives here, behind one type-erased table, so
// that `IDataTree` stays structural and a new item type becomes hashable,
// printable and persistable in one place instead of in every operator that
// happens to touch it.
//
// This is the C++ answer to GH2's ITypeAssistant/TypeAssistantServer, minus the
// dynamic registration: Tapioca's item vocabulary is a closed enum (7.2), so
// the table is static, complete and checkable at startup rather than a map a
// plugin can leave a hole in.
//
// Assistants are pure and thread-safe: an immutable tree is read concurrently
// by the evaluator, and these run on whatever thread reads it.

#include "NodeGraph/Data/AtomicValue.hpp"
#include "NodeGraph/Json.hpp"

#include <string>

namespace evp::nodegraph::data {

struct ValueTypeAssistant {
    ItemType itemType = ItemType::Any;

    // The stable wire/file name. Same spelling as ItemTypeName.
    const char* name = "";

    size_t (*Hash) (const Value& item) = nullptr;
    bool (*Equals) (const Value& left, const Value& right) = nullptr;
    std::string (*Format) (const Value& item) = nullptr;

    // Null for an item type that is NOT persistable. Mesh is the one such type
    // today: a computed mesh belongs to the run cache, which 9.4 discards and
    // recomputes rather than migrating, and writing megabytes of triangles into
    // a graph document would make the document a cache. A tree of meshes
    // therefore fails serialisation loudly, naming the site, instead of
    // round-tripping into something that is not the mesh it started as.
    bool (*Serialize) (const Value& item, json::JsonValue& result, std::string& error) = nullptr;
    bool (*Deserialize) (const json::JsonValue& encoded, Value& result, std::string& error) = nullptr;

    bool CanSerialize () const
    {
        return Serialize != nullptr && Deserialize != nullptr;
    }
};

// The double codec every encoder in this layer shares.
//
// JSON has no NaN and no infinity, and its writer renders -0.0 as "-0", which
// its parser reads back as the INTEGER zero. Item and metadata equality are
// both bitwise (see ItemTraits<double>), so a value that survives the trip as
// +0.0 is a different value from the one that was saved, with a different hash.
// All four therefore travel as their IEEE names.
json::JsonValue EncodeJsonDouble (double value);
bool DecodeJsonDouble (const json::JsonValue& encoded, double& result, std::string& error);

// Null only for an unknown enum value; every declared ItemType has one.
const ValueTypeAssistant* FindValueTypeAssistant (ItemType type);
const ValueTypeAssistant* FindValueTypeAssistant (std::string_view name);

// Every registered assistant, in enum order. For diagnostics and for the
// startup check that the table is complete.
std::vector<const ValueTypeAssistant*> AllValueTypeAssistants ();

} // namespace evp::nodegraph::data

#endif
