#ifndef EVP_ARCHVIZ_ANALYSISLIVERERUN_HPP
#define EVP_ARCHVIZ_ANALYSISLIVERERUN_HPP

// ArchViz/AnalysisLiveRerun -- when an analysis that has been run once runs
// again by itself. After the first run the workflow follows its inputs: a
// model edit in Archicad (a new viewer capture) or a changed setting re-runs
// it, and the last result stays on screen until the new one replaces it.
//
// The inputs are one key: everything the result depends on, the capture's
// identity included. An empty key means the inputs cannot run yet (no current
// capture, a role left empty); the result on screen is kept and nothing runs.
//
// Pure: no ImGui, no worker. The HUD feeds it the key and the clock each frame
// and does what it answers.

#include <string>

namespace geomsrv::archviz::liverun {

struct Follow {
    bool armed = false;    // a run has been asked for; until then nothing follows
    std::string submitted; // the inputs of the last run submitted
    std::string pending;   // inputs that differ from it, waiting to settle
    double pendingSince = 0.0;
};

enum class Step {
    Idle,   // not armed, or the result on screen describes the inputs
    Wait,   // the inputs changed; they are settling or cannot run yet
    Cancel, // they settled while a run is going: stop it, submit next frame
    Submit, // they settled and nothing runs: submit them, then call Submitted
};

// Once a frame. `quietSeconds` is how long the inputs must stay the same before
// they run: a slider drag or a burst of edits then runs once, at its end,
// instead of cancelling a run every frame.
Step Decide (Follow& follow, const std::string& key, bool running, double now, double quietSeconds);

// The run for `key` was submitted (by Decide's Submit, or by a Run button,
// which also arms the follow).
void Submitted (Follow& follow, const std::string& key);

// Stop following: the result was cleared, so nothing should bring it back.
void Disarm (Follow& follow);

} // namespace geomsrv::archviz::liverun

#endif
