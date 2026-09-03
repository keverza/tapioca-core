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

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace evp::nodegraph::data {

// One tree per port, keyed by port id: what an edge actually carries, and the
// shape both the evaluator and a tree-native node body speak. Declared here
// rather than in NodeLifting so that NodeType can name it without depending on
// the lifting adapter it is an alternative to.
using TreeMap = std::map<std::string, TreeValue>;

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

// ---- Erased topology operations ---------------------------------------------
//
// The `tree.*` node family's whole job is reshaping - flatten, graft, simplify,
// shift - and every one of those is already written once, templated on the item
// type, in DataTreeOps.hpp. A node body does not have that template argument:
// it holds a `TreeValue` whose item type is the runtime enum. These wrappers are
// the same DispatchItemType switch AnyTreeBuilder uses, applied to the
// DataTreeOps functions instead of to construction, so a tree node body stays
// one call into this file rather than a second switch over the item vocabulary.
//
// Each one preserves whatever structural sharing the wrapped operation already
// does - SimplifyTree returns the SAME shared_ptr for a tree that is already
// simple, and that pointer identity survives the wrap, because rebuilding an
// unchanged tree here would silently defeat the sharing the templated layer was
// written to keep.

// False only when `input` is absent; the identity/no-op shape decisions are the
// wrapped operation's, not this wrapper's.
bool FlattenTreeValue (const TreeValue& input, TreeValue& result, std::string& error);
bool GraftTreeValue (const TreeValue& input, TreeValue& result, std::string& error);
bool SimplifyTreeValue (const TreeValue& input, TreeValue& result, std::string& error);

// `shift` and `policy` mean what ShiftTreePaths says; this only adds the
// runtime dispatch. Fails when the shift would empty a path or (under
// PathCollision::Error) fold two paths together - `error` names the path.
bool ShiftTreeValuePaths (const TreeValue& input, int32_t shift, PathCollision policy, TreeValue& result,
                          std::string& error);

// One tree as a tree of `target`, when CanWidenItemType allows it.
//
// Returns the input UNCHANGED - same pointer - when it is already that type or
// when no widening applies, so this is free on the overwhelmingly common path
// and callers need no "do I have to?" test of their own. It never narrows and
// never fails on a type it simply cannot convert: deciding whether that is an
// error belongs to the caller who knows what the port asked for.
TreeValue WidenTreeValue (const TreeValue& input, ItemType target);

// One tree of Doubles as a tree of Integers, rounded to nearest.
//
// The counterpart of WidenTreeValue, and deliberately NOT its mirror image:
// widening happens on any wire that needs it because nothing is lost, while this
// runs only where something asked for it by name. Returns the input unchanged
// when it is not a Double tree.
TreeValue RoundTreeValue (const TreeValue& input);

// Every branch's items in the opposite order, keeping paths, nullness and
// metadata with the items they belong to.
//
// Written against the erased interface rather than dispatched on the item type,
// because reversing does not read an item at all - it only moves it.
bool ReverseTreeValue (const TreeValue& input, TreeValue& result, std::string& error);

// Each branch's numbers remapped onto 0..1, smallest to largest.
//
// ⚠️ PER BRANCH, like every other per-branch operation in this layer, so a
// grafted tree normalises each branch against itself rather than against a
// range computed somewhere the user cannot see.
//
// ⚠️ AND A BRANCH WHOSE VALUES ARE ALL EQUAL BECOMES ALL ZEROS, not all ones and
// not a division by zero. There is no spread to place them in, and zero is the
// answer that stays continuous as the spread shrinks toward nothing.
//
// Returns the input unchanged when it is not a Double tree.
TreeValue NormaliseTreeValue (const TreeValue& input);

} // namespace evp::nodegraph::data

#endif
