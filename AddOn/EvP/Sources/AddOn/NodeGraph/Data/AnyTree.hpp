#ifndef EVP_NODEGRAPH_DATA_ANYTREE_HPP
#define EVP_NODEGRAPH_DATA_ANYTREE_HPP

// Building and combining trees whose item type is known only at RUNTIME.
//
// Everything in DataTree/DataTreeOps is templated on the item type, which is
// what a node body wants: it knows it deals in doubles. The evaluator does not.
// It holds `TreeValue` on an edge, reads a port's declared `ItemType` out of the
// registry, and has to assemble whatever a lifted body returned. Without this
// file that assembly would be a switch over ten item types written out at every
// call site - and the one place somebody forgets to extend is a node family
// that silently stops working when a new item type is added.
//
// So: one erased builder that takes the declared ItemType up front and accepts
// erased `Value` items, and erased forms of the operations the evaluator needs.
// A Value whose type does not match the declared one is REFUSED, naming the
// site. That check is the whole reason this is a class rather than a cast: a
// tree declares one item type (7.4), and the erased path is where a node body
// returning the wrong thing would otherwise get in.

#include "NodeGraph/Data/DataTree.hpp"
#include "NodeGraph/Data/DataTreeOps.hpp"
#include "NodeGraph/Data/TreeIteration.hpp"

#include <memory>
#include <string>
#include <vector>

namespace evp::nodegraph::data {

class AnyTreeBuilder {
  public:
    explicit AnyTreeBuilder (ItemType itemType);
    ~AnyTreeBuilder ();

    AnyTreeBuilder (AnyTreeBuilder&&) noexcept;
    AnyTreeBuilder& operator= (AnyTreeBuilder&&) noexcept;
    AnyTreeBuilder (const AnyTreeBuilder&) = delete;
    AnyTreeBuilder& operator= (const AnyTreeBuilder&) = delete;

    ItemType Type () const
    {
        return itemType_;
    }

    // False when `value` is not an item of the declared type; `error` says
    // which type arrived instead. An Absent value is never an item - a body
    // that produced nothing says so with AddNull.
    bool Add (const DataPath& path, const Value& value, SharedMetadata metadata, std::string& error);
    bool Add (const DataPath& path, const Value& value, std::string& error)
    {
        return Add (path, value, nullptr, error);
    }

    void AddNull (const DataPath& path, SharedMetadata metadata = nullptr);
    void EnsureList (const DataPath& path);

    // Copies one whole list out of an existing tree of the same item type.
    bool AddList (const DataPath& path, const IDataList& list, std::string& error);

    TreeValue Finish () &&;

  private:
    struct Impl;

    ItemType itemType_;
    std::unique_ptr<Impl> impl_;
};

// The empty tree of a declared item type. What an input port hands a body when
// nothing is wired and nothing is internalised, so a node body never sees null.
TreeValue EmptyTreeValue (ItemType itemType);

// One tree from many, for a port that accepts several edges. §8.1 requires such
// a port to STATE its ordering, collision and metadata behaviour rather than
// letting edge insertion order become an undocumented contract, so the caller
// passes the declared contract in and the trees arrive in the order the port
// declares - this function does not decide either.
struct FanInContract {
    MergeCollision collision = MergeCollision::Append;
    MetadataMerge metadata = MetadataMerge::Error;
};

bool MergeTreeValues (const std::vector<TreeValue>& trees, const FanInContract& contract, TreeValue& result,
                      std::string& error);

// The argument one iteration hands a body, at the access the port declared:
//   Item -> the item itself, or nullopt for a null/absent site
//   List -> a tree of exactly one list, at the source path
//   Tree -> the whole input tree, unchanged and shared
//
// Slicing a List/Tree argument does not copy items; the one-list tree shares
// the source list.
bool SliceForAccess (const TreeValue& input, PortAccess access, const InputCursor& cursor, TreeValue& slice,
                     std::string& error);

std::optional<Value> ItemForCursor (const TreeValue& input, const InputCursor& cursor);

} // namespace evp::nodegraph::data

#endif
