#ifndef EVP_NODEGRAPH_DATA_ATOMICVALUE_HPP
#define EVP_NODEGRAPH_DATA_ATOMICVALUE_HPP

// The atomic item vocabulary of the data-tree layer (HANDOFF §7.2).
//
// A tree stores atomic items and nothing else: collection shape lives in the
// tree, never inside an item. That is why this enum is NOT the existing
// `ValueType`. `ValueType` carries two members the tree contract forbids -
// `List`, which would let a tree hold a collection inside a cell, and `Absent`,
// which the current runtime overloads as both "no value" and "wildcard type"
// (§7.4 requires that overload to be removed). Mapping between the two enums is
// explicit and lossy in one direction on purpose, so the migration cannot
// smuggle a list into a tree cell by accident.
//
// Item behaviour beyond hashing and equality (formatting, serialisation,
// conversion, transform, preview) belongs to the registered ValueTypeAssistant
// (§7.8), not here.

#include "NodeGraph/Value.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace evp::nodegraph::data {

// Wire and file contract: the numbers are persisted in graph documents and in
// cache keys. Append only; never renumber.
enum class ItemType : uint8_t {
    Bool = 1,
    Integer = 2,
    Double = 3,
    String = 4,
    Point3 = 5,
    Polyline = 6,
    Polygon = 7,
    Mesh = 8,
    ElementRef = 9,

    // An explicitly declared heterogeneous tree, for inspection and grouping
    // nodes only (§7.4). It is a declared item type, not the absence of one,
    // and not the default escape from port typing.
    Any = 255,
};

const char* ItemTypeName (ItemType type);
std::optional<ItemType> ItemTypeFromName (std::string_view name);

// Absent/List have no item type: a tree cannot express either (see above).
// ---- Widening ---------------------------------------------------------------

// Whether an item of `from` may become one of `to` with nothing lost.
//
// ⚠️ ONE DIRECTION ONLY, AND THE ASYMMETRY IS THE POINT. An integer is a double
// exactly, so a wire may carry one into the other silently. A double is NOT an
// integer, and the reason it needs an explicit node rather than a silent cast is
// that there are FOUR defensible answers - nearest, floor, ceiling, truncate -
// and picking one on the user's behalf means 2.5 quietly becoming a 2 or a 3
// with nothing on the canvas saying which. This is the ordinary rule of every
// statically typed language, and it is here for the ordinary reason.
//
// Any is not in the lattice: a wildcard accepts anything already, so it needs no
// conversion and offers none.
bool CanWidenItemType (ItemType from, ItemType to);

// The same rule in the vocabulary the catalog and the edit rules speak, so the
// editor and the runtime cannot come to different conclusions about one wire.
bool CanWidenValueType (ValueType from, ValueType to);

std::optional<ItemType> ItemTypeFromValueType (ValueType type);
std::optional<ValueType> ValueTypeFromItemType (ItemType type);

// Per-item hashing and equality for the types a tree may store.
//
// Mesh is compared and hashed by POINTER, matching Value::Hash. Meshes are
// published as immutable shared_ptr and reused rather than rebuilt, so equal
// pointers really do mean equal content; the converse does not hold, so two
// separately built identical meshes compare unequal and merely cause a
// recompute. Content equality for meshes arrives with the type assistants.
template <class T> struct ItemTraits;

#define EVP_DECLARE_ITEM_TRAITS(TYPE, KIND)                                                                            \
    template <> struct ItemTraits<TYPE> {                                                                              \
        static constexpr ItemType Kind = ItemType::KIND;                                                               \
        static size_t Hash (const TYPE& value);                                                                        \
        static bool Equals (const TYPE& left, const TYPE& right);                                                      \
    }

EVP_DECLARE_ITEM_TRAITS (bool, Bool);
EVP_DECLARE_ITEM_TRAITS (int64_t, Integer);
EVP_DECLARE_ITEM_TRAITS (double, Double);
EVP_DECLARE_ITEM_TRAITS (std::string, String);
EVP_DECLARE_ITEM_TRAITS (Point3, Point3);
EVP_DECLARE_ITEM_TRAITS (Polyline, Polyline);
EVP_DECLARE_ITEM_TRAITS (Polygon, Polygon);
EVP_DECLARE_ITEM_TRAITS (Value::ImmutableMesh, Mesh);
EVP_DECLARE_ITEM_TRAITS (ArchicadElementRef, ElementRef);
EVP_DECLARE_ITEM_TRAITS (Value, Any);

#undef EVP_DECLARE_ITEM_TRAITS

// True when a type-erased `Value` is a legal tree item: it holds one atomic
// value, not `Absent` and not a `List`. `ItemType::Any` trees are the only
// place a `Value` is stored, and they still may not contain a collection, so
// every insertion into such a tree is checked with this.
//
// ⚠️ THIS NOW EXCLUDES ONLY `Absent`, AND THAT IS A NARROWING WORTH KNOWING.
// It used to exclude `List` as well, because a Value could carry one and a
// recursive walk over an untrusted nested list is a stack overflow inside
// Archicad's process. A Value has no List alternative any more - collection
// shape belongs to the tree and to `Argument`, and nothing else - so the case
// is unreachable rather than unhandled. The runtime depth ceiling that guarded
// it went with it.
bool IsAtomicValue (const Value& value);

// Type-erased read of one stored item, for the browser projection and the
// generic tree operators. The reverse direction (Value -> T) belongs to the
// node input binding, not here, because it has to report a type mismatch.
Value ToValue (bool item);
Value ToValue (int64_t item);
Value ToValue (double item);
Value ToValue (const std::string& item);
Value ToValue (const Point3& item);
Value ToValue (const Polyline& item);
Value ToValue (const Polygon& item);
Value ToValue (const Value::ImmutableMesh& item);
Value ToValue (const ArchicadElementRef& item);
Value ToValue (const Value& item);

// Hash mixer shared by every structure in this layer. One definition so a
// tree hash, a list hash and a metadata hash cannot drift apart.
void CombineItemHash (size_t& seed, size_t value);

} // namespace evp::nodegraph::data

#endif
