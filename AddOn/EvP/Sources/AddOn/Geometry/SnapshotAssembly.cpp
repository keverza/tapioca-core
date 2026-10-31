#include "Geometry/SnapshotAssembly.hpp"

namespace geomsrv {

namespace {

// Every entry `base` holds is in `next` with the same value; `next` may hold more.
//
// ⚠️ APPEND-ONLY, NOT EQUAL. A kept element's triangles index the pool it was
// captured against, so an index that changed its meaning must force a full walk
// -- but one that is merely NEW cannot be referenced by any kept element. Equality
// sent a single element's new surface (69 -> 70 surfaces) down a 48 s full
// re-extraction of an 806k-triangle model (2026-10-31 17:02, #97).
template <typename Value> bool Extends (const std::map<int32_t, Value>& base, const std::map<int32_t, Value>& next)
{
    for (const auto& [index, value] : base) {
        const auto it = next.find (index);
        if (it == next.end () || !(it->second == value))
            return false;
    }
    return true;
}

} // namespace

bool SnapshotAssembly::CanUpdate (const Snapshot* base, uint64_t stamp, const std::map<int32_t, double>& transparency,
                                  const std::map<int32_t, CapturedMaterialIdentity>& materialIdentity,
                                  const std::map<int32_t, CapturedSurface>& surfaceAttributes)
{
    return base != nullptr && base->completeModel && base->scope == "all" && base->captureStamp != 0 &&
           base->captureStamp + 1 == stamp && Extends (base->materialTransparency, transparency) &&
           Extends (base->materialIdentity, materialIdentity) && Extends (base->surfaceAttributes, surfaceAttributes);
}

SnapshotAssembly::SnapshotAssembly (uint64_t id, uint64_t stamp, std::map<int32_t, double> transparency,
                                    std::shared_ptr<const Snapshot> base, std::set<std::string> changed,
                                    std::map<int32_t, CapturedMaterialIdentity> materialIdentity,
                                    std::map<int32_t, CapturedSurface> surfaceAttributes)
    : result_ (std::make_shared<Snapshot> ()), base_ (std::move (base)), changed_ (std::move (changed))
{
    result_->id = id;
    result_->captureStamp = stamp;
    result_->materialTransparency = std::move (transparency);
    result_->materialIdentity = std::move (materialIdentity);
    result_->surfaceAttributes = std::move (surfaceAttributes);
    if (base_ != nullptr)
        valid_ = !changed_.empty () && CanUpdate (base_.get (), stamp, result_->materialTransparency,
                                                  result_->materialIdentity, result_->surfaceAttributes);
}

void SnapshotAssembly::Add (Mesh mesh)
{
    if (result_ == nullptr || mesh.guid.empty () || !seen_.insert (mesh.guid).second ||
        (base_ != nullptr && changed_.count (mesh.guid) == 0)) {
        valid_ = false;
        return;
    }
    result_->meshes.push_back (std::move (mesh));
}

std::shared_ptr<const Snapshot> SnapshotAssembly::Finish (bool completed)
{
    if (!completed || !valid_ || result_ == nullptr)
        return nullptr;
    if (base_ != nullptr) {
        std::set<std::string> before;
        for (const auto& mesh : base_->meshes) {
            if (mesh.guid.empty () || !before.insert (mesh.guid).second)
                return nullptr;
            if (changed_.count (mesh.guid) == 0)
                result_->meshes.push_back (mesh); // copy only off the host thread
        }
    }
    result_->completeModel = true;
    return std::move (result_);
}

} // namespace geomsrv
