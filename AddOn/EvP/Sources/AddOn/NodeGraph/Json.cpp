#include "NodeGraph/Json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace evp::nodegraph::json {
namespace {

void EscapeInto (const std::string& text, std::string& out)
{
    out += '"';
    for (const unsigned char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf (buffer, sizeof (buffer), "\\u%04x", c);
                    out += buffer;
                }
                else {
                    // UTF-8 bytes pass through: the document is UTF-8 in and
                    // UTF-8 out, and re-encoding it would be the one place a
                    // non-ASCII node label could be mangled.
                    out += static_cast<char> (c);
                }
        }
    }
    out += '"';
}

void NumberInto (double value, bool integral, int64_t integer, std::string& out)
{
    char buffer[40];
    if (integral) {
        std::snprintf (buffer, sizeof (buffer), "%lld", static_cast<long long> (integer));
    }
    else {
        // 17 significant digits round-trips every finite double exactly, which
        // is what a stored parameter needs: a graph that reloads to a slightly
        // different number computes a slightly different model.
        std::snprintf (buffer, sizeof (buffer), "%.17g", value);
    }
    out += buffer;
}

void WriteInto (const JsonValue& value, size_t indent, size_t depth, std::string& out);

void Newline (size_t indent, size_t depth, std::string& out)
{
    if (indent == 0)
        return;
    out += '\n';
    out.append (indent * depth, ' ');
}

void WriteInto (const JsonValue& value, size_t indent, size_t depth, std::string& out)
{
    switch (value.Kind ()) {
        case JsonKind::Null:
            out += "null";
            return;
        case JsonKind::Bool: {
            bool flag = false;
            value.AsBool (flag);
            out += flag ? "true" : "false";
            return;
        }
        case JsonKind::Number: {
            double number = 0.0;
            int64_t integer = 0;
            value.AsDouble (number);
            value.AsInteger (integer);
            NumberInto (number, value.IsIntegral (), integer, out);
            return;
        }
        case JsonKind::String: {
            std::string text;
            value.AsString (text);
            EscapeInto (text, out);
            return;
        }
        case JsonKind::Array: {
            const JsonArray& array = *value.AsArray ();
            if (array.empty ()) {
                out += "[]";
                return;
            }
            out += '[';
            for (size_t i = 0; i < array.size (); ++i) {
                if (i > 0)
                    out += ',';
                Newline (indent, depth + 1, out);
                WriteInto (array[i], indent, depth + 1, out);
            }
            Newline (indent, depth, out);
            out += ']';
            return;
        }
        case JsonKind::Object: {
            const JsonObject& object = *value.AsObject ();
            if (object.empty ()) {
                out += "{}";
                return;
            }
            out += '{';
            bool first = true;
            for (const auto& [key, member] : object) {
                if (!first)
                    out += ',';
                first = false;
                Newline (indent, depth + 1, out);
                EscapeInto (key, out);
                out += ':';
                if (indent > 0)
                    out += ' ';
                WriteInto (member, indent, depth + 1, out);
            }
            Newline (indent, depth, out);
            out += '}';
            return;
        }
    }
}

class Parser {
  public:
    Parser (const std::string& text, size_t maxDepth) : text_ (text), maxDepth_ (maxDepth)
    {
    }

    ParseResult Run ()
    {
        ParseResult result;
        JsonValue value;
        if (!ParseValue (0, value)) {
            result.error = error_;
            result.offset = position_;
            return result;
        }
        SkipWhitespace ();
        if (position_ != text_.size ()) {
            result.error = "trailing content after the document";
            result.offset = position_;
            return result;
        }
        result.ok = true;
        result.value = std::move (value);
        return result;
    }

  private:
    bool Fail (const std::string& message)
    {
        if (error_.empty ())
            error_ = message;
        return false;
    }

