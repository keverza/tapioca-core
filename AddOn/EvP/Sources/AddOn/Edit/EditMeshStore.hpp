#ifndef TAPIOCA_EDIT_MESHSTORE_HPP
#define TAPIOCA_EDIT_MESHSTORE_HPP

// #139 M0: the derived editor/layer mesh store. Reuses MeshStore's immutable
// shared_ptr publication pattern, but as a SEPARATE store keyed by durable
// object id: published terrain, editor meshes, volumetric details and later
// LOD chains live here. geomsrv::MeshStore::current/shared stays the
// native-capture authority and is never touched by editor-generated geometry
// (PLAN-LandscapeEditor-112-139-140, "One derived scene-composition lane").

#include "Edit/EditPublishedMesh.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tapioca::geometry {

class EditorMeshStore {
  public:
    static EditorMeshStore& Get ()
    {
        static EditorMeshStore instance;
        return instance;
    }

    // Any thread: atomically replace (or insert) the published snapshot for
    // `mesh->object`. Readers hold their shared_ptr and never see partial
    // state; the previous snapshot stays alive until the last reader lets go.
    void Publish (std::shared_ptr<const PublishedMesh> mesh)
    {
        if (mesh == nullptr)
            return;
        std::lock_guard<std::mutex> lock (mtx);
        meshes[mesh->object] = std::move (mesh);
    }

    // Any thread: the current snapshot for `object`, or null if absent/stale.
    std::shared_ptr<const PublishedMesh> Get (const std::string& object) const
    {
        std::lock_guard<std::mutex> lock (mtx);
        auto it = meshes.find (object);
        return it == meshes.end () ? nullptr : it->second;
    }

    // Deterministic, sorted key listing for diagnostics and debug replay.
    std::vector<std::string> Objects () const
    {
        std::lock_guard<std::mutex> lock (mtx);
        std::vector<std::string> keys;
        keys.reserve (meshes.size ());
        for (const auto& [key, mesh] : meshes)
            keys.push_back (key);
        return keys;
    }

    void Remove (const std::string& object)
    {
        std::lock_guard<std::mutex> lock (mtx);
        meshes.erase (object);
    }

    void Release ()
    {
        std::lock_guard<std::mutex> lock (mtx);
        meshes.clear ();
    }

    size_t Bytes () const
    {
        std::lock_guard<std::mutex> lock (mtx);
        size_t total = 0;
        for (const auto& [key, mesh] : meshes)
            total += sizeof (std::pair<const std::string, std::shared_ptr<const PublishedMesh>>) +
                     key.capacity () + (mesh ? mesh->Bytes () : 0);
        return total;
    }

  private:
    EditorMeshStore () = default;

    mutable std::mutex mtx;
    std::map<std::string, std::shared_ptr<const PublishedMesh>> meshes;
};

} // namespace tapioca::geometry

#endif
