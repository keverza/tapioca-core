#ifndef EVP_NODEGRAPH_DATA_DATAPATH_HPP
#define EVP_NODEGRAPH_DATA_DATAPATH_HPP

// The address of one list inside a data tree (HANDOFF 7.3, evidence in 56.2).
//
// A path is an immutable, non-empty sequence of non-negative integers. {0} is
// legal and is the path a scalar and a flat list live at, so "the tree of one
// item" and "the tree of one list" are the same storage shape as any other
// tree - that single fact is what removes the separate item/list/tree value
// types the current runtime carries.
//
// Ordering is length-aware lexicographic: a prefix sorts before what extends
// it, so {0} < {0;1} < {1}. This is the canonical traversal order every tree
// operation, serialisation and browser projection uses, so it is defined once,
// here, rather than by whichever container happens to hold the paths.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace evp::nodegraph::data {

class DataPath {
  public:
    using Segment = uint32_t;

    // {0} - the default path, not an empty one. There is no empty path.
    DataPath ();
    DataPath (std::initializer_list<Segment> segments);

    // Throws std::invalid_argument on an empty segment list. Callers holding
    // untrusted input (deserialisation, browser payloads, script results) use
    // TryCreate or Parse instead and report the failure as a diagnostic.
    explicit DataPath (std::vector<Segment> segments);

    static std::optional<DataPath> TryCreate (std::vector<Segment> segments);

    // Accepts "{0;1;2}" and the bare "0;1;2". Whitespace around segments is
    // tolerated; anything else, including a negative or empty segment, fails.
    static std::optional<DataPath> Parse (std::string_view text);

    static DataPath Zero ();
    static DataPath Repeated (Segment segment, size_t length);

    size_t Length () const
    {
        return segments_.size ();
    }
    Segment operator[] (size_t index) const
    {
        return segments_[index];
    }
    Segment First () const
    {
        return segments_.front ();
    }
    Segment Last () const
    {
        return segments_.back ();
    }
    const std::vector<Segment>& Segments () const
    {
        return segments_;
    }
    bool IsZero () const;

    bool StartsWith (const DataPath& prefix) const;

    DataPath Append (Segment segment) const;
    DataPath Prepend (Segment segment) const;
    DataPath Concat (const DataPath& tail) const;
    DataPath WithLast (Segment segment) const;

    // Nullopt rather than a truncated path when the result would be empty: a
    // caller that drops the last segment of {0} has a real decision to make
    // (keep {0}, or refuse), and silently returning {0} makes graft/simplify
    // round-trips lie about what happened.
    std::optional<DataPath> DropLast () const;
    std::optional<DataPath> DropFirst (size_t count = 1) const;
    std::optional<DataPath> SubPath (size_t start, size_t length) const;

    // Rightmost segment + 1. Used to derive the next sibling path.
    DataPath Increment () const;

    // <0, 0, >0 in canonical order.
    int Compare (const DataPath& other) const;

    size_t Hash () const
    {
        return hash_;
    }

    // "{0;1;2}"
    std::string ToString () const;

  private:
    void Seal ();

    std::vector<Segment> segments_;
    size_t hash_ = 0;
};

bool operator== (const DataPath& left, const DataPath& right);
bool operator!= (const DataPath& left, const DataPath& right);
bool operator< (const DataPath& left, const DataPath& right);
bool operator<= (const DataPath& left, const DataPath& right);
bool operator> (const DataPath& left, const DataPath& right);
bool operator>= (const DataPath& left, const DataPath& right);

// Number of leading segments the two paths share. The basis of tree.simplify.
size_t CommonPrefixLength (const DataPath& left, const DataPath& right);

struct DataPathHash {
    size_t operator() (const DataPath& path) const
    {
        return path.Hash ();
    }
};

} // namespace evp::nodegraph::data

#endif
