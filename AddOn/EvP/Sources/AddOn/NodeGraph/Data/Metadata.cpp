#include "NodeGraph/Data/Metadata.hpp"

#include "NodeGraph/Data/AtomicValue.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace evp::nodegraph::data {
namespace {

bool IsValidSegment (const std::string& segment)
{
    if (segment.empty ())
        return false;
    for (char character : segment) {
        const unsigned char raw = static_cast<unsigned char> (character);
        if (character == '.')
            return false;
        if (raw < 0x20 || raw == 0x7F)
            return false;
    }
    return true;
}

bool SameDouble (double left, double right)
{
    return std::memcmp (&left, &right, sizeof (double)) == 0;
}

size_t HashDouble (double value)
{
    return std::hash<double> {}(value);
}

bool AllFinite (std::initializer_list<double> values)
{
    return std::all_of (values.begin (), values.end (), [] (double value) { return std::isfinite (value); });
}

bool IsFiniteValue (const MetadataValue& value)
{
    if (const auto* number = std::get_if<double> (&value))
        return std::isfinite (*number);
    if (const auto* point = std::get_if<Point3> (&value))
        return AllFinite ({ point->x, point->y, point->z });
    if (const auto* vector = std::get_if<Vector3> (&value))
        return AllFinite ({ vector->x, vector->y, vector->z });
    if (const auto* transform = std::get_if<Transform> (&value)) {
        for (const auto& row : transform->m) {
            for (double cell : row) {
                if (!std::isfinite (cell))
                    return false;
            }
        }
    }
    return true;
}

} // namespace

MetadataKey::MetadataKey (std::vector<std::string> segments) : segments_ (std::move (segments))
{
    hash_ = segments_.size ();
    for (const std::string& segment : segments_)
        CombineItemHash (hash_, std::hash<std::string> {}(segment));
}

std::optional<MetadataKey> MetadataKey::Create (std::vector<std::string> segments)
{
    if (segments.empty ())
        return std::nullopt;
    for (const std::string& segment : segments) {
        if (!IsValidSegment (segment))
            return std::nullopt;
    }
    return MetadataKey (std::move (segments));
}

std::optional<MetadataKey> MetadataKey::Parse (std::string_view dotted)
{
    if (dotted.empty ())
        return std::nullopt;

    std::vector<std::string> segments;
    size_t cursor = 0;
    while (true) {
        const size_t separator = dotted.find ('.', cursor);
        const std::string_view piece =
            separator == std::string_view::npos ? dotted.substr (cursor) : dotted.substr (cursor, separator - cursor);
        segments.emplace_back (piece);
        if (separator == std::string_view::npos)
            break;
        cursor = separator + 1;
    }
    return Create (std::move (segments));
}

bool MetadataKey::IsReserved () const
{
    return segments_.front () == kReservedNamespace;
}

std::string MetadataKey::ToString () const
{
    std::string text;
    for (size_t index = 0; index < segments_.size (); ++index) {
        if (index > 0)
            text += '.';
        text += segments_[index];
    }
    return text;
}

int MetadataKey::Compare (const MetadataKey& other) const
{
    const size_t shared = std::min (segments_.size (), other.segments_.size ());
    for (size_t index = 0; index < shared; ++index) {
        const int comparison = segments_[index].compare (other.segments_[index]);
        if (comparison != 0)
            return comparison < 0 ? -1 : 1;
    }
    if (segments_.size () == other.segments_.size ())
        return 0;
    return segments_.size () < other.segments_.size () ? -1 : 1;
}

bool operator== (const MetadataKey& left, const MetadataKey& right)
{
    return left.Hash () == right.Hash () && left.Segments () == right.Segments ();
}

bool operator!= (const MetadataKey& left, const MetadataKey& right)
{
    return !(left == right);
}

bool operator< (const MetadataKey& left, const MetadataKey& right)
{
    return left.Compare (right) < 0;
}

bool IsTransformableMetadataValue (const MetadataValue& value)
{
    return std::holds_alternative<Point3> (value) || std::holds_alternative<Vector3> (value) ||
           std::holds_alternative<Transform> (value);
}

