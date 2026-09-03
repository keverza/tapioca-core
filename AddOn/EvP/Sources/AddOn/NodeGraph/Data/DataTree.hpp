#ifndef EVP_NODEGRAPH_DATA_DATATREE_HPP
#define EVP_NODEGRAPH_DATA_DATATREE_HPP

// The one value container an edge carries (HANDOFF 7.3-7.6, evidence in 56.4).
//
// A tree is a sorted, path-keyed collection of lists. Item, list and tree are
// the SAME storage: an item is one list of one at {0}, a flat list is one list
// at {0}, a tree is many. That is what lets the port declare its access (7.8.2)
// without the wire changing shape, and it is why there is no separate scalar
// type here to keep in sync.
//
// Trees are immutable once built. Every operation returns a new tree and shares
// the lists it did not touch, so graft/flatten/filter on a large tree copies
// path keys, not geometry.
//
// A tree cannot contain a tree: collection shape belongs to this container and
// nowhere else (7.2). DataTreeBuilder is the only way to make one.

#include "NodeGraph/Data/DataList.hpp"
#include "NodeGraph/Data/DataPath.hpp"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

namespace evp::nodegraph::data {

// The address of one item in one version of one tree. Not a durable identity:
// a filter or an insert moves the item that lives here. Workflows that need
// identity across transforms put it in the item's metadata (7.3).
struct DataSite {
    DataPath path;
    size_t index = 0;

    std::string ToString () const
    {
        return path.ToString () + "[" + std::to_string (index) + "]";
    }
};

class IDataTree {
  public:
    virtual ~IDataTree () = default;

    virtual ItemType Type () const = 0;
    virtual size_t ListCount () const = 0;
    virtual size_t ItemCount () const = 0;

    // Canonical order (see DataPath): the traversal order of every operation.
    virtual const std::vector<DataPath>& Paths () const = 0;

    virtual const IDataList& ListAt (size_t listIndex) const = 0;
    virtual const IDataList* FindList (const DataPath& path) const = 0;

    virtual size_t Hash () const = 0;

    bool IsEmpty () const
    {
        return ListCount () == 0;
    }
};

template <class T> class DataTreeBuilder;

template <class T> class DataTree final : public IDataTree {
  public:
    using SharedConstTree = std::shared_ptr<const DataTree<T>>;

    DataTree () = default;

    static SharedConstTree EmptyTree ();

    // The three shapes a node most often publishes. All of them are ordinary
    // trees; the names document intent, they do not select a storage type.
    static SharedConstTree FromItem (T value, SharedMetadata metadata = nullptr);
    static SharedConstTree FromList (std::vector<T> values);

    ItemType Type () const override
    {
        return ItemTraits<T>::Kind;
    }
    size_t ListCount () const override
    {
        return paths_.size ();
    }
    size_t ItemCount () const override
    {
        return itemCount_;
    }
    const std::vector<DataPath>& Paths () const override
    {
        return paths_;
    }
    const IDataList& ListAt (size_t listIndex) const override
    {
        return lists_[listIndex];
    }
    const IDataList* FindList (const DataPath& path) const override
    {
        return Find (path);
    }
    size_t Hash () const override
    {
        return hash_;
    }

    const DataPath& PathAt (size_t listIndex) const
    {
        return paths_[listIndex];
    }
    const DataList<T>& TypedListAt (size_t listIndex) const
    {
        return lists_[listIndex];
    }

    // Nullopt when the path holds no list. An EXISTING empty list is not the
    // same answer as a missing one (7.5), so this does not fold them together.
    std::optional<size_t> IndexOf (const DataPath& path) const;
    const DataList<T>* Find (const DataPath& path) const;

    // Nullopt when the site addresses no item, or addresses a null one.
    std::optional<T> ItemAt (const DataSite& site) const;

    bool Equals (const DataTree& other) const;

  private:
    friend class DataTreeBuilder<T>;

    void Seal ();

    // Kept in lockstep and sorted by path: paths_[i] addresses lists_[i].
    std::vector<DataPath> paths_;
    std::vector<DataList<T>> lists_;

    size_t itemCount_ = 0;
    size_t hash_ = 0;
};

// Transient, single-owner builder (7.6). Validates, sorts and seals in Finish;
// the builder is empty afterwards and is never published or retained.
//
// Items appended to the same path keep their append order; the paths themselves
// are sorted at the end, so a node body may emit paths in whatever order its
// loop produces without having to think about canonical order.
template <class T> class DataTreeBuilder {
  public:
    // The C++ type this builder stores. The erased AnyTreeBuilder recovers T
    // from the variant alternative it is visiting through this.
    using Item = T;

    void Add (DataPath path, T value, SharedMetadata metadata = nullptr)
    {
        ListFor (std::move (path)).Add (std::move (value), std::move (metadata));
    }

    void AddNull (DataPath path, SharedMetadata metadata = nullptr)
    {
        ListFor (std::move (path)).AddNull (std::move (metadata));
    }

    void AddItem (DataPath path, DataItem<T> item)
    {
        ListFor (std::move (path)).AddItem (std::move (item));
    }

    // An empty list is a fact about the tree, not an absence, so it has to be
    // stateable: a filter that rejects everything on one path produces this.
    void EnsureList (DataPath path)
    {
        ListFor (std::move (path));
    }

    void AddList (DataPath path, const DataList<T>& list)
    {
        DataListBuilder<T>& target = ListFor (std::move (path));
        for (size_t index = 0; index < list.Size (); ++index)
            target.AddItem (list.Item (index));
    }

    size_t ListCount () const
    {
        return entries_.size ();
    }

    std::shared_ptr<const DataTree<T>> Finish () &&;

  private:
    DataListBuilder<T>& ListFor (DataPath path);

    // Insertion-ordered while building, sorted in Finish. The index map keeps
    // repeated Add calls on the same path O(1) instead of a scan per item.
    std::vector<std::pair<DataPath, DataListBuilder<T>>> entries_;
    std::unordered_map<DataPath, size_t, DataPathHash> index_;
};

// What an edge carries and what a node result stores (7.4). The item type is
// repeated next to the pointer so a consumer can reject a mismatch without
// downcasting the tree.
struct TreeValue {
    ItemType itemType = ItemType::Any;
    std::shared_ptr<const IDataTree> tree;

