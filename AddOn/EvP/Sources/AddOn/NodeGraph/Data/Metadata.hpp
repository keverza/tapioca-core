#ifndef EVP_NODEGRAPH_DATA_METADATA_HPP
#define EVP_NODEGRAPH_DATA_METADATA_HPP

// Per-item semantic metadata (HANDOFF 7.7, evidence in 56.8).
//
// Metadata belongs to ONE item and travels with it through every topology
// transform. It is not graph metadata, editor layout, ParameterUi, node status
// or browser presentation state: those live on the document, and mixing them in
// here would make an editor nudge a semantic output change (7.10 makes metadata
// part of content equality, so it would invalidate caches).
//
// Keys are segmented rather than nested. Segments give a namespace without a
// recursive dictionary, so a map stays flat, hashable, sortable and cheap to
// share: every item without metadata points at ONE empty singleton, and items
// that survive a transform unchanged keep pointing at the same map.

#include "Annotation/DrawList.hpp"
#include "Geometry/GeometryEngine.hpp"
#include "Geometry/Transforms.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace evp::nodegraph::data {

using Point3 = geomsrv::annotation::Point3;
using Vector3 = geomsrv::engine::Vector3;
using Transform = geomsrv::engine::Transform;
using ColourRgba = geomsrv::annotation::ColourRgba;

// The first segment the runtime reserves. Third parties use a reverse-domain
// or otherwise registered first segment and must not write under this one.
inline constexpr std::string_view kReservedNamespace = "tapioca";

class MetadataKey {
  public:
    // Every segment must be non-empty and free of '.' (the join character) and
    // control characters. Nullopt rather than a throw: keys arrive from scripts
    // and browser payloads, and a bad key is a node diagnostic, not a crash.
    static std::optional<MetadataKey> Create (std::vector<std::string> segments);

    // "tapioca.source.elementGuid"
    static std::optional<MetadataKey> Parse (std::string_view dotted);

    const std::vector<std::string>& Segments () const
    {
        return segments_;
    }
    const std::string& First () const
    {
        return segments_.front ();
    }
    bool IsReserved () const;

    std::string ToString () const;
    int Compare (const MetadataKey& other) const;
    size_t Hash () const
    {
        return hash_;
    }

  private:
    explicit MetadataKey (std::vector<std::string> segments);

    std::vector<std::string> segments_;
    size_t hash_ = 0;
};

bool operator== (const MetadataKey& left, const MetadataKey& right);
bool operator!= (const MetadataKey& left, const MetadataKey& right);
bool operator< (const MetadataKey& left, const MetadataKey& right);

// Deliberately small. Every member is a value with a defined hash, a defined
// equality and (for the geometric three) a defined transform, so the whole map
// stays hashable and serialisable. Byte arrays and free-form JSON are excluded
// until a real contract needs them (7.7); adding one is an append at the end of
// this variant, because the index is the persisted discriminator.
using MetadataValue = std::variant<bool, int64_t, double, std::string, Point3, Vector3, Transform, ColourRgba>;

struct MetadataEntry {
    MetadataKey key;
    MetadataValue value;

    // Whether a geometry transform applied to the item must also be applied to
    // this value. Legal only for Point3, Vector3 and Transform: a node may not
    // infer transformability from how a key is spelled, and a flag on a string
    // is a contradiction, so the builder rejects it.
    bool transformable = false;
};

bool IsTransformableMetadataValue (const MetadataValue& value);
size_t HashMetadataValue (const MetadataValue& value);
bool MetadataValuesEqual (const MetadataValue& left, const MetadataValue& right);

class MetadataMap;
using SharedMetadata = std::shared_ptr<const MetadataMap>;

// Immutable, sorted by key, unique keys, shareable across items and across
// trees of different item types.
class MetadataMap {
  public:
    // One process-wide empty map. Items without metadata all point here, so
    // "has metadata" costs a pointer comparison and an empty map costs nothing
    // per item. Never returns null - every accessor in this layer returns a
    // usable map rather than making callers null-check.
    static SharedMetadata Empty ();

    bool IsEmpty () const
    {
        return entries_.empty ();
    }
    size_t Size () const
    {
        return entries_.size ();
    }
    const std::vector<MetadataEntry>& Entries () const
    {
        return entries_;
    }

    const MetadataEntry* Find (const MetadataKey& key) const;
    const MetadataValue* FindValue (const MetadataKey& key) const;
    bool Contains (const MetadataKey& key) const;

    size_t Hash () const
    {
        return hash_;
    }

  private:
    friend class MetadataBuilder;
    explicit MetadataMap (std::vector<MetadataEntry> entries);

    std::vector<MetadataEntry> entries_;
    size_t hash_ = 0;
};

bool operator== (const MetadataMap& left, const MetadataMap& right);
bool operator!= (const MetadataMap& left, const MetadataMap& right);

// True when both describe the same metadata, including when one or both are
// null pointers standing in for the empty map.
bool MetadataEquals (const SharedMetadata& left, const SharedMetadata& right);

// Null-safe: a null pointer reads as the empty map.
const MetadataMap& MetadataOrEmpty (const SharedMetadata& metadata);
SharedMetadata NonNullMetadata (SharedMetadata metadata);

// Transient, single-owner builder. Never published, cached or shared.
class MetadataBuilder {
  public:
    // Fails on a duplicate key, a non-finite number, or `transformable` on a
    // value type that has no transform. `error` names the offending key.
    bool Set (MetadataKey key, MetadataValue value, bool transformable, std::string& error);
    bool Set (MetadataKey key, MetadataValue value, std::string& error)
    {
        return Set (std::move (key), std::move (value), false, error);
    }

    void Remove (const MetadataKey& key);
    bool Contains (const MetadataKey& key) const;
    size_t Size () const
    {
        return entries_.size ();
    }

    // Sorts, seals and empties the builder. Returns the empty singleton when
    // nothing was added, so an item never carries a distinct empty map.
    SharedMetadata Finish () &&;

    static MetadataBuilder From (const SharedMetadata& metadata);

  private:
    std::vector<MetadataEntry> entries_;
};

// What a many-to-one operation does when both sides carry the same key.
//
// Error is the default on purpose (9.3): last-writer-wins silently picks one
// item's provenance for a merged item, and the node that wanted a rule can say
// which rule it wanted.
enum class MetadataMerge {
    Error,
    KeepLeft,
    PreferRight,
};

bool MergeMetadata (const SharedMetadata& left, const SharedMetadata& right, MetadataMerge policy,
                    SharedMetadata& result, std::string& error);

// Adds or replaces one entry. Fails on the same grounds as MetadataBuilder::Set
// (non-finite value, transformable flag on a non-transformable type); the
// caller reports that as a node diagnostic rather than silently keeping the
// item as it was.
bool WithMetadataEntry (const SharedMetadata& metadata, MetadataEntry entry, SharedMetadata& result,
                        std::string& error);
SharedMetadata WithoutMetadataKey (const SharedMetadata& metadata, const MetadataKey& key);

} // namespace evp::nodegraph::data

#endif
