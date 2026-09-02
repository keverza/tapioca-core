#include "NodeGraph/Data/DataPath.hpp"

#include "NodeGraph/Data/AtomicValue.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <stdexcept>

namespace evp::nodegraph::data {
namespace {

bool ParseSegment (std::string_view text, DataPath::Segment& result)
{
    // Hand-rolled instead of std::stoul: the input is untrusted, stoul throws
    // on failure, and it would accept a leading '-' and wrap it into a huge
    // unsigned segment. A negative segment is not a path (56.2).
    size_t begin = 0;
    size_t end = text.size ();
    while (begin < end && std::isspace (static_cast<unsigned char> (text[begin])) != 0)
        ++begin;
    while (end > begin && std::isspace (static_cast<unsigned char> (text[end - 1])) != 0)
        --end;
    if (begin >= end)
        return false;

    uint64_t value = 0;
    for (size_t index = begin; index < end; ++index) {
        const char character = text[index];
        if (character < '0' || character > '9')
            return false;
        value = value * 10 + static_cast<uint64_t> (character - '0');
        if (value > std::numeric_limits<DataPath::Segment>::max ())
            return false;
    }
    result = static_cast<DataPath::Segment> (value);
    return true;
}

} // namespace

DataPath::DataPath () : segments_ { 0 }
{
    Seal ();
}

DataPath::DataPath (std::initializer_list<Segment> segments) : segments_ (segments)
{
    if (segments_.empty ())
        throw std::invalid_argument ("A data path requires at least one segment.");
    Seal ();
}

DataPath::DataPath (std::vector<Segment> segments) : segments_ (std::move (segments))
{
    if (segments_.empty ())
        throw std::invalid_argument ("A data path requires at least one segment.");
    Seal ();
}

void DataPath::Seal ()
{
    hash_ = segments_.size ();
    for (Segment segment : segments_)
        CombineItemHash (hash_, std::hash<Segment> {}(segment));
}

std::optional<DataPath> DataPath::TryCreate (std::vector<Segment> segments)
{
    if (segments.empty ())
        return std::nullopt;
    return DataPath (std::move (segments));
}

std::optional<DataPath> DataPath::Parse (std::string_view text)
{
    size_t begin = 0;
    size_t end = text.size ();
    while (begin < end && std::isspace (static_cast<unsigned char> (text[begin])) != 0)
        ++begin;
    while (end > begin && std::isspace (static_cast<unsigned char> (text[end - 1])) != 0)
        --end;
    if (begin < end && text[begin] == '{') {
        if (text[end - 1] != '}')
            return std::nullopt;
        ++begin;
        --end;
    }

    const std::string_view body = text.substr (begin, end - begin);
    if (body.empty ())
        return std::nullopt;

    std::vector<Segment> segments;
    size_t cursor = 0;
    while (true) {
        const size_t separator = body.find (';', cursor);
        const std::string_view piece =
            separator == std::string_view::npos ? body.substr (cursor) : body.substr (cursor, separator - cursor);
        Segment segment = 0;
        if (!ParseSegment (piece, segment))
            return std::nullopt;
        segments.push_back (segment);
        if (separator == std::string_view::npos)
            break;
        cursor = separator + 1;
    }
    return DataPath (std::move (segments));
}

DataPath DataPath::Zero ()
{
    return DataPath ();
}

DataPath DataPath::Repeated (Segment segment, size_t length)
{
    if (length == 0)
        throw std::invalid_argument ("A data path requires at least one segment.");
    return DataPath (std::vector<Segment> (length, segment));
}

bool DataPath::IsZero () const
{
    return std::all_of (segments_.begin (), segments_.end (), [] (Segment segment) { return segment == 0; });
}

bool DataPath::StartsWith (const DataPath& prefix) const
{
    if (prefix.Length () > Length ())
        return false;
    return std::equal (prefix.segments_.begin (), prefix.segments_.end (), segments_.begin ());
}

DataPath DataPath::Append (Segment segment) const
{
    std::vector<Segment> segments = segments_;
    segments.push_back (segment);
    return DataPath (std::move (segments));
}

DataPath DataPath::Prepend (Segment segment) const
{
    std::vector<Segment> segments;
    segments.reserve (segments_.size () + 1);
    segments.push_back (segment);
    segments.insert (segments.end (), segments_.begin (), segments_.end ());
    return DataPath (std::move (segments));
}

DataPath DataPath::Concat (const DataPath& tail) const
{
    std::vector<Segment> segments = segments_;
    segments.insert (segments.end (), tail.segments_.begin (), tail.segments_.end ());
    return DataPath (std::move (segments));
}

DataPath DataPath::WithLast (Segment segment) const
{
    std::vector<Segment> segments = segments_;
    segments.back () = segment;
    return DataPath (std::move (segments));
}

std::optional<DataPath> DataPath::DropLast () const
{
    if (segments_.size () <= 1)
        return std::nullopt;
    return DataPath (std::vector<Segment> (segments_.begin (), segments_.end () - 1));
}

std::optional<DataPath> DataPath::DropFirst (size_t count) const
{
    if (count >= segments_.size ())
        return std::nullopt;
    return DataPath (std::vector<Segment> (segments_.begin () + static_cast<ptrdiff_t> (count), segments_.end ()));
}

std::optional<DataPath> DataPath::SubPath (size_t start, size_t length) const
{
    if (length == 0 || start >= segments_.size () || start + length > segments_.size ())
        return std::nullopt;
    const auto begin = segments_.begin () + static_cast<ptrdiff_t> (start);
    return DataPath (std::vector<Segment> (begin, begin + static_cast<ptrdiff_t> (length)));
}

DataPath DataPath::Increment () const
{
    std::vector<Segment> segments = segments_;
    ++segments.back ();
    return DataPath (std::move (segments));
}

int DataPath::Compare (const DataPath& other) const
{
    const size_t shared = std::min (segments_.size (), other.segments_.size ());
    for (size_t index = 0; index < shared; ++index) {
        if (segments_[index] != other.segments_[index])
            return segments_[index] < other.segments_[index] ? -1 : 1;
    }
    if (segments_.size () == other.segments_.size ())
        return 0;
    return segments_.size () < other.segments_.size () ? -1 : 1;
}

std::string DataPath::ToString () const
{
    std::string text = "{";
    for (size_t index = 0; index < segments_.size (); ++index) {
        if (index > 0)
            text += ';';
        text += std::to_string (segments_[index]);
    }
    text += '}';
    return text;
}

bool operator== (const DataPath& left, const DataPath& right)
{
    return left.Hash () == right.Hash () && left.Segments () == right.Segments ();
}

bool operator!= (const DataPath& left, const DataPath& right)
{
    return !(left == right);
}

bool operator< (const DataPath& left, const DataPath& right)
{
    return left.Compare (right) < 0;
}

bool operator<= (const DataPath& left, const DataPath& right)
{
    return left.Compare (right) <= 0;
}

bool operator> (const DataPath& left, const DataPath& right)
{
    return left.Compare (right) > 0;
}

bool operator>= (const DataPath& left, const DataPath& right)
{
    return left.Compare (right) >= 0;
}

size_t CommonPrefixLength (const DataPath& left, const DataPath& right)
{
    const size_t shared = std::min (left.Length (), right.Length ());
    size_t count = 0;
    while (count < shared && left[count] == right[count])
        ++count;
    return count;
}

} // namespace evp::nodegraph::data
