#include "NativeCommands/SunStudyHudRunner.hpp"
#include "NativeCommands/SunStudyFollowerDriver.hpp"

namespace geomsrv::sunstudyhud {

Status JobStatusOf (Status status, const sunfollow::FollowerStats& follow)
{
    status.paused = follow.paused;
    status.stale = follow.dirty || follow.paused || (status.running && !follow.studyId.empty ());
    if (status.running)
        return status;
    const bool active = follow.cancelling || follow.workerBusy ||
                        (follow.autoFollow && follow.state != evp::sunstudy::SunStudyFollowState::Failed &&
                         (follow.dirty || follow.millisecondsUntilStart >= 0));
    if (active || follow.stage == "cancelled" || follow.stage == "failed") {
        status.following = true;
        status.running = active;
        status.cancelling = follow.cancelling || (!follow.autoFollow && follow.workerBusy);
        status.stage = follow.stage;
        status.error = follow.lastError;
        status.studyId = follow.studyId;
        status.resolvedSteps = active ? follow.resolvedSteps : 0;
        status.totalSteps = active ? follow.totalSteps : 0;
    }
    return status;
}

} // namespace geomsrv::sunstudyhud
