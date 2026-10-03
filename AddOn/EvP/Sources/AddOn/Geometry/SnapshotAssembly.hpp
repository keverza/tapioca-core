#ifndef GEOMSRV_SNAPSHOTASSEMBLY_HPP
#define GEOMSRV_SNAPSHOTASSEMBLY_HPP

#include "Geometry/Mesh.hpp"

#include <memory>
#include <set>

namespace geomsrv {

// Pure worker-side assembly. No partial walk ever becomes a whole-model capture.
// Incremental assembly is allowed only for the immediately preceding capture and
// an identical transparency pool; otherwise the host must walk the full model.
class SnapshotAssembly {
  public:
    static bool CanUpdate (const Snapshot* base, uint64_t stamp, const std::map<int32_t, double>& transparency);
    SnapshotAssembly (uint64_t id, uint64_t stamp, std::map<int32_t, double> transparency,
                      std::shared_ptr<const Snapshot> base = nullptr, std::set<std::string> changed = {});
    void Add (Mesh mesh);
    std::shared_ptr<const Snapshot> Finish (bool completed);

  private:
    std::shared_ptr<Snapshot> result_;
    std::shared_ptr<const Snapshot> base_;
    std::set<std::string> changed_, seen_;
    bool valid_ = true;
};

} // namespace geomsrv
#endif
