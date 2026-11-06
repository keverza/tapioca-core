#include "Edit/EditDraft.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "NodeGraph/Json.hpp"
#include <cctype>
#include <charconv>

namespace tapioca::geometry {
namespace {
namespace json = evp::nodegraph::json;

std::string SessionText (const geomsrv::edit::Document& doc)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    if (doc.landscape)
        for (const uint8_t byte : doc.landscape->sessionUuid) {
            out.push_back (digits[byte >> 4]);
            out.push_back (digits[byte & 15]);
        }
    return out;
}

bool Text (const json::JsonValue& root, const char* key, std::string& out)
{
    const auto* value = root.Find (key);
    return value != nullptr && value->AsString (out);
}
} // namespace

bool WriteDraft (const geomsrv::edit::Document& doc, const std::string& savedUtc, std::string& text, std::string& error)
{
    std::string definition;
    if (!WriteLandscape (doc, LandscapeStorage::Virtual, definition, error))
        return false;
    json::JsonObject root;
    root["format"] = json::JsonValue::String (kDraftFormat);
    root["version"] = json::JsonValue::Integer (kDraftVersion);
    root["savedUtc"] = json::JsonValue::String (savedUtc);
    root["revision"] = json::JsonValue::String (std::to_string (doc.revision));
    root["session"] = json::JsonValue::String (SessionText (doc));
    root["definition"] = json::JsonValue::String (std::move (definition));
    text = json::Write (json::JsonValue::Object (std::move (root)));
    error.clear ();
    return true;
}

bool ReadDraft (const std::string& text, geomsrv::edit::Document& doc, DraftInfo& info, std::string& error)
{
    const auto parsed = json::Parse (text);
    if (!parsed.ok) {
        error = "The draft file is not valid JSON: " + parsed.error;
        return false;
    }
    const auto& root = parsed.value;
    std::string format, revision, definition;
    int64_t version = 0;
    const auto* versionValue = root.Find ("version");
    if (!Text (root, "format", format) || format != kDraftFormat) {
        error = "That file is not a Tapioca virtual-editor draft.";
        return false;
    }
    if (versionValue == nullptr || !versionValue->AsInteger (version) || version < 1) {
        error = "The draft has no valid version.";
        return false;
    }
    if (version > kDraftVersion) {
        error = "The draft was saved by a newer Tapioca (version " + std::to_string (version) + ").";
        return false;
    }
    DraftInfo read;
    read.version = int (version);
    if (!Text (root, "definition", definition) || !Text (root, "savedUtc", read.savedUtc) ||
        !Text (root, "session", read.session) || !Text (root, "revision", revision) ||
        std::from_chars (revision.data (), revision.data () + revision.size (), read.revision).ec != std::errc {}) {
        error = "The draft envelope is incomplete.";
        return false;
    }
    geomsrv::edit::Document loaded;
    std::string reason;
    if (!ReadLandscape (definition, loaded, reason)) {
        error = "The draft's definition cannot be read: " + reason;
        return false;
    }
    doc = std::move (loaded);
    info = std::move (read);
    error.clear ();
    return true;
}

std::string DraftFileName (const std::string& name)
{
    std::string out;
    for (const char c : name) {
        const auto u = static_cast<unsigned char> (c);
        out.push_back (std::isalnum (u) || c == ' ' || c == '-' || c == '_' || c == '.' ? c : '_');
    }
    const auto first = out.find_first_not_of (" ._");
    if (first == std::string::npos)
        return {};
    out = out.substr (first, 80);
    while (!out.empty () && (out.back () == ' ' || out.back () == '.'))
        out.pop_back ();
    if (out.empty ())
        return {};
    const std::string extension = kDraftExtension;
    if (out.size () < extension.size () || out.compare (out.size () - extension.size (), extension.size (), extension))
        out += extension;
    return out;
}

} // namespace tapioca::geometry
