#include "Edit/EditLandscapeJsonInternal.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>

namespace tapioca::geometry::landscape_json {
Reader Reader::Field (const std::string& name) const
{
    const auto* member = value.Find (name);
    if (!member)
        Fail (path + "." + name, "required field is absent");
    return Reader (*member, path + "." + name);
}
void Reader::Fields (std::initializer_list<const char*> names, std::initializer_list<const char*> optional) const
{
    const auto* object = value.AsObject ();
    if (!object)
        Fail (path, "object required");
    std::set<std::string> allowed;
    for (const auto* name : names) {
        allowed.insert (name);
        Field (name);
    }
    for (const auto* name : optional)
        allowed.insert (name);
    for (const auto& [name, member] : *object) {
        (void) member;
        if (!allowed.count (name))
            Fail (path + "." + name, "unknown field");
    }
}
const Array& Reader::Items () const
{
    const auto* array = value.AsArray ();
    if (!array || array->size () > 65536)
        Fail (path, "bounded array required (maximum 65536 items)");
    return *array;
}
std::string Reader::Text () const
{
    std::string text;
    if (!value.AsString (text))
        Fail (path, "string required");
    if (text.find ('\0') != std::string::npos)
        Fail (path, "embedded NUL is not allowed");
    return text;
}
Id Reader::Identifier () const
{
    const auto text = Text ();
    if (text.empty () || (text.size () > 1 && text.front () == '0') ||
        std::any_of (text.begin (), text.end (), [] (char c) { return c < '0' || c > '9'; }))
        Fail (path, "canonical unsigned decimal string required");
    Id id = 0;
    const auto parsed = std::from_chars (text.data (), text.data () + text.size (), id);
    if (parsed.ec != std::errc {} || parsed.ptr != text.data () + text.size ())
        Fail (path, "unsigned 64-bit overflow");
    return id;
}
double Reader::Number () const
{
    double number = 0;
    if (!value.AsDouble (number) || !std::isfinite (number))
        Fail (path, "finite number required");
    return number;
}
int Reader::Integer () const
{
    const double number = Number ();
    if (std::floor (number) != number || number < std::numeric_limits<int>::min () ||
        number > std::numeric_limits<int>::max ())
        Fail (path, "32-bit integer required");
    return static_cast<int> (number);
}
bool Reader::Flag () const
{
    bool flag = false;
    if (!value.AsBool (flag))
        Fail (path, "boolean required");
    return flag;
}
Vec3 Reader::Point () const
{
    Fields ({ "x", "y", "z" });
    return { Field ("x").Number (), Field ("y").Number (), Field ("z").Number () };
}
std::vector<Id> Reader::Ids () const
{
    std::vector<Id> ids;
    size_t i = 0;
    for (const auto& item : Items ())
        ids.push_back (Reader (item, path + "[" + std::to_string (i++) + "]").Identifier ());
    return ids;
}
MaterialSpec Reader::Material () const
{
    Fields ({ "preset", "fillGuid", "fillName", "surfaceGuid", "surfaceName", "hatchDirection", "hatchDensity" });
    MaterialSpec material;
    const int preset = Field ("preset").Integer ();
    if (preset < 0 || preset > int (MaterialPreset::Grass))
        Fail (path + ".preset", "unknown material preset");
    material.preset = MaterialPreset (preset);
    const auto guid = [&] (const char* field) {
        auto text = Field (field).Text ();
        if (!text.empty () && !CanonicalLandscapeGuid (text, text))
            Fail (path + "." + field, "canonical non-nil GUID required");
        return text;
    };
    material.fillGuid = guid ("fillGuid");
    material.fillName = Field ("fillName").Text ();
    material.surfaceGuid = guid ("surfaceGuid");
    material.surfaceName = Field ("surfaceName").Text ();
    material.hatchDirection = Field ("hatchDirection").Number ();
    material.hatchDensity = Field ("hatchDensity").Number ();
    return material;
}
RegionDescriptor Reader::Descriptor () const
{
    Fields ({ "object", "sources", "operations", "boundaries" });
    RegionDescriptor descriptor;
    descriptor.object = Field ("object").Identifier ();
    descriptor.sources = Field ("sources").Ids ();
    descriptor.operations = Field ("operations").Ids ();
    size_t i = 0;
    for (const auto& item : Field ("boundaries").Items ()) {
        Reader boundary (item, path + ".boundaries[" + std::to_string (i++) + "]");
        std::vector<BoundaryRef> cycle;
        size_t j = 0;
        for (const auto& piece : boundary.Items ()) {
            Reader ref (piece, boundary.path + "[" + std::to_string (j++) + "]");
            ref.Fields ({ "source", "segment", "reversed" });
            cycle.push_back ({ ref.Field ("source").Identifier (), ref.Field ("segment").Identifier (),
                               ref.Field ("reversed").Flag () });
        }
        if (cycle.empty ())
            Fail (boundary.path, "empty boundary support");
        descriptor.boundaries.push_back (std::move (cycle));
    }
    return descriptor;
}

namespace {
bool Utf8 (const std::string& text)
{
    for (size_t i = 0; i < text.size ();) {
        const unsigned char c = text[i++];
        if (c < 0x80)
            continue;
        unsigned int code = 0, count = 0, minimum = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            count = 1;
            code = c & 31;
            minimum = 0x80;
        }
        else if (c >= 0xE0 && c <= 0xEF) {
            count = 2;
            code = c & 15;
            minimum = 0x800;
        }
        else if (c >= 0xF0 && c <= 0xF4) {
            count = 3;
            code = c & 7;
            minimum = 0x10000;
        }
        else
            return false;
        if (count > text.size () - i)
            return false;
        for (unsigned int j = 0; j < count; ++j) {
            const unsigned char next = text[i++];
            if ((next & 0xC0) != 0x80)
                return false;
            code = (code << 6) | (next & 63);
        }
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            return false;
    }
    return true;
}
bool Hex4 (const std::string& text, size_t at, unsigned int& code)
{
    if (at + 4 > text.size ())
        return false;
    code = 0;
    for (size_t i = at; i < at + 4; ++i) {
        const char c = text[i];
        code <<= 4;
        if (c >= '0' && c <= '9')
            code |= c - '0';
        else if (c >= 'A' && c <= 'F')
            code |= c - 'A' + 10;
        else if (c >= 'a' && c <= 'f')
            code |= c - 'a' + 10;
        else
            return false;
    }
    return true;
}
} // namespace