size_t HashMetadataValue (const MetadataValue& value)
{
    size_t hash = value.index ();
    std::visit (
        [&hash] (const auto& held) {
            using T = std::decay_t<decltype (held)>;
            if constexpr (std::is_same_v<T, Point3>) {
                CombineItemHash (hash, HashDouble (held.x));
                CombineItemHash (hash, HashDouble (held.y));
                CombineItemHash (hash, HashDouble (held.z));
            }
            else if constexpr (std::is_same_v<T, Vector3>) {
                CombineItemHash (hash, HashDouble (held.x));
                CombineItemHash (hash, HashDouble (held.y));
                CombineItemHash (hash, HashDouble (held.z));
            }
            else if constexpr (std::is_same_v<T, Transform>) {
                for (const auto& row : held.m) {
                    for (double cell : row)
                        CombineItemHash (hash, HashDouble (cell));
                }
            }
            else if constexpr (std::is_same_v<T, ColourRgba>) {
                CombineItemHash (hash, held.red);
                CombineItemHash (hash, held.green);
                CombineItemHash (hash, held.blue);
                CombineItemHash (hash, held.alpha);
            }
            else {
                CombineItemHash (hash, std::hash<T> {}(held));
            }
        },
        value);
    return hash;
}

bool MetadataValuesEqual (const MetadataValue& left, const MetadataValue& right)
{
    if (left.index () != right.index ())
        return false;

    if (const auto* number = std::get_if<double> (&left))
        return SameDouble (*number, std::get<double> (right));
    if (const auto* point = std::get_if<Point3> (&left)) {
        const Point3& other = std::get<Point3> (right);
        return SameDouble (point->x, other.x) && SameDouble (point->y, other.y) && SameDouble (point->z, other.z);
    }
    if (const auto* vector = std::get_if<Vector3> (&left)) {
        const Vector3& other = std::get<Vector3> (right);
        return SameDouble (vector->x, other.x) && SameDouble (vector->y, other.y) && SameDouble (vector->z, other.z);
    }
    if (const auto* transform = std::get_if<Transform> (&left)) {
        const Transform& other = std::get<Transform> (right);
        for (size_t row = 0; row < 3; ++row) {
            for (size_t column = 0; column < 4; ++column) {
                if (!SameDouble (transform->m[row][column], other.m[row][column]))
                    return false;
            }
        }
        return true;
    }
    if (const auto* colour = std::get_if<ColourRgba> (&left)) {
        const ColourRgba& other = std::get<ColourRgba> (right);
        return colour->red == other.red && colour->green == other.green && colour->blue == other.blue &&
               colour->alpha == other.alpha;
    }
    if (const auto* flag = std::get_if<bool> (&left))
        return *flag == std::get<bool> (right);
    if (const auto* integer = std::get_if<int64_t> (&left))
        return *integer == std::get<int64_t> (right);
    return std::get<std::string> (left) == std::get<std::string> (right);
}

MetadataMap::MetadataMap (std::vector<MetadataEntry> entries) : entries_ (std::move (entries))
{
    hash_ = entries_.size ();
    for (const MetadataEntry& entry : entries_) {
        CombineItemHash (hash_, entry.key.Hash ());
        CombineItemHash (hash_, HashMetadataValue (entry.value));
        CombineItemHash (hash_, entry.transformable ? 1U : 0U);
    }
}

SharedMetadata MetadataMap::Empty ()
{
    static const SharedMetadata empty = SharedMetadata (new MetadataMap (std::vector<MetadataEntry> {}));
    return empty;
}

const MetadataEntry* MetadataMap::Find (const MetadataKey& key) const
{
    const auto found = std::lower_bound (
        entries_.begin (), entries_.end (), key,
        [] (const MetadataEntry& entry, const MetadataKey& probe) { return entry.key.Compare (probe) < 0; });
    if (found == entries_.end () || found->key != key)
        return nullptr;
    return &*found;
}

const MetadataValue* MetadataMap::FindValue (const MetadataKey& key) const
{
    const MetadataEntry* entry = Find (key);
    return entry == nullptr ? nullptr : &entry->value;
}

bool MetadataMap::Contains (const MetadataKey& key) const
{
    return Find (key) != nullptr;
}

bool operator== (const MetadataMap& left, const MetadataMap& right)
{
    if (&left == &right)
        return true;
    if (left.Hash () != right.Hash () || left.Size () != right.Size ())
        return false;
    for (size_t index = 0; index < left.Size (); ++index) {
        const MetadataEntry& a = left.Entries ()[index];
        const MetadataEntry& b = right.Entries ()[index];
        if (a.key != b.key || a.transformable != b.transformable || !MetadataValuesEqual (a.value, b.value))
            return false;
    }
    return true;
}

bool operator!= (const MetadataMap& left, const MetadataMap& right)
{
    return !(left == right);
}

