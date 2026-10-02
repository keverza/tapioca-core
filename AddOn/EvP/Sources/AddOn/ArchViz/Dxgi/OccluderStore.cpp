// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.
// See OccluderStore.hpp.

#include "ArchViz/Dxgi/OccluderStore.hpp"

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace occluderstore {

void Store::Clear ()
{
    elements_.clear ();
    current_ = nullptr;
    vertices_ = 0;
    opaque_ = 0;
}

void Store::Forget (Element& element)
{
    vertices_ -= element.positions.size () / 3;
    opaque_ -= element.opaque.size ();
    element.positions.clear ();
    element.opaque.clear ();
    element.transparent.clear ();
}

bool Store::Begin (const std::string& guid, const float* xyz, uint32_t vertexCount, size_t maxVertices)
{
    current_ = nullptr;
    auto found = elements_.find (guid);
    if (found != elements_.end ())
        Forget (found->second);
    if (xyz == nullptr || vertexCount == 0 || vertices_ + vertexCount > maxVertices) {
        if (found != elements_.end ())
            elements_.erase (found); // its old geometry is not what the model holds now
        return false;
    }
    Element& element = found != elements_.end () ? found->second : elements_[guid];
    element.positions.assign (xyz, xyz + size_t (vertexCount) * 3);
    vertices_ += vertexCount;
    current_ = &element;
    return true;
}

bool Store::AddOpaque (const uint32_t* indices, uint32_t indexCount, size_t maxIndices)
{
    if (current_ == nullptr || indices == nullptr || indexCount == 0 || opaque_ + indexCount > maxIndices)
        return false;
    current_->opaque.insert (current_->opaque.end (), indices, indices + indexCount);
    opaque_ += indexCount;
    return true;
}

bool Store::AddTransparent (const uint32_t* indices, uint32_t indexCount)
{
    if (current_ == nullptr || indices == nullptr || indexCount == 0)
        return false;
    current_->transparent.insert (current_->transparent.end (), indices, indices + indexCount);
    return true;
}

bool Store::Remove (const std::string& guid)
{
    auto found = elements_.find (guid);
    if (found == elements_.end ())
        return false;
    if (current_ == &found->second)
        current_ = nullptr;
    Forget (found->second);
    elements_.erase (found);
    return true;
}

void Store::Assemble (std::vector<float>& positions, std::vector<uint32_t>& opaque,
                      std::vector<uint32_t>& transparent) const
{
    positions.clear ();
    opaque.clear ();
    transparent.clear ();
    positions.reserve (vertices_ * 3);
    opaque.reserve (opaque_);
    for (const auto& entry : elements_) {
        const Element& element = entry.second;
        const uint32_t base = uint32_t (positions.size () / 3);
        positions.insert (positions.end (), element.positions.begin (), element.positions.end ());
        for (uint32_t index : element.opaque)
            opaque.push_back (base + index);
        for (uint32_t index : element.transparent)
            transparent.push_back (base + index);
    }
}

} // namespace occluderstore
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