bool StrictJson (const std::string& text, std::string& normalized, std::string& error)
{
    if (text.size () > 8 * 1024 * 1024 || !Utf8 (text)) {
        error = "document: 8 MiB limit exceeded or malformed UTF-8";
        return false;
    }
    normalized.clear ();
    bool inString = false;
    size_t at = 0;
    const auto fail = [&] (const char* message) {
        error = "JSON byte " + std::to_string (at) + ": " + message;
        return false;
    };
    while (at < text.size ()) {
        const char c = text[at];
        if (inString) {
            if (static_cast<unsigned char> (c) < 0x20)
                return fail ("unescaped string control");
            if (c == '"') {
                inString = false;
                normalized += c;
                ++at;
                continue;
            }
            if (c != '\\') {
                normalized += c;
                ++at;
                continue;
            }
            if (at + 1 == text.size ())
                return fail ("truncated escape");
            const char next = text[at + 1];
            if (next != 'u') {
                normalized += c;
                normalized += next;
                at += 2;
                continue;
            }
            unsigned int code = 0;
            if (!Hex4 (text, at + 2, code))
                return fail ("invalid Unicode escape");
            if (code >= 0xDC00 && code <= 0xDFFF)
                return fail ("unpaired low surrogate");
            if (code >= 0xD800 && code <= 0xDBFF) {
                unsigned int low = 0;
                if (at + 12 > text.size () || text[at + 6] != '\\' || text[at + 7] != 'u' ||
                    !Hex4 (text, at + 8, low) || low < 0xDC00 || low > 0xDFFF)
                    return fail ("unpaired high surrogate");
                code = 0x10000 + ((code - 0xD800) << 10) + low - 0xDC00;
                normalized += char (0xF0 | (code >> 18));
                normalized += char (0x80 | ((code >> 12) & 63));
                normalized += char (0x80 | ((code >> 6) & 63));
                normalized += char (0x80 | (code & 63));
                at += 12;
            }
            else {
                normalized.append (text, at, 6);
                at += 6;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
            normalized += c;
            ++at;
            continue;
        }
        if (c == '+' || c == '.' || (static_cast<unsigned char> (c) < 0x20 && c != '\t' && c != '\r' && c != '\n'))
            return fail ("invalid JSON token");
        if (c == '-' || (c >= '0' && c <= '9')) {
            const size_t start = at;
            if (text[at] == '-')
                ++at;
            if (at == text.size () || text[at] < '0' || text[at] > '9')
                return fail ("missing integer digits");
            if (text[at] == '0')
                ++at;
            else
                while (at < text.size () && text[at] >= '0' && text[at] <= '9')
                    ++at;
            if (at < text.size () && text[at] >= '0' && text[at] <= '9')
                return fail ("leading zero in JSON number");
            if (at < text.size () && text[at] == '.') {
                ++at;
                const size_t digits = at;
                while (at < text.size () && text[at] >= '0' && text[at] <= '9')
                    ++at;
                if (at == digits)
                    return fail ("missing fractional digits");
            }
            if (at < text.size () && (text[at] == 'e' || text[at] == 'E')) {
                ++at;
                if (at < text.size () && (text[at] == '+' || text[at] == '-'))
                    ++at;
                const size_t digits = at;
                while (at < text.size () && text[at] >= '0' && text[at] <= '9')
                    ++at;
                if (at == digits)
                    return fail ("missing exponent digits");
            }
            double number = 0;
            const auto parsed = std::from_chars (text.data () + start, text.data () + at, number);
            if (parsed.ec != std::errc {} || !std::isfinite (number))
                return fail ("number outside finite double range");
            normalized.append (text, start, at - start);
            continue;
        }
        normalized += c;
        ++at;
    }
    return true;
}
} // namespace tapioca::geometry::landscape_json
