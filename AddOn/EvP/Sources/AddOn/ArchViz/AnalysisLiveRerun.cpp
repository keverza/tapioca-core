#include "ArchViz/AnalysisLiveRerun.hpp"

namespace geomsrv::archviz::liverun {

Step Decide (Follow& follow, const std::string& key, bool running, double now, double quietSeconds)
{
    if (!follow.armed)
        return Step::Idle;
    if (key == follow.submitted || key.empty ()) {
        // A change undone, or one that cannot run, restarts the settle clock.
        follow.pending.clear ();
        return key.empty () ? Step::Wait : Step::Idle;
    }
    if (key != follow.pending) {
        follow.pending = key;
        follow.pendingSince = now;
    }
    if (now - follow.pendingSince < quietSeconds)
        return Step::Wait;
    // ⚠️ A RUN IN FLIGHT IS CANCELLED, NOT AWAITED. It describes inputs that are
    // already gone; waiting for it would delay the current ones by a whole run.
    return running ? Step::Cancel : Step::Submit;
}

void Submitted (Follow& follow, const std::string& key)
{
    follow.armed = true;
    follow.submitted = key;
    follow.pending.clear ();
}

void Disarm (Follow& follow)
{
    follow = {};
}

} // namespace geomsrv::archviz::liverun
