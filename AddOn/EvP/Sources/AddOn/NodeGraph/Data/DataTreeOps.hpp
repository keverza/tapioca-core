#ifndef EVP_NODEGRAPH_DATA_DATATREEOPS_HPP
#define EVP_NODEGRAPH_DATA_DATATREEOPS_HPP

// The topology operations of the data tree (HANDOFF 9.1-9.3).
//
// These are the operations every other tree node is written in terms of, so
// they are defined once here rather than re-derived per node. All of them obey
// the transform laws in 9.2: inputs are untouched, outputs are canonical, item
// order is preserved, and nullness and metadata travel with their item -
// flatten, graft, simplify, merge, filter and path remapping never drop
// metadata.
//
// What is deliberately NOT here: matching and replication. A tree does not lace
// itself (8.3). A node that pairs two trees declares its own policy; the
// container has no default that guesses.

#include "NodeGraph/Data/DataTree.hpp"

#include <string>
#include <type_traits>
#include <utility>

namespace evp::nodegraph::data {

// ---- Topology --------------------------------------------------------------

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

// ---- Values ----------------------------------------------------------------

// One output item per input item, at the same site. `convert` runs only for
// items that have a value: a null item stays null and keeps its metadata,
// because "no value" is not something a conversion applies to, and mapping it
// to a default is how a hole silently becomes a zero.
//
// Metadata is PRESERVED, which 9.3 makes the default for a one-to-one map. A
// node that means to produce or drop metadata rebuilds the item itself.
template <class T, class F>
auto MapTree (const DataTree<T>& tree, F convert)
    -> std::shared_ptr<const DataTree<std::decay_t<decltype (convert (std::declval<const T&> ()))>>>
{
    using U = std::decay_t<decltype (convert (std::declval<const T&> ()))>;

    DataTreeBuilder<U> builder;
    for (size_t listIndex = 0; listIndex < tree.ListCount (); ++listIndex) {
        const DataPath& path = tree.PathAt (listIndex);
        const DataList<T>& list = tree.TypedListAt (listIndex);
        builder.EnsureList (path);
        for (size_t index = 0; index < list.Size (); ++index) {
            if (list.IsNullAt (index))
                builder.AddNull (path, list.MetadataAt (index));
            else
                builder.Add (path, convert (list.At (index)), list.MetadataAt (index));
        }
    }
    return std::move (builder).Finish ();
}

// Keep the items `keep` accepts. `keep` receives the whole item, not just a
// value, so a filter can test nullness and metadata rather than only content.
//
// Paths survive by default, including ones left with nothing: an empty list is
// a fact (7.5), and an operation downstream needs to see that this path
// produced nothing rather than that it never existed. Pass removeEmptyLists
// when the caller genuinely wants those paths gone.
template <class T, class F>
std::shared_ptr<const DataTree<T>> FilterTree (const DataTree<T>& tree, F keep, bool removeEmptyLists = false)
{
    DataTreeBuilder<T> builder;
    for (size_t listIndex = 0; listIndex < tree.ListCount (); ++listIndex) {
        const DataPath& path = tree.PathAt (listIndex);
        const DataList<T>& list = tree.TypedListAt (listIndex);

        bool kept = false;
        for (size_t index = 0; index < list.Size (); ++index) {
            const DataItem<T> item = list.Item (index);
            if (!keep (item))
                continue;
            builder.AddItem (path, item);
            kept = true;
        }
        if (!kept && !removeEmptyLists)
            builder.EnsureList (path);
    }
    return std::move (builder).Finish ();
}

// The item count of every list, as one integer per path. The shape answer to
// "how much is on each branch" without materialising the items.
template <class T> std::shared_ptr<const DataTree<int64_t>> CountTreeItems (const DataTree<T>& tree)
{
    DataTreeBuilder<int64_t> builder;
    for (size_t listIndex = 0; listIndex < tree.ListCount (); ++listIndex)
        builder.Add (tree.PathAt (listIndex), static_cast<int64_t> (tree.TypedListAt (listIndex).Size ()));
    return std::move (builder).Finish ();
}

// ---- Paths -----------------------------------------------------------------

// What a path remapping does when two source paths land on one target.
enum class PathCollision {
    // Refuse. A remapping that folds two paths together usually means the
    // mapping is wrong, and the fold leaves no trace in the result.
    Error,

