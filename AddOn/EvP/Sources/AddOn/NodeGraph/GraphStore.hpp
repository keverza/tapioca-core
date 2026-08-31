#ifndef EVP_NODEGRAPH_GRAPHSTORE_HPP
#define EVP_NODEGRAPH_GRAPHSTORE_HPP

// Where graphs live between sessions.
//
// The runtime API is storage-provider independent by design (architecture doc
// §34): the graph model must not care whether a workflow came from a project, a
// sidecar file or a user library. The user chose the TAPIOCA WORKFLOW LIBRARY as
// the first real backend on 2026-08-29 - graphs as reusable assets independent
// of any one project, which is the shape ADR-006's Player direction already
// implies.
//
// ⚠️ TWO BACKENDS, NOT FOUR. The reviewed plan (§2.3) refuses to name four
// implementations before one of them exists, because an interface shaped around
// three backends nobody has written is an interface shaped around the wrong one.
// So: `MemoryGraphStore`, which the offline suite and headless tests run
// against and which is not a placeholder, and `FileGraphStore`, the one real
// backend the user chose. A project-embedded store and a sidecar store are noted
// in the architecture document and deliberately unbuilt.
//
// ⚠️ RUN HISTORY IS NOT STORED HERE. Graph persistence and cache persistence are
// separate concerns (§40.4), and a store that saved results would make reloading
// a workflow reload somebody else's answers.

#include "NodeGraph/GraphSerializer.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace evp::nodegraph {

class NodeRegistry;

enum class StoreStatus {
    Ok,

    // No graph by that name. Ordinary, not exceptional: a client asking whether
    // a workflow exists gets this rather than an error string to parse.
    NotFound,

    // The stored text is not a graph this build can load. Carries the
    // serializer's own message, which names the node.
    Invalid,

    // The backing store refused - a permission, a full disk, a locked file.
    IoFailed,

    // The id itself is unusable: empty, or carrying separators that a file-backed
    // backend could read as a path. Rejected by every backend so a graph written
    // to memory today cannot fail to save to a file tomorrow.
    InvalidId,
};

const char* StoreStatusName (StoreStatus status);

struct StoreResult {
    StoreStatus status = StoreStatus::Ok;
    std::string error;

    bool Ok () const
    {
        return status == StoreStatus::Ok;
    }
};

// What a listing shows without loading every graph. Kept small so a library of
// hundreds costs one cheap call.
struct StoredGraphInfo {
    GraphId graphId;
    std::string label;
    std::string description;
    size_t nodeCount = 0;
};

// True when `graphId` is usable as a name in any backend. The rule is a
// file-backed backend's rule applied everywhere, so the memory store cannot
// accept a name the library store would have to reject.
bool IsValidGraphId (const GraphId& graphId);

class IGraphStore {
  public:
    virtual ~IGraphStore () = default;

    virtual StoreResult Save (const GraphId& graphId, const GraphDocument& document,
                              const GraphMetadata& metadata) = 0;

    // `registry` validates the load: a stored graph naming a node type this
    // build does not have is refused here rather than at the first evaluation.
    virtual StoreResult Load (const GraphId& graphId, const NodeRegistry& registry, SerializedGraph& out) const = 0;

    virtual StoreResult Delete (const GraphId& graphId) = 0;

    virtual bool Exists (const GraphId& graphId) const = 0;

    virtual std::vector<StoredGraphInfo> List () const = 0;
};

// The offline and headless backend: graphs held as serialized TEXT, not as live
// documents.
//
// ⚠️ TEXT, AND THAT IS THE POINT. Holding a GraphDocument would make the memory
// store the one backend that never exercises the format, so a serializer bug
// would surface only against a file. Storing text means every backend round
// trips through the same code the library store will.
class MemoryGraphStore final : public IGraphStore {
  public:
    StoreResult Save (const GraphId& graphId, const GraphDocument& document,
                      const GraphMetadata& metadata) override;
    StoreResult Load (const GraphId& graphId, const NodeRegistry& registry, SerializedGraph& out) const override;
    StoreResult Delete (const GraphId& graphId) override;
    bool Exists (const GraphId& graphId) const override;
    std::vector<StoredGraphInfo> List () const override;

  private:
    struct Entry {
        std::string text;
        StoredGraphInfo info;
    };

    mutable std::mutex mutex_;
    std::map<GraphId, Entry> graphs_;
};

// The Tapioca workflow library: graphs as files, in one user-owned directory.
//
// Chosen by the user on 2026-08-29 as the first real backend. A workflow is a
// REUSABLE ASSET rather than a property of one project - the same direction
// ADR-006 sets for the Player - so it does not live inside a .pln, and a graph
// authored against one project can be run against another. A project-embedded
// or sidecar store remains addable behind IGraphStore if some workflow turns out
// to be about one specific project.
//
// ⚠️ A SAVE IS WRITE-THEN-RENAME, never a write in place. A workflow is
// something a person spent an afternoon on; a crash or a full disk halfway
// through writing it must cost the SAVE, not the file that was already there.
// The rename is the atomic step, and it is the only one that touches the real
// name.
//
// ⚠️ LISTING DOES NOT LOAD. It parses each file's header and counts its node
// array; it does not rebuild documents through ApplyEdit, which is the expensive
// half. A library of hundreds stays a cheap call, and a single corrupt file
// shows up in the listing as itself rather than making the whole library
// unlistable.
class FileGraphStore final : public IGraphStore {
  public:
    // Defaults to the per-user workflow library. An explicit root is what the
    // offline suite uses, against a temporary directory.
    explicit FileGraphStore (std::string rootDirectory = DefaultWorkflowDirectory ());

    // %LOCALAPPDATA%\Tapioca\Workflows on Windows, matching where private
    // command source is already deployed. Empty when the environment does not
    // say - the store then refuses every call rather than inventing a location.
    static std::string DefaultWorkflowDirectory ();

    const std::string& RootDirectory () const
    {
        return root_;
    }

    StoreResult Save (const GraphId& graphId, const GraphDocument& document,
                      const GraphMetadata& metadata) override;
    StoreResult Load (const GraphId& graphId, const NodeRegistry& registry, SerializedGraph& out) const override;
    StoreResult Delete (const GraphId& graphId) override;
    bool Exists (const GraphId& graphId) const override;
    std::vector<StoredGraphInfo> List () const override;

  private:
    std::string root_;
};

} // namespace evp::nodegraph

#endif
