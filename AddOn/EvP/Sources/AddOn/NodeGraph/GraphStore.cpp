#include "NodeGraph/GraphStore.hpp"

#include "NodeGraph/Json.hpp"
#include "NodeGraph/NodeRegistry.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace evp::nodegraph {
namespace {

// Long enough for any name a person types, short enough that no backend has to
// worry about a path length limit.
constexpr size_t kMaxGraphIdLength = 128;

} // namespace

const char* StoreStatusName (StoreStatus status)
{
    switch (status) {
        case StoreStatus::Ok:
            return "ok";
        case StoreStatus::NotFound:
            return "notFound";
        case StoreStatus::Invalid:
            return "invalid";
        case StoreStatus::IoFailed:
            return "ioFailed";
        case StoreStatus::InvalidId:
            return "invalidId";
    }
    return "ok";
}

bool IsValidGraphId (const GraphId& graphId)
{
    if (graphId.empty () || graphId.size () > kMaxGraphIdLength)
        return false;
    // A conservative allowlist rather than a list of forbidden characters: the
    // ways a string can be read as a path are host-specific and open-ended, and
    // a backend must never be the place that discovers a new one.
    for (const unsigned char c : graphId) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                             c == '-' || c == '_' || c == '.';
        if (!allowed)
            return false;
    }
    // Leading dots, and any run of two, would let a name mean a parent
    // directory or a hidden file once a backend writes to disk.
    if (graphId.front () == '.')
        return false;
    return graphId.find ("..") == std::string::npos;
}

StoreResult MemoryGraphStore::Save (const GraphId& graphId, const GraphDocument& document,
                                    const GraphMetadata& metadata)
{
    if (!IsValidGraphId (graphId))
        return { StoreStatus::InvalidId, "'" + graphId + "' is not a usable graph name" };

    const SerializeResult serialized = SerializeGraph (document, metadata);
    if (!serialized.ok)
        return { StoreStatus::Invalid, serialized.error };

    Entry entry;
    entry.text = serialized.text;
    entry.info.graphId = graphId;
    entry.info.label = metadata.label;
    entry.info.description = metadata.description;
    entry.info.nodeCount = document.Nodes ().size ();

    const std::lock_guard<std::mutex> lock (mutex_);
    graphs_[graphId] = std::move (entry);
    return {};
}

StoreResult MemoryGraphStore::Load (const GraphId& graphId, const NodeRegistry& registry,
                                    SerializedGraph& out) const
{
    if (!IsValidGraphId (graphId))
        return { StoreStatus::InvalidId, "'" + graphId + "' is not a usable graph name" };

    std::string text;
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        const auto iterator = graphs_.find (graphId);
        if (iterator == graphs_.end ())
            return { StoreStatus::NotFound, "there is no graph called '" + graphId + "'" };
        text = iterator->second.text;
    }

    // Deserialised OUTSIDE the lock: a load validates every node and edge
    // through ApplyEdit, and holding the store's lock across that would make one
    // slow load block every other client of the library.
    const DeserializeResult loaded = DeserializeGraph (text, registry);
    if (!loaded.ok)
        return { StoreStatus::Invalid, loaded.error };

    out = loaded.graph;
    return {};
}

StoreResult MemoryGraphStore::Delete (const GraphId& graphId)
{
    if (!IsValidGraphId (graphId))
        return { StoreStatus::InvalidId, "'" + graphId + "' is not a usable graph name" };

    const std::lock_guard<std::mutex> lock (mutex_);
    if (graphs_.erase (graphId) == 0)
        return { StoreStatus::NotFound, "there is no graph called '" + graphId + "'" };
    return {};
}

bool MemoryGraphStore::Exists (const GraphId& graphId) const
{
    const std::lock_guard<std::mutex> lock (mutex_);
    return graphs_.contains (graphId);
}

std::vector<StoredGraphInfo> MemoryGraphStore::List () const
{
    const std::lock_guard<std::mutex> lock (mutex_);
    std::vector<StoredGraphInfo> listing;
    listing.reserve (graphs_.size ());
    for (const auto& [graphId, entry] : graphs_)
        listing.push_back (entry.info);
    return listing;
}