    // Items arrive in canonical source-path order, then item order.
    Append,
};

// Rewrite every path through `remap`, keeping each list's items and their
// order. `error` names the first colliding target path.
template <class T, class F>
bool MapTreePaths (const DataTree<T>& tree, F remap, PathCollision policy, std::shared_ptr<const DataTree<T>>& result,
                   std::string& error)
{
    DataTreeBuilder<T> builder;
    std::vector<DataPath> taken;
    taken.reserve (tree.ListCount ());

    for (size_t listIndex = 0; listIndex < tree.ListCount (); ++listIndex) {
        const DataPath target = remap (tree.PathAt (listIndex));
        if (policy == PathCollision::Error) {
            if (std::find (taken.begin (), taken.end (), target) != taken.end ()) {
                error = "Two paths map onto " + target.ToString ();
                return false;
            }
            taken.push_back (target);
        }
        builder.AddList (target, tree.TypedListAt (listIndex));
    }

    result = std::move (builder).Finish ();
    return true;
}

// Move every path along by `shift` segments: a positive shift drops that many
// leading segments, a negative one prepends that many zeroes.
//
// Unlike simplify, this is unconditional - it is what a user reaches for when
// they already know the shape they want - so a shift that would leave a path
// with no segments is an ERROR naming that path rather than a quietly clamped
// path (7.3: a path always has at least one segment).
template <class T>
bool ShiftTreePaths (const DataTree<T>& tree, int32_t shift, PathCollision policy,
                     std::shared_ptr<const DataTree<T>>& result, std::string& error)
{
    if (shift == 0) {
        return MapTreePaths (tree, [] (const DataPath& path) { return path; }, policy, result, error);
    }

    if (shift > 0) {
        const size_t drop = static_cast<size_t> (shift);
        for (size_t listIndex = 0; listIndex < tree.ListCount (); ++listIndex) {
            const DataPath& path = tree.PathAt (listIndex);
            if (drop >= path.Length ()) {
                error = "Shifting by " + std::to_string (shift) + " would empty the path " + path.ToString ();
                return false;
            }
        }
        return MapTreePaths (
            tree, [drop] (const DataPath& path) { return *path.DropFirst (drop); }, policy, result, error);
    }

    const size_t add = static_cast<size_t> (-static_cast<int64_t> (shift));
    return MapTreePaths (
        tree,
        [add] (const DataPath& path) {
            DataPath shifted = path;
            for (size_t step = 0; step < add; ++step)
                shifted = shifted.Prepend (0);
            return shifted;
        },
        policy, result, error);
}

// ---- Metadata --------------------------------------------------------------

// Add or replace one metadata entry on every item, null items included: a null
// item is a cell that exists, and provenance applies to it as much as to a
// valued one. Fails for the same reasons MetadataBuilder::Set does.
template <class T>
bool SetTreeMetadata (const DataTree<T>& tree, const MetadataEntry& entry, std::shared_ptr<const DataTree<T>>& result,
                      std::string& error)
{
    DataTreeBuilder<T> builder;
    for (size_t listIndex = 0; listIndex < tree.ListCount (); ++listIndex) {
        const DataPath& path = tree.PathAt (listIndex);
        const DataList<T>& list = tree.TypedListAt (listIndex);
        builder.EnsureList (path);
        for (size_t index = 0; index < list.Size (); ++index) {
            SharedMetadata metadata;
            if (!WithMetadataEntry (list.MetadataAt (index), entry, metadata, error))
                return false;
            if (list.IsNullAt (index))
                builder.AddNull (path, std::move (metadata));
            else
                builder.Add (path, list.At (index), std::move (metadata));
        }
    }

    result = std::move (builder).Finish ();
    return true;
}

// Remove one key from every item. Items that never carried it are untouched.
template <class T>
std::shared_ptr<const DataTree<T>> RemoveTreeMetadata (const DataTree<T>& tree, const MetadataKey& key)
{
    DataTreeBuilder<T> builder;
    for (size_t listIndex = 0; listIndex < tree.ListCount (); ++listIndex) {
        const DataPath& path = tree.PathAt (listIndex);
        const DataList<T>& list = tree.TypedListAt (listIndex);
        builder.EnsureList (path);
        for (size_t index = 0; index < list.Size (); ++index) {
            SharedMetadata metadata = WithoutMetadataKey (list.MetadataAt (index), key);
            if (list.IsNullAt (index))
                builder.AddNull (path, std::move (metadata));
            else
                builder.Add (path, list.At (index), std::move (metadata));
        }
    }
    return std::move (builder).Finish ();
}

} // namespace evp::nodegraph::data

#endif