    void SkipWhitespace ()
    {
        while (position_ < text_.size ()) {
            const char c = text_[position_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++position_;
            else
                break;
        }
    }

    bool Literal (const char* word)
    {
        const size_t length = std::char_traits<char>::length (word);
        if (text_.compare (position_, length, word) != 0)
            return Fail (std::string ("expected ") + word);
        position_ += length;
        return true;
    }

    bool ParseString (std::string& out)
    {
        if (position_ >= text_.size () || text_[position_] != '"')
            return Fail ("expected a string");
        ++position_;
        out.clear ();
        while (position_ < text_.size ()) {
            const char c = text_[position_++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (position_ >= text_.size ())
                return Fail ("the string ends inside an escape");
            const char escape = text_[position_++];
            switch (escape) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    if (position_ + 4 > text_.size ())
                        return Fail ("a \\u escape is truncated");
                    unsigned int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char digit = text_[position_++];
                        code <<= 4U;
                        if (digit >= '0' && digit <= '9')
                            code |= static_cast<unsigned int> (digit - '0');
                        else if (digit >= 'a' && digit <= 'f')
                            code |= static_cast<unsigned int> (digit - 'a' + 10);
                        else if (digit >= 'A' && digit <= 'F')
                            code |= static_cast<unsigned int> (digit - 'A' + 10);
                        else
                            return Fail ("a \\u escape has a non-hex digit");
                    }
                    // Encoded as UTF-8. Surrogate halves are written through as
                    // they arrive rather than paired: this writer never emits
                    // \u for anything above 0x1F, so a surrogate pair here came
                    // from another tool and re-encoding it wrongly would corrupt
                    // text we were asked only to carry.
                    if (code < 0x80) {
                        out += static_cast<char> (code);
                    }
                    else if (code < 0x800) {
                        out += static_cast<char> (0xC0U | (code >> 6U));
                        out += static_cast<char> (0x80U | (code & 0x3FU));
                    }
                    else {
                        out += static_cast<char> (0xE0U | (code >> 12U));
                        out += static_cast<char> (0x80U | ((code >> 6U) & 0x3FU));
                        out += static_cast<char> (0x80U | (code & 0x3FU));
                    }
                    break;
                }
                default:
                    return Fail ("unknown escape in a string");
            }
        }
        return Fail ("the string is never closed");
    }

    bool ParseNumber (JsonValue& out)
    {
        const size_t start = position_;
        if (position_ < text_.size () && (text_[position_] == '-' || text_[position_] == '+'))
            ++position_;
        bool integral = true;
        bool anyDigit = false;
        while (position_ < text_.size ()) {
            const char c = text_[position_];
            if (c >= '0' && c <= '9') {
                anyDigit = true;
                ++position_;
            }
            else if (c == '.' || c == 'e' || c == 'E') {
                integral = false;
                ++position_;
            }
            else if ((c == '-' || c == '+') && position_ > start &&
                     (text_[position_ - 1] == 'e' || text_[position_ - 1] == 'E')) {
                ++position_;
            }
            else {
                break;
            }
        }
        if (!anyDigit)
            return Fail ("expected a number");

        const std::string token = text_.substr (start, position_ - start);
        if (integral) {
            errno = 0;
            char* end = nullptr;
            const long long parsed = std::strtoll (token.c_str (), &end, 10);
            // An integer too large for int64 is kept as a double rather than
            // silently wrapping, which is the one outcome that would change a
            // value without saying so.
            if (errno == 0 && end != nullptr && *end == '\0') {
                out = JsonValue::Integer (static_cast<int64_t> (parsed));
                return true;
            }
            integral = false;
        }
        char* end = nullptr;
        const double parsed = std::strtod (token.c_str (), &end);
        if (end == nullptr || *end != '\0')
            return Fail ("malformed number");
        out = JsonValue::Double (parsed);
        return true;
    }

    bool ParseValue (size_t depth, JsonValue& out)
    {
        if (depth > maxDepth_)
            return Fail ("the document nests too deeply");
        SkipWhitespace ();
        if (position_ >= text_.size ())
            return Fail ("the document ends where a value was expected");

        const char c = text_[position_];
        if (c == '{')
            return ParseObject (depth, out);
        if (c == '[')
            return ParseArray (depth, out);
        if (c == '"') {
            std::string text;
            if (!ParseString (text))
                return false;
            out = JsonValue::String (std::move (text));
            return true;
        }
        if (c == 't') {
            if (!Literal ("true"))
                return false;
            out = JsonValue::Bool (true);
            return true;
        }
        if (c == 'f') {
            if (!Literal ("false"))
                return false;
            out = JsonValue::Bool (false);
            return true;
        }
        if (c == 'n') {
            if (!Literal ("null"))
                return false;
            out = JsonValue {};
            return true;
        }
        return ParseNumber (out);
    }

    bool ParseArray (size_t depth, JsonValue& out)
    {
        ++position_; // '['
        JsonArray array;
        SkipWhitespace ();
        if (position_ < text_.size () && text_[position_] == ']') {
            ++position_;
            out = JsonValue::Array (std::move (array));
            return true;
        }
        for (;;) {
            JsonValue element;
            if (!ParseValue (depth + 1, element))
                return false;
            array.push_back (std::move (element));
            SkipWhitespace ();
            if (position_ >= text_.size ())
                return Fail ("the array is never closed");
            const char c = text_[position_++];
            if (c == ']')
                break;
            if (c != ',')
                return Fail ("expected , or ] in an array");
        }
        out = JsonValue::Array (std::move (array));
        return true;
    }

    bool ParseObject (size_t depth, JsonValue& out)
    {
        ++position_; // '{'
        JsonObject object;
        SkipWhitespace ();
        if (position_ < text_.size () && text_[position_] == '}') {
            ++position_;
            out = JsonValue::Object (std::move (object));
            return true;
        }
        for (;;) {
            SkipWhitespace ();
            std::string key;
            if (!ParseString (key))
                return false;
            SkipWhitespace ();
            if (position_ >= text_.size () || text_[position_] != ':')
                return Fail ("expected : after an object key");
            ++position_;
            JsonValue member;
            if (!ParseValue (depth + 1, member))
                return false;
            // A repeated key is a rejection rather than a last-one-wins: two
            // spellings of one member mean the file disagrees with itself and
            // picking either silently is how a graph loads as something the
            // author did not write.
            if (!object.emplace (std::move (key), std::move (member)).second)
                return Fail ("the object has a repeated key");
            SkipWhitespace ();
            if (position_ >= text_.size ())
                return Fail ("the object is never closed");
            const char c = text_[position_++];
            if (c == '}')
                break;
            if (c != ',')
                return Fail ("expected , or } in an object");
        }
        out = JsonValue::Object (std::move (object));
        return true;
    }

    const std::string& text_;
    size_t maxDepth_;
    size_t position_ = 0;
    std::string error_;
};

} // namespace

