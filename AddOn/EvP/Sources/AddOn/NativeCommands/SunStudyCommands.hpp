#ifndef EVP_NATIVECOMMANDS_SUNSTUDYCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"
#include "Geometry/MeshStore.hpp"
#include "SunStudy/SunSeries.hpp"

#include <atomic>

namespace evp::sunstudy {
struct StudyRecord;
}

namespace geomsrv {

struct CapturedSunStudyInputs {
    GS::ObjectState params;
    std::shared_ptr<const Snapshot> snapshot;
    API_PlaceInfo place = {};
    evp::sunstudy::SunSeries series;
};

// Main-thread capture only; no BVH, winding proof, sampling or atlas assembly.
NativeCommandResult CaptureSunStudyInputs (const GS::ObjectState& params,
                                           std::shared_ptr<const CapturedSunStudyInputs>& captured);
// Owned immutable capture; no ACAPI or gate. Same preparation as StartSunStudy.
NativeCommandResult PrepareCapturedSunStudy (const std::shared_ptr<const CapturedSunStudyInputs>& captured,
                                             const std::atomic<bool>& cancelled,
                                             std::shared_ptr<const evp::sunstudy::StudyRecord> reuseSource = nullptr);

// The sun study's bus surface: StartSunStudy, AdvanceSunStudy, SunStudyState,
// GetSunStudyResults, CancelSunStudy.
//
// The synchronous public StartSunStudy command captures the host's sun and then
// prepares the record on its caller. The automatic follower uses the SAME capture
// and preparation, dispatching the latter to a worker. AdvanceSunStudy remains
// synchronous and SDK-free; a caller needing host interactivity must schedule it.
//
// A caller therefore starts once, advances in as many bounded slices as it
// likes, and reads progress between them. That is what makes a long study
// interruptible at timestep boundaries and a partial result available. No command
// here ever runs a whole study.
NativeCommandRegistrations GetSunStudyCommandRegistrations ();

} // namespace geomsrv

#endif
