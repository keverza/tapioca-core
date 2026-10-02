#ifndef EVP_ARCHVIZ_DXGI_OCCLUDERSTORE_HPP
#define EVP_ARCHVIZ_DXGI_OCCLUDERSTORE_HPP

// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and each cost at
// least one. This is the producer's side of the host occluder (HostOccluders.hpp): the render
// thread never sees it, only the flat snapshot assembled from it.
//
// ArchViz/Dxgi/OccluderStore -- the host model's triangles, kept PER ELEMENT, so a change to a few
// elements changes only theirs.
//
// ⚠️ THE OCCLUDER USED TO BE ONE FLAT BUFFER THAT A BATCH ONLY APPENDED TO. A full batch cleared it;
// a partial one added -- so an element re-extracted was drawn twice and one hidden or deleted could
// never leave, and every visibility change had to re-extract the whole model. On a 3889-element
// project that was 4.9 s to hide three elements (2026-10-02 11:45), once the change had been
// noticed. Kept by element GUID, an upsert replaces and a removal removes, and the snapshot the
// render thread uploads is assembled from the store at the end of each batch.
//
// Pure C++: no Direct3D, no ACAPI -- tests/cpp pins it.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace occluderstore {

struct Element {
    std::vector<float> positions;      // xyz, world metres
    std::vector<uint32_t> opaque;      // into this element's own positions
    std::vector<uint32_t> transparent; // likewise: glass and helpers -- edges, never occlusion
};

class Store {
  public:
    void Clear ();

    // Start `guid`'s geometry from these vertices, REPLACING whatever it held. False -- and the
    // element no longer in the store -- when it would take the store past `maxVertices`.
    bool Begin (const std::string& guid, const float* xyz, uint32_t vertexCount, size_t maxVertices);
    // Indices into the vertices `Begin` was given, for the element it started. `AddOpaque` is
    // false, and keeps nothing, past `maxIndices` opaque indices in the whole store.
    bool AddOpaque (const uint32_t* indices, uint32_t indexCount, size_t maxIndices);
    bool AddTransparent (const uint32_t* indices, uint32_t indexCount);

    // The element is no longer in the 3D model. False when the store did not hold it.
    bool Remove (const std::string& guid);

    size_t ElementCount () const
    {
        return elements_.size ();
    }
    size_t VertexCount () const
    {
        return vertices_;
    }
    size_t OpaqueIndexCount () const
    {
        return opaque_;
    }

    // Every element in GUID order, flattened: positions concatenated, each element's indices
    // offset by where its block landed.
    void Assemble (std::vector<float>& positions, std::vector<uint32_t>& opaque,
                   std::vector<uint32_t>& transparent) const;

  private:
    void Forget (Element& element);

    std::map<std::string, Element> elements_;
    Element* current_ = nullptr;
    size_t vertices_ = 0;
    size_t opaque_ = 0;
};

} // namespace occluderstore
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