    bool IsPresent () const
    {
        return tree != nullptr;
    }
};

template <class T> TreeValue MakeTreeValue (std::shared_ptr<const DataTree<T>> tree)
{
    return TreeValue { ItemTraits<T>::Kind, std::move (tree) };
}

// ---------------------------------------------------------------------------

template <class T> DataListBuilder<T>& DataTreeBuilder<T>::ListFor (DataPath path)
{
    const auto found = index_.find (path);
    if (found != index_.end ())
        return entries_[found->second].second;

    index_.emplace (path, entries_.size ());
    entries_.emplace_back (std::move (path), DataListBuilder<T> {});
    return entries_.back ().second;
}

template <class T> std::shared_ptr<const DataTree<T>> DataTreeBuilder<T>::Finish () &&
{
    auto entries = std::move (entries_);
    entries_.clear ();
    index_.clear ();

    std::sort (entries.begin (), entries.end (),
               [] (const auto& left, const auto& right) { return left.first.Compare (right.first) < 0; });

    auto tree = std::make_shared<DataTree<T>> ();
    tree->paths_.reserve (entries.size ());
    tree->lists_.reserve (entries.size ());
    for (auto& entry : entries) {
        tree->paths_.push_back (entry.first);
        tree->lists_.push_back (std::move (entry.second).Finish ());
    }
    tree->Seal ();
    return tree;
}

template <class T> void DataTree<T>::Seal ()
{
    itemCount_ = 0;
    hash_ = paths_.size ();
    CombineItemHash (hash_, static_cast<size_t> (ItemTraits<T>::Kind));
    for (size_t index = 0; index < paths_.size (); ++index) {
        itemCount_ += lists_[index].Size ();
        CombineItemHash (hash_, paths_[index].Hash ());
        CombineItemHash (hash_, lists_[index].Hash ());
    }
}

template <class T> std::shared_ptr<const DataTree<T>> DataTree<T>::EmptyTree ()
{
    static const std::shared_ptr<const DataTree<T>> empty = DataTreeBuilder<T> {}.Finish ();
    return empty;
}

template <class T> std::shared_ptr<const DataTree<T>> DataTree<T>::FromItem (T value, SharedMetadata metadata)
{
    DataTreeBuilder<T> builder;
    builder.Add (DataPath::Zero (), std::move (value), std::move (metadata));
    return std::move (builder).Finish ();
}

template <class T> std::shared_ptr<const DataTree<T>> DataTree<T>::FromList (std::vector<T> values)
{
    DataTreeBuilder<T> builder;
    if (values.empty ())
        builder.EnsureList (DataPath::Zero ());
    for (T& value : values)
        builder.Add (DataPath::Zero (), std::move (value));
    return std::move (builder).Finish ();
}

template <class T> std::optional<size_t> DataTree<T>::IndexOf (const DataPath& path) const
{
    const auto found =
        std::lower_bound (paths_.begin (), paths_.end (), path, [] (const DataPath& candidate, const DataPath& probe) {
            return candidate.Compare (probe) < 0;
        });
    if (found == paths_.end () || *found != path)
        return std::nullopt;
    return static_cast<size_t> (found - paths_.begin ());
}

template <class T> const DataList<T>* DataTree<T>::Find (const DataPath& path) const
{
    const std::optional<size_t> index = IndexOf (path);
    return index.has_value () ? &lists_[*index] : nullptr;
}

template <class T> std::optional<T> DataTree<T>::ItemAt (const DataSite& site) const
{
    const DataList<T>* list = Find (site.path);
    if (list == nullptr || site.index >= list->Size () || list->IsNullAt (site.index))
        return std::nullopt;
    return list->At (site.index);
}

template <class T> bool DataTree<T>::Equals (const DataTree& other) const
{
    if (this == &other)
        return true;
    if (hash_ != other.hash_ || paths_.size () != other.paths_.size () || itemCount_ != other.itemCount_)
        return false;

    for (size_t index = 0; index < paths_.size (); ++index) {
        if (paths_[index] != other.paths_[index])
            return false;
        if (!lists_[index].Equals (other.lists_[index]))
            return false;
    }
    return true;
}

} // namespace evp::nodegraph::data

#endif
