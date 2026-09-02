#ifndef EVP_NODEGRAPH_DATA_DATALIST_HPP
#define EVP_NODEGRAPH_DATA_DATALIST_HPP

// One path's ordered items - the container GH2 calls a twig, named here for
// what it is (HANDOFF 7.3, evidence in 56.5).
//
// A list is immutable and holds three parallel arrays: values, null flags and
// per-item metadata. Parallel rather than a vector of item structs because the
// common list carries neither nulls nor metadata, and the two side arrays then
// stay empty: a list of a million doubles costs a million doubles, not a
// million optionals with a shared_ptr each. Nullness cannot live in the value
// slot anyway - a null double is not any particular double (56.3 shows GH2
// reaching the same conclusion and keeping a parallel bool[]).
//
// Null, empty and absent stay distinct here (7.5): an empty list is a real list
// with zero items, a null item is a present cell with a declared type and no
// value, and absence is the business of the port, not of this container.

#include "NodeGraph/Data/AtomicValue.hpp"
#include "NodeGraph/Data/Metadata.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace evp::nodegraph::data {

// One item: its value (or nullness) plus its metadata. GH2's "pear".
template <class T> class DataItem {
  public:
    DataItem () : metadata_ (MetadataMap::Empty ())
    {
    }
    explicit DataItem (T value, SharedMetadata metadata = nullptr)
        : value_ (std::move (value)), metadata_ (NonNullMetadata (std::move (metadata)))
    {
    }

    static DataItem Null (SharedMetadata metadata = nullptr)
    {
        DataItem item;
        item.metadata_ = NonNullMetadata (std::move (metadata));
        return item;
    }

    bool IsNull () const
    {
        return !value_.has_value ();
    }

    // Precondition: !IsNull(). Throws rather than returning a default, because
    // a silently defaulted null is exactly the bug 7.5 exists to prevent.
    const T& Value () const
    {
        if (!value_.has_value ())
            throw std::logic_error ("Data item is null and has no value.");
        return *value_;
    }

    const SharedMetadata& Metadata () const
    {
        return metadata_;
    }

  private:
    std::optional<T> value_;
    SharedMetadata metadata_;
};

// Type-erased structural surface: what the evaluator, the browser projection
// and the generic tree operators need without knowing T.
class IDataList {
  public:
    virtual ~IDataList () = default;

    virtual ItemType Type () const = 0;
    virtual size_t Size () const = 0;
    virtual bool IsNullAt (size_t index) const = 0;
    virtual const SharedMetadata& MetadataAt (size_t index) const = 0;

    // Absent Value for a null item, so a projection can tell the two apart.
    virtual std::optional<Value> ValueAt (size_t index) const = 0;

    virtual size_t Hash () const = 0;

    bool IsEmpty () const
    {
        return Size () == 0;
    }
};

template <class T> class DataListBuilder;

template <class T> class DataList final : public IDataList {
  public:
    DataList () = default;

    static const DataList& EmptyList ()
    {
        static const DataList empty;
        return empty;
    }

    ItemType Type () const override
    {
        return ItemTraits<T>::Kind;
    }
    size_t Size () const override
    {
        return values_.size ();
    }
    bool IsNullAt (size_t index) const override
    {
        return !nulls_.empty () && nulls_[index] != 0;
    }
    const SharedMetadata& MetadataAt (size_t index) const override
    {
        if (metadata_.empty ())
            return EmptyMetadataSlot ();
        return metadata_[index];
    }
    std::optional<Value> ValueAt (size_t index) const override
    {
        if (IsNullAt (index))
            return std::nullopt;
        return ToValue (values_[index]);
    }
    size_t Hash () const override
    {
        return hash_;
    }

    const T& At (size_t index) const
    {
        if (IsNullAt (index))
            throw std::logic_error ("Data item is null and has no value.");
        return values_[index];
    }

    DataItem<T> Item (size_t index) const
    {
        if (IsNullAt (index))
            return DataItem<T>::Null (MetadataAt (index));
        return DataItem<T> (values_[index], MetadataAt (index));
    }

    bool HasNulls () const
    {
        return !nulls_.empty ();
    }
    bool HasMetadata () const
    {
        return !metadata_.empty ();
    }

    // this then other, item order preserved, metadata and nullness carried.
    DataList Concat (const DataList& other) const;

    // count items from start; a range past the end is clamped, an empty range
    // yields the empty list. Slicing is an address operation, not a query with
    // a failure mode, so it never throws.
    DataList Slice (size_t start, size_t count) const;

    bool Equals (const DataList& other) const;

  private:
    friend class DataListBuilder<T>;

    void Seal ();

    static const SharedMetadata& EmptyMetadataSlot ()
    {
        static const SharedMetadata empty = MetadataMap::Empty ();
        return empty;
    }

    std::vector<T> values_;

    // Both stay EMPTY until the list actually needs them, and are then filled
    // for every slot. So `nulls_.empty()` means "no nulls anywhere", never
    // "not populated yet", and no accessor has to branch on a half state.
    std::vector<uint8_t> nulls_;
    std::vector<SharedMetadata> metadata_;

    size_t hash_ = 0;
};

