#ifndef EVP_NODEGRAPH_DATA_DATATREEOPS_HPP
#define EVP_NODEGRAPH_DATA_DATATREEOPS_HPP

// The topology operations of the data tree (HANDOFF 9.1-9.3).
//
// These are the four that every other tree node is written in terms of, so they
// are defined once here rather than re-derived per node. All of them obey the
// transform laws in 9.2: inputs are untouched, outputs are canonical, item
// order is preserved, and nullness and metadata travel with their item -
// flatten, graft, simplify and merge never drop metadata.
//
// What is deliberately NOT here: matching and replication. A tree does not lace
// itself (8.3). A node that pairs two trees declares its own policy; the
// container has no default that guesses.

#include "NodeGraph/Data/DataTree.hpp"

#include <string>

namespace evp::nodegraph::data {

// Concatenate every item into {0} in canonical traversal order. Empty lists are
// discarded (they have nothing to contribute to a flat list), so a tree of
// nothing but empty lists flattens to a single empty list at {0}. The empty
// tree flattens to the empty tree: there is no list to flatten and inventing
// one would make "flatten" a way to conjure a path.
template <class T> std::shared_ptr<const DataTree<T>> FlattenTree (const DataTree<T>& tree)
{
    if (tree.ListCount () == 0)
        return DataTree<T>::EmptyTree ();

    DataTreeBuilder<T> builder;
    builder.EnsureList (DataPath::Zero ());
    for (size_t listIndex = 0; listIndex < tree.ListCount (); ++listIndex) {
        const DataList<T>& list = tree.TypedListAt (listIndex);
        for (size_t index = 0; index < list.Size (); ++index)
            builder.AddItem (DataPath::Zero (), list.Item (index));
    }
    return std::move (builder).Finish ();
}

// Every item at P[i] becomes the only item of a new list at P+i. An empty list
// stays where it is, empty: it has no item to graft, and dropping it would lose
// the fact that the path exists.
template <class T> std::shared_ptr<const DataTree<T>> GraftTree (const DataTree<T>& tree)
{
    DataTreeBuilder<T> builder;
    for (size_t listIndex = 0; listIndex < tree.ListCount (); ++listIndex) {
        const DataPath& path = tree.PathAt (listIndex);
        const DataList<T>& list = tree.TypedListAt (listIndex);
        if (list.IsEmpty ()) {
            builder.EnsureList (path);
            continue;
        }
        for (size_t index = 0; index < list.Size (); ++index)
            builder.AddItem (path.Append (static_cast<DataPath::Segment> (index)), list.Item (index));
    }
    return std::move (builder).Finish ();
}

// Drop the longest path prefix every list shares. A tree of one list simplifies
// to {0} rather than to nothing: a path must keep at least one segment, so the
// last shared segment is retained when removing it would leave an empty path.
//
// Takes the shared pointer rather than the tree so that "nothing to simplify"
// can return the SAME tree instead of a copy of it. Structural sharing is the
// point of an immutable tree, and simplify is the operation most often applied
// to a tree that is already simple.
template <class T> std::shared_ptr<const DataTree<T>> SimplifyTree (const std::shared_ptr<const DataTree<T>>& original)
{
    const DataTree<T>& tree = *original;
    if (tree.ListCount () == 0)
        return original;

    size_t shared = tree.PathAt (0).Length ();
    for (size_t listIndex = 1; listIndex < tree.ListCount (); ++listIndex)
        shared = std::min (shared, CommonPrefixLength (tree.PathAt (0), tree.PathAt (listIndex)));

    // Never strip a whole path, and never strip so much that two paths collide:
    // the shortest path bounds how much can go.
    size_t shortest = tree.PathAt (0).Length ();
    for (size_t listIndex = 1; listIndex < tree.ListCount (); ++listIndex)
        shortest = std::min (shortest, tree.PathAt (listIndex).Length ());
    const size_t removable = std::min (shared, shortest - 1);
    if (removable == 0)
        return original;

    DataTreeBuilder<T> builder;
    for (size_t listIndex = 0; listIndex < tree.ListCount (); ++listIndex) {
        const std::optional<DataPath> path = tree.PathAt (listIndex).DropFirst (removable);
        builder.AddList (*path, tree.TypedListAt (listIndex));
    }
    return std::move (builder).Finish ();
}

// What a merge does when both trees hold the same path.
enum class MergeCollision {
    // Refuse. The default: two nodes writing the same path is usually a wiring
    // mistake, and silently picking one of them hides it.
    Error,

    // Right's items follow left's in the same list.
    Append,

    // Right's list replaces left's entirely.
    Replace,
};

// Combine two complete trees. Paths present in only one side are carried over
// untouched; shared paths follow `policy`. `error` names the colliding path.
template <class T>
bool MergeTrees (const DataTree<T>& left, const DataTree<T>& right, MergeCollision policy,
                 std::shared_ptr<const DataTree<T>>& result, std::string& error)
{
    DataTreeBuilder<T> builder;
    for (size_t listIndex = 0; listIndex < left.ListCount (); ++listIndex) {
        const DataPath& path = left.PathAt (listIndex);
        const DataList<T>* other = right.Find (path);
        if (other != nullptr) {
            if (policy == MergeCollision::Error) {
                error = "Both trees hold the path " + path.ToString ();
                return false;
            }
            if (policy == MergeCollision::Replace)
                continue; // Right's list is added in the second pass.
        }
        builder.AddList (path, left.TypedListAt (listIndex));
    }
    for (size_t listIndex = 0; listIndex < right.ListCount (); ++listIndex)
        builder.AddList (right.PathAt (listIndex), right.TypedListAt (listIndex));

    result = std::move (builder).Finish ();
    return true;
}

} // namespace evp::nodegraph::data

#endif