const MetadataMap& MetadataOrEmpty (const SharedMetadata& metadata)
{
    static const SharedMetadata empty = MetadataMap::Empty ();
    return metadata == nullptr ? *empty : *metadata;
}

SharedMetadata NonNullMetadata (SharedMetadata metadata)
{
    return metadata == nullptr ? MetadataMap::Empty () : std::move (metadata);
}

bool MetadataEquals (const SharedMetadata& left, const SharedMetadata& right)
{
    if (left == right)
        return true;
    return MetadataOrEmpty (left) == MetadataOrEmpty (right);
}

bool MetadataBuilder::Set (MetadataKey key, MetadataValue value, bool transformable, std::string& error)
{
    if (Contains (key)) {
        error = "Duplicate metadata key: " + key.ToString ();
        return false;
    }
    if (!IsFiniteValue (value)) {
        error = "Non-finite metadata value for key: " + key.ToString ();
        return false;
    }
    if (transformable && !IsTransformableMetadataValue (value)) {
        error = "Metadata key is marked transformable but its value type has no transform: " + key.ToString ();
        return false;
    }

    entries_.push_back (MetadataEntry { std::move (key), std::move (value), transformable });
    return true;
}

void MetadataBuilder::Remove (const MetadataKey& key)
{
    entries_.erase (std::remove_if (entries_.begin (), entries_.end (),
                                    [&key] (const MetadataEntry& entry) { return entry.key == key; }),
                    entries_.end ());
}

bool MetadataBuilder::Contains (const MetadataKey& key) const
{
    return std::any_of (entries_.begin (), entries_.end (),
                        [&key] (const MetadataEntry& entry) { return entry.key == key; });
}

SharedMetadata MetadataBuilder::Finish () &&
{
    std::vector<MetadataEntry> entries = std::move (entries_);
    entries_.clear ();
    if (entries.empty ())
        return MetadataMap::Empty ();

    std::sort (entries.begin (), entries.end (),
               [] (const MetadataEntry& left, const MetadataEntry& right) { return left.key.Compare (right.key) < 0; });
    return SharedMetadata (new MetadataMap (std::move (entries)));
}

MetadataBuilder MetadataBuilder::From (const SharedMetadata& metadata)
{
    MetadataBuilder builder;
    builder.entries_ = MetadataOrEmpty (metadata).Entries ();
    return builder;
}

bool MergeMetadata (const SharedMetadata& left, const SharedMetadata& right, MetadataMerge policy,
                    SharedMetadata& result, std::string& error)
{
    const MetadataMap& a = MetadataOrEmpty (left);
    const MetadataMap& b = MetadataOrEmpty (right);
    if (b.IsEmpty ()) {
        result = NonNullMetadata (left);
        return true;
    }
    if (a.IsEmpty ()) {
        result = NonNullMetadata (right);
        return true;
    }

    MetadataBuilder builder = MetadataBuilder::From (left);
    for (const MetadataEntry& entry : b.Entries ()) {
        const MetadataEntry* existing = a.Find (entry.key);
        if (existing != nullptr) {
            // An identical value is not a conflict: two items carrying the same
            // provenance can merge without the node declaring a resolver.
            const bool identical =
                existing->transformable == entry.transformable && MetadataValuesEqual (existing->value, entry.value);
            if (identical)
                continue;
            switch (policy) {
                case MetadataMerge::Error:
                    error = "Conflicting metadata for key: " + entry.key.ToString ();
                    return false;
                case MetadataMerge::KeepLeft:
                    continue;
                case MetadataMerge::PreferRight:
                    builder.Remove (entry.key);
                    break;
            }
        }
        if (!builder.Set (entry.key, entry.value, entry.transformable, error))
            return false;
    }

    result = std::move (builder).Finish ();
    return true;
}

bool WithMetadataEntry (const SharedMetadata& metadata, MetadataEntry entry, SharedMetadata& result, std::string& error)
{
    MetadataBuilder builder = MetadataBuilder::From (metadata);
    builder.Remove (entry.key);
    if (!builder.Set (std::move (entry.key), std::move (entry.value), entry.transformable, error))
        return false;
    result = std::move (builder).Finish ();
    return true;
}

SharedMetadata WithoutMetadataKey (const SharedMetadata& metadata, const MetadataKey& key)
{
    const MetadataMap& map = MetadataOrEmpty (metadata);
    if (!map.Contains (key))
        return NonNullMetadata (metadata);

    MetadataBuilder builder = MetadataBuilder::From (metadata);
    builder.Remove (key);
    return std::move (builder).Finish ();
}

} // namespace evp::nodegraph::data