// Transient, single-owner builder. Never published, cached, shared between node
// bodies or retained in the document.
template <class T> class DataListBuilder {
  public:
    void Add (T value, SharedMetadata metadata = nullptr)
    {
        values_.push_back (std::move (value));
        nulls_.push_back (0);
        metadata_.push_back (NonNullMetadata (std::move (metadata)));
    }

    void AddNull (SharedMetadata metadata = nullptr)
    {
        values_.push_back (T {});
        nulls_.push_back (1);
        metadata_.push_back (NonNullMetadata (std::move (metadata)));
    }

    void AddItem (DataItem<T> item)
    {
        if (item.IsNull ())
            AddNull (item.Metadata ());
        else
            Add (item.Value (), item.Metadata ());
    }

    void Reserve (size_t count)
    {
        values_.reserve (count);
        nulls_.reserve (count);
        metadata_.reserve (count);
    }

    size_t Size () const
    {
        return values_.size ();
    }

    // Seals the list and empties the builder.
    DataList<T> Finish () &&;

  private:
    std::vector<T> values_;
    std::vector<uint8_t> nulls_;
    std::vector<SharedMetadata> metadata_;
};

// ---------------------------------------------------------------------------

template <class T> void DataList<T>::Seal ()
{
    // Drop the side arrays when they carry nothing, so the cheap list stays
    // cheap however it was built.
    if (std::find (nulls_.begin (), nulls_.end (), uint8_t { 1 }) == nulls_.end ())
        nulls_.clear ();

    bool anyMetadata = false;
    for (const SharedMetadata& metadata : metadata_) {
        if (metadata != nullptr && !metadata->IsEmpty ()) {
            anyMetadata = true;
            break;
        }
    }
    if (!anyMetadata)
        metadata_.clear ();
    else {
        for (SharedMetadata& metadata : metadata_)
            metadata = NonNullMetadata (std::move (metadata));
    }

    hash_ = values_.size ();
    CombineItemHash (hash_, static_cast<size_t> (ItemTraits<T>::Kind));
    for (size_t index = 0; index < values_.size (); ++index) {
        if (IsNullAt (index))
            CombineItemHash (hash_, 0x6E756C6CU); // "null"
        else
            CombineItemHash (hash_, ItemTraits<T>::Hash (values_[index]));
        CombineItemHash (hash_, MetadataAt (index)->Hash ());
    }
}

template <class T> DataList<T> DataListBuilder<T>::Finish () &&
{
    DataList<T> list;
    list.values_ = std::move (values_);
    list.nulls_ = std::move (nulls_);
    list.metadata_ = std::move (metadata_);
    values_.clear ();
    nulls_.clear ();
    metadata_.clear ();
    list.Seal ();
    return list;
}

template <class T> DataList<T> DataList<T>::Concat (const DataList& other) const
{
    DataListBuilder<T> builder;
    builder.Reserve (Size () + other.Size ());
    for (size_t index = 0; index < Size (); ++index)
        builder.AddItem (Item (index));
    for (size_t index = 0; index < other.Size (); ++index)
        builder.AddItem (other.Item (index));
    return std::move (builder).Finish ();
}

template <class T> DataList<T> DataList<T>::Slice (size_t start, size_t count) const
{
    DataListBuilder<T> builder;
    if (start >= Size () || count == 0)
        return std::move (builder).Finish ();

    const size_t end = std::min (Size (), start + count);
    builder.Reserve (end - start);
    for (size_t index = start; index < end; ++index)
        builder.AddItem (Item (index));
    return std::move (builder).Finish ();
}

template <class T> bool DataList<T>::Equals (const DataList& other) const
{
    if (this == &other)
        return true;
    if (hash_ != other.hash_ || Size () != other.Size ())
        return false;

    for (size_t index = 0; index < Size (); ++index) {
        if (IsNullAt (index) != other.IsNullAt (index))
            return false;
        if (!IsNullAt (index) && !ItemTraits<T>::Equals (values_[index], other.values_[index]))
            return false;
        if (!MetadataEquals (MetadataAt (index), other.MetadataAt (index)))
            return false;
    }
    return true;
}

} // namespace evp::nodegraph::data

#endif
