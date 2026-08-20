#ifndef GEOMETRYSERVER_METADATASTORE_HPP
#define GEOMETRYSERVER_METADATASTORE_HPP

#include "ElementMeta.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

// Immutable snapshot of element metadata. Built on the main thread (Send All),
// read by HTTP workers — same publish pattern as MeshStore.
namespace geomsrv {

struct MetaSet {
    std::vector<ElementMeta>                elems;
    std::vector<Story>                      stories;  // sorted by index (bottom -> top)
    std::unordered_map<std::string, size_t> byGuid;   // guid -> index into elems

    const ElementMeta* Find (const std::string& guid) const
    {
        auto it = byGuid.find (guid);
        return it == byGuid.end () ? nullptr : &elems[it->second];
    }
};

class MetadataStore {
public:
    static MetadataStore& Get ()
    {
        static MetadataStore instance;
        return instance;
    }

    void Publish (std::shared_ptr<const MetaSet> set)
    {
        std::lock_guard<std::mutex> lock (mtx);
        current = std::move (set);
    }

    std::shared_ptr<const MetaSet> Current () const
    {
        std::lock_guard<std::mutex> lock (mtx);
        return current;
    }

    void Release ()
    {
        std::lock_guard<std::mutex> lock (mtx);
        current.reset ();
    }

    size_t Bytes () const
    {
        std::lock_guard<std::mutex> lock (mtx);
        if (!current) return 0;
        size_t n = sizeof (MetaSet);
        for (const auto& e : current->elems) {
            n += sizeof (ElementMeta) + e.guid.capacity () + e.typeName.capacity ()
               + e.elemId.capacity () + e.layer.capacity () + e.story.capacity ();
            for (const auto& p : e.classifications) n += p.first.capacity () + p.second.capacity ();
            for (const auto& p : e.properties)      n += p.first.capacity () + p.second.capacity ();
        }
        return n;
    }

private:
    MetadataStore () = default;
    mutable std::mutex             mtx;
    std::shared_ptr<const MetaSet> current;
};

} // namespace geomsrv

#endif
