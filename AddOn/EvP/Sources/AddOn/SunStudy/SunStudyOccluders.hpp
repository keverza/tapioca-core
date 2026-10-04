#ifndef EVP_SUNSTUDY_SUNSTUDYOCCLUDERS_HPP
#define EVP_SUNSTUDY_SUNSTUDYOCCLUDERS_HPP

#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudyRoles.hpp"

namespace evp::sunstudy {

// Context is reusable geometry, NOT permanently static geometry. Its canonical
// GUID membership and exact vertices/indices are checked on each captured model.
struct SunStudyOccluders {
    std::shared_ptr<const geomsrv::QueryEngine> analysis;
    std::shared_ptr<const geomsrv::QueryEngine> context;
    std::shared_ptr<const geomsrv::Snapshot> contextSnapshot;
    bool contextReused = false;
};

std::shared_ptr<const SunStudyOccluders> BuildSunStudyOccluders (const geomsrv::Snapshot& snapshot,
                                                                 const ElementRoles& roles,
                                                                 const SunStudyOccluders* previous = nullptr,
                                                                 const std::function<bool ()>& isCancelled = {});

// The CPU oracle/fallback for partitioned scenes. Occlusion is the OR of both
// indices, with one current-model version even when the context index is older.
class SunStudyPartitionTraversal final : public ITraversal {
  public:
    explicit SunStudyPartitionTraversal (std::shared_ptr<const geomsrv::QueryEngine> analysis,
                                         std::shared_ptr<const geomsrv::QueryEngine> context = nullptr);
    bool Occluded (const double origin[3], const double dir[3], double tmin, double tmax) const;
    void OccludeDirectional (const double* origins, size_t count, const double dir[3], double tmin, double tmax,
                             uint8_t* out, size_t maxParallel = 0) const override;
    bool OccludeDirectionalCancellable (const double* origins, size_t count, const double dir[3], double tmin,
                                        double tmax, uint8_t* out, size_t maxParallel,
                                        const std::function<bool ()>& isCancelled) const override;
    void OccludeRays (const OcclusionRay* rays, size_t count, uint8_t* out, size_t maxParallel = 0) const override;
    uint64_t SceneVersion () const override;

  private:
    std::shared_ptr<const geomsrv::QueryEngine> analysis_, context_;
};

} // namespace evp::sunstudy
#endif
