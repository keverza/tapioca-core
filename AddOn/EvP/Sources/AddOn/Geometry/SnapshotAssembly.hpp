#ifndef GEOMSRV_SNAPSHOTASSEMBLY_HPP
#define GEOMSRV_SNAPSHOTASSEMBLY_HPP

#include "Geometry/Mesh.hpp"

#include <memory>
#include <set>

namespace geomsrv {

// Pure worker-side assembly. No partial walk ever becomes a whole-model capture.
// Incremental assembly is allowed only for the immediately preceding capture and
// a material/catalog identity that only GREW (every earlier entry unchanged,
// new ones allowed); otherwise walk the full model.
//
// `sameCapture` (#14): a pass that REFINES the base's own capture -- the texture coordinates read
// after a full pass (ArchViz/ExtractionDeferredUv.hpp) -- splices at the base's stamp, not + 1.
// The model has not changed; any edit since moves the stamp and refuses it.
class SnapshotAssembly {
  public:
    static bool CanUpdate (const Snapshot* base, uint64_t stamp, const std::map<int32_t, double>& transparency,
                           const std::map<int32_t, CapturedMaterialIdentity>& materialIdentity = {},
                           const std::map<int32_t, CapturedSurface>& surfaceAttributes = {}, bool sameCapture = false);
    SnapshotAssembly (uint64_t id, uint64_t stamp, std::map<int32_t, double> transparency,
                      std::shared_ptr<const Snapshot> base = nullptr, std::set<std::string> changed = {},
                      std::map<int32_t, CapturedMaterialIdentity> materialIdentity = {},
                      std::map<int32_t, CapturedSurface> surfaceAttributes = {}, bool sameCapture = false);
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
