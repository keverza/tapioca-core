#ifndef EVP_NOTIFY_MODELDIFF_HPP
#define EVP_NOTIFY_MODELDIFF_HPP

// Notify/ModelDiff — "what changed since I last looked", asked of Archicad's own
// difference generator (ACAPI_DifferenceGenerator_*, AC26+).
//
// WHY THIS EXISTS RATHER THAN AN OBSERVER. Attaching a per-element observer is a
// DATABASE WRITE (PLAT-RE68): the project marks dirty and Archicad autosaves, so
// a viewer that merely WATCHES ends up editing. Attaching to a whole model does
// not scale either — measured at ~22 minutes of sliced main-thread work on a
// 12,238-element project. The difference generator answers the same question in
// ONE call, writes nothing, and needs nothing armed in advance.
//
// ⚠️ EACH CONSUMER OWNS ITS OWN BASELINE, and that is not a convenience — it is
// the same rule ChangeTracker's per-consumer cursors exist for. Polling ADOPTS
// the current state as the new baseline, so a shared one would mean whichever
// caller polled first consumed the change and hid it from the rest. The viewer's
// watch timer and a script calling `EvP.GetModelDiff` must each see every edit.
//
// ⚠️ MAIN THREAD ONLY. Both generator calls are ACAPI.
//
// ⚠️ COST IS NOT CONSTANT and is not known in advance. `Result::elapsedMs` is
// returned so a polling caller can set its own cadence from what it measured
// rather than from a number somebody guessed — see ArchViz/ModelWatch, which
// backs off until the diff is a small fraction of the interval.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace modeldiff {

// Which generator, and therefore what counts as a change.
enum class Scope {
    Model,   // APIDiff_3DModelBased — geometry as the modeler sees it. What a VIEWER wants.
    File,    // APIDiff_ModificationStampBased — every element edit, 3D consequence or not.
};

struct Result {
    bool ok = false;
    // True when this call only ESTABLISHED the baseline. The lists below are
    // then empty because there was nothing to compare against, which is not the
    // same fact as "nothing changed" and must not be collapsed into it.
    bool firstCall = false;
    std::vector<std::string> created;
    std::vector<std::string> modified;
    std::vector<std::string> deleted;
    bool environmentChanged = false;
    int64_t elapsedMs = 0;
    std::string error;   // filled only when ok is false

    bool AnythingChanged () const
    {
        return !created.empty () || !modified.empty () || !deleted.empty () ||
               environmentChanged;
    }
};

// One caller's private baseline. Construct it once and keep it; each Poll
// reports the difference since the previous Poll and re-baselines.
//
// The ACAPI state struct owns its GSHandle, deep-copies on assignment and frees
// in its destructor, so holding one by value needs no manual cleanup — the
// reason this class can be copy-free and still leak nothing.
class Baseline final {
public:
    explicit Baseline (Scope scope = Scope::Model);
    ~Baseline ();
    Baseline (const Baseline&) = delete;
    Baseline& operator= (const Baseline&) = delete;

    // `reset` throws the baseline away first, so the call reports `firstCall`
    // and establishes a fresh one.
    Result Poll (bool reset = false);

    // Change the generator. ⚠️ IT DROPS THE BASELINE, because the two generators
    // do not produce comparable states and diffing across them would be nonsense
    // presented as a result.
    void SetScope (Scope scope);
    Scope GetScope () const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}   // namespace modeldiff
}   // namespace geomsrv

#endif