JsonValue JsonValue::Bool (bool value)
{
    JsonValue result;
    result.kind_ = JsonKind::Bool;
    result.bool_ = value;
    return result;
}

JsonValue JsonValue::Integer (int64_t value)
{
    JsonValue result;
    result.kind_ = JsonKind::Number;
    result.integral_ = true;
    result.integer_ = value;
    result.number_ = static_cast<double> (value);
    return result;
}

JsonValue JsonValue::Double (double value)
{
    JsonValue result;
    result.kind_ = JsonKind::Number;
    result.integral_ = false;
    result.number_ = value;
    result.integer_ = static_cast<int64_t> (value);
    return result;
}

JsonValue JsonValue::String (std::string value)
{
    JsonValue result;
    result.kind_ = JsonKind::String;
    result.string_ = std::move (value);
    return result;
}

JsonValue JsonValue::Array (JsonArray value)
{
    JsonValue result;
    result.kind_ = JsonKind::Array;
    result.array_ = std::move (value);
    return result;
}

JsonValue JsonValue::Object (JsonObject value)
{
    JsonValue result;
    result.kind_ = JsonKind::Object;
    result.object_ = std::move (value);
    return result;
}

bool JsonValue::AsBool (bool& out) const
{
    if (kind_ != JsonKind::Bool)
        return false;
    out = bool_;
    return true;
}

bool JsonValue::AsInteger (int64_t& out) const
{
    if (kind_ != JsonKind::Number)
        return false;
    out = integral_ ? integer_ : static_cast<int64_t> (number_);
    return true;
}

bool JsonValue::AsDouble (double& out) const
{
    if (kind_ != JsonKind::Number)
        return false;
    out = integral_ ? static_cast<double> (integer_) : number_;
    return true;
}

bool JsonValue::AsString (std::string& out) const
{
    if (kind_ != JsonKind::String)
        return false;
    out = string_;
    return true;
}

const JsonArray* JsonValue::AsArray () const
{
    return kind_ == JsonKind::Array ? &array_ : nullptr;
}

const JsonObject* JsonValue::AsObject () const
{
    return kind_ == JsonKind::Object ? &object_ : nullptr;
}

const JsonValue* JsonValue::Find (const std::string& key) const
{
    if (kind_ != JsonKind::Object)
        return nullptr;
    const auto iterator = object_.find (key);
    return iterator == object_.end () ? nullptr : &iterator->second;
}

std::string Write (const JsonValue& value, size_t indent)
{
    std::string out;
    WriteInto (value, indent, 0, out);
    return out;
}

ParseResult Parse (const std::string& text, size_t maxDepth)
{
    Parser parser (text, maxDepth);
    return parser.Run ();
}

} // namespace evp::nodegraph::json