namespace {

// One file per graph. The double extension keeps the library greppable and
// makes a graph obviously JSON to anything that opens one.
const char* const kGraphFileSuffix = ".tapiocagraph.json";

std::string FileNameFor (const GraphId& graphId)
{
    return graphId + kGraphFileSuffix;
}

// The graph id a file name carries, or empty when the name is not one of ours.
GraphId GraphIdFromFileName (const std::string& fileName)
{
    const size_t suffix = std::char_traits<char>::length (kGraphFileSuffix);
    if (fileName.size () <= suffix)
        return {};
    if (fileName.compare (fileName.size () - suffix, suffix, kGraphFileSuffix) != 0)
        return {};
    const GraphId graphId = fileName.substr (0, fileName.size () - suffix);
    // A file whose name this build would refuse to WRITE is also one it will not
    // offer to load: the allowlist is the same rule in both directions.
    return IsValidGraphId (graphId) ? graphId : GraphId {};
}

bool ReadWholeFile (const std::filesystem::path& path, std::string& out, std::string& error)
{
    std::ifstream file (path, std::ios::binary);
    if (!file) {
        error = "the file could not be opened";
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf ();
    if (file.bad ()) {
        error = "the file could not be read";
        return false;
    }
    out = buffer.str ();
    return true;
}

} // namespace

FileGraphStore::FileGraphStore (std::string rootDirectory) : root_ (std::move (rootDirectory))
{
}

std::string FileGraphStore::DefaultWorkflowDirectory ()
{
#ifdef _WIN32
    // Read rather than assembled from a user name: LOCALAPPDATA is what the
    // shell actually redirects, and a roaming or relocated profile makes any
    // constructed path wrong.
    const char* local = std::getenv ("LOCALAPPDATA");
    if (local == nullptr || *local == '\0')
        return {};
    return (std::filesystem::path (local) / "Tapioca" / "Workflows").string ();
#else
    const char* home = std::getenv ("HOME");
    if (home == nullptr || *home == '\0')
        return {};
    return (std::filesystem::path (home) / ".tapioca" / "workflows").string ();
#endif
}

StoreResult FileGraphStore::Save (const GraphId& graphId, const GraphDocument& document,
                                  const GraphMetadata& metadata)
{
    if (!IsValidGraphId (graphId))
        return { StoreStatus::InvalidId, "'" + graphId + "' is not a usable graph name" };
    if (root_.empty ())
        return { StoreStatus::IoFailed, "there is no workflow library location on this machine" };

    const SerializeResult serialized = SerializeGraph (document, metadata);
    if (!serialized.ok)
        return { StoreStatus::Invalid, serialized.error };

    // Every filesystem call is checked rather than left to throw: a full disk is
    // an outcome this store reports, not an exception crossing the runtime.
    std::error_code code;
    const std::filesystem::path root (root_);
    std::filesystem::create_directories (root, code);
    if (code && !std::filesystem::is_directory (root))
        return { StoreStatus::IoFailed, "the workflow library could not be created: " + code.message () };

    const std::filesystem::path target = root / FileNameFor (graphId);
    const std::filesystem::path temporary = root / (FileNameFor (graphId) + ".partial");
    {
        std::ofstream file (temporary, std::ios::binary | std::ios::trunc);
        if (!file)
            return { StoreStatus::IoFailed, "the workflow could not be written" };
        file << serialized.text;
        file.flush ();
        if (!file) {
            file.close ();
            std::filesystem::remove (temporary, code);
            return { StoreStatus::IoFailed, "the workflow could not be written in full" };
        }
    }

    // The atomic step. rename over an existing file is replace-in-place on
    // Windows and POSIX alike, so a reader sees either the old graph or the new
    // one and never a half of either.
    std::filesystem::rename (temporary, target, code);
    if (code) {
        std::filesystem::remove (temporary, code);
        return { StoreStatus::IoFailed, "the workflow could not be saved: " + code.message () };
    }
    return {};
}

StoreResult FileGraphStore::Load (const GraphId& graphId, const NodeRegistry& registry, SerializedGraph& out) const
{
    if (!IsValidGraphId (graphId))
        return { StoreStatus::InvalidId, "'" + graphId + "' is not a usable graph name" };
    if (root_.empty ())
        return { StoreStatus::IoFailed, "there is no workflow library location on this machine" };

    const std::filesystem::path path = std::filesystem::path (root_) / FileNameFor (graphId);
    std::error_code code;
    if (!std::filesystem::exists (path, code))
        return { StoreStatus::NotFound, "there is no graph called '" + graphId + "'" };

    std::string text;
    std::string error;
    if (!ReadWholeFile (path, text, error))
        return { StoreStatus::IoFailed, "'" + graphId + "' could not be read: " + error };

    const DeserializeResult loaded = DeserializeGraph (text, registry);
    if (!loaded.ok)
        return { StoreStatus::Invalid, loaded.error };

    out = loaded.graph;
    return {};
}

StoreResult FileGraphStore::Delete (const GraphId& graphId)
{
    if (!IsValidGraphId (graphId))
        return { StoreStatus::InvalidId, "'" + graphId + "' is not a usable graph name" };
    if (root_.empty ())
        return { StoreStatus::IoFailed, "there is no workflow library location on this machine" };

    const std::filesystem::path path = std::filesystem::path (root_) / FileNameFor (graphId);
    std::error_code code;
    if (!std::filesystem::exists (path, code))
        return { StoreStatus::NotFound, "there is no graph called '" + graphId + "'" };
    if (!std::filesystem::remove (path, code))
        return { StoreStatus::IoFailed, "'" + graphId + "' could not be deleted: " + code.message () };
    return {};
}

bool FileGraphStore::Exists (const GraphId& graphId) const
{
    if (!IsValidGraphId (graphId) || root_.empty ())
        return false;
    std::error_code code;
    return std::filesystem::exists (std::filesystem::path (root_) / FileNameFor (graphId), code);
}

std::vector<StoredGraphInfo> FileGraphStore::List () const
{
    std::vector<StoredGraphInfo> listing;
    if (root_.empty ())
        return listing;

    std::error_code code;
    std::filesystem::directory_iterator entries (root_, code);
    // A library that does not exist yet is an EMPTY library, not an error: the
    // first Save creates it, and a user who has never saved one should not be
    // shown a failure.
    if (code)
        return listing;

    for (const std::filesystem::directory_entry& entry : entries) {
        if (!entry.is_regular_file (code))
            continue;
        const GraphId graphId = GraphIdFromFileName (entry.path ().filename ().string ());
        if (graphId.empty ())
            continue;

        StoredGraphInfo info;
        info.graphId = graphId;

        // Parsed, not loaded: the header and the node COUNT, without rebuilding
        // the document through ApplyEdit. A file this build cannot read is still
        // listed - by name, with nothing else filled in - because a library that
        // hides a graph is worse than one that shows a graph needing repair.
        std::string text;
        std::string error;
        if (ReadWholeFile (entry.path (), text, error)) {
            const json::ParseResult parsed = json::Parse (text);
            if (parsed.ok) {
                if (const json::JsonValue* metadata = parsed.value.Find ("metadata"); metadata != nullptr) {
                    std::string field;
                    if (const json::JsonValue* label = metadata->Find ("label");
                        label != nullptr && label->AsString (field))
                        info.label = field;
                    if (const json::JsonValue* description = metadata->Find ("description");
                        description != nullptr && description->AsString (field))
                        info.description = field;
                }
                if (const json::JsonValue* nodes = parsed.value.Find ("nodes"); nodes != nullptr) {
                    if (const json::JsonArray* array = nodes->AsArray (); array != nullptr)
                        info.nodeCount = array->size ();
                }
            }
        }
        listing.push_back (std::move (info));
    }

    // Directory order is filesystem order, which is not an order a user can
    // predict. Sorted by name so a listing reads the same twice.
    std::sort (listing.begin (), listing.end (),
               [] (const StoredGraphInfo& left, const StoredGraphInfo& right) {
                   return left.graphId < right.graphId;
               });
    return listing;
}

} // namespace evp::nodegraph
