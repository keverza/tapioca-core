#include "Geometry/SnapshotAssembly.hpp"

namespace geomsrv {

bool SnapshotAssembly::CanUpdate (const Snapshot* base, uint64_t stamp, const std::map<int32_t, double>& transparency)
{
    return base != nullptr && base->completeModel && base->scope == "all" && base->captureStamp != 0 &&
           base->captureStamp + 1 == stamp && base->materialTransparency == transparency;
}

SnapshotAssembly::SnapshotAssembly (uint64_t id, uint64_t stamp, std::map<int32_t, double> transparency,
                                    std::shared_ptr<const Snapshot> base, std::set<std::string> changed)
    : result_ (std::make_shared<Snapshot> ()), base_ (std::move (base)), changed_ (std::move (changed))
{
    result_->id = id;
    result_->captureStamp = stamp;
    result_->materialTransparency = std::move (transparency);
    if (base_ != nullptr)
        valid_ = !changed_.empty () && CanUpdate (base_.get (), stamp, result_->materialTransparency);
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
