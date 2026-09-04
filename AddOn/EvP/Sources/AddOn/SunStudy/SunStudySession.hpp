#ifndef EVP_SUNSTUDY_SUNSTUDYSESSION_HPP
#define EVP_SUNSTUDY_SUNSTUDYSESSION_HPP

// SunStudy/SunStudySession — a study that is ADVANCED, not awaited.
//
// ⚠️ THIS IS THE WHOLE REASON THE FEATURE DOES NOT STUTTER THE HOST. A study is
// seconds of work; a caller that blocks for it freezes whatever thread asked.
// So the session owns the state and exposes ONE SMALL STEP: `Advance` resolves
// as many timesteps as the caller's budget allows and returns immediately. A
// frame loop calls it once a frame, a graph node calls it once an evaluation,
// and neither ever holds a thread for the whole run.
//
// ⚠️ WHICH ALSO MEANS PARTIAL RESULTS ARE THE NORMAL STATE, and the API is
// shaped so a caller cannot mistake one for a finished one. `Progress` reports
// `converged`; `SunHours` is available at every moment but is documented as
// too-low-until-converged; and an hours figure must be PUBLISHED only when
// converged is true. Too-low reads exactly like a shadier site.
//
// ---------------------------------------------------------------------------
// Selective invalidation
//
// Three things can change under a running study, and they cost different
// amounts to absorb:
//
//   GEOMETRY  the model was edited          -> everything is void
//   SUN       date, window, timestep, place -> the accumulator, not the samples
//   GRID      scope or sample spacing       -> the samples and the accumulator
//
// A threshold, a palette, a display mode or a filter range change NONE of them,
// which is the point: those are reads of a resident result and must never cost
// a recomputation. `Sync` compares the three versions it is handed against the
// three it holds and resets only what actually died.

#include "SunStudy/ITraversal.hpp"
#include "SunStudy/SunStudyOcclusion.hpp"
#include "SunStudy/SunSeries.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace evp::sunstudy {

// What a caller needs to draw a progress readout and decide whether to publish.
struct StudyProgress {
    uint64_t generation = 0;
    size_t resolvedSteps = 0;
    size_t totalSteps = 0;
    size_t sampleCount = 0;

    // ⚠️ THE ONLY HONEST GATE ON AN HOURS FIGURE.
    bool converged = false;

    // No samples, or no sun above the horizon. Distinguished from converged
    // because "the study finished" and "there was nothing to study" produce the
    // same empty numbers and mean very different things.
    bool empty = true;
};

// What changed since the last call. Zero means "no such input" rather than
// "unchanged" for every one of them, matching ITraversal::SceneVersion and
// SunSeries::Version.
struct StudyInputs {
    uint64_t geometryVersion = 0;
    uint64_t sunVersion = 0;
    uint64_t gridVersion = 0;
};

class SunStudySession final {
  public:
    SunStudySession () = default;

    // Point the session at a scene, a day and a sample set, resetting only what
    // the changed versions invalidate. Safe to call every frame with unchanged
    // inputs: that is the cheap path, and it must stay cheap.
    //
    // The session does NOT own `samples`; the caller keeps the arrays alive for
    // as long as it keeps advancing.
    void Sync (const StudyInputs& inputs, const SunSeries& series, const SampleSet& samples);

    // Resolve up to `maxSteps` more timesteps. Returns how many it resolved: 0
    // means converged, or nothing to do.
    //
    // ⚠️ `maxSteps` IS THE CALLER'S BUDGET AND THE SESSION NEVER SECOND-GUESSES
    // IT. A frame loop that can afford two steps passes two. A session that
    // decided for itself would be tuned against one caller's budget and wrong
    // for every other.
    size_t Advance (const ITraversal& traversal, size_t maxSteps, double tmin = 0.001, double tmax = 0.0,
                    size_t maxParallel = 0);

    StudyProgress Progress () const;

    // ⚠️ TOO LOW UNTIL Progress().converged. See the header note.
    std::vector<double> SunHours () const;

    // Per-sample, per-step lit bits: the resident result every display mode
    // reads. Empty before the first Sync.
    const OcclusionAccumulator& Accumulator () const
    {
        return accumulator_;
    }

    const SunSeries& Series () const
    {
        return series_;
    }

    // Bumped whenever a reset discards accumulated work.
    //
    // ⚠️ IT EXISTS TO RETIRE WORK IN FLIGHT. A slice computed against the old
    // scene can land after the model changed; a caller stamps its request with
    // the generation it saw and drops the answer if this has moved on. Without
    // it, a stale slice merges into a fresh accumulator and the study is quietly
    // a blend of two models.
    uint64_t Generation () const
    {
        return generation_;
    }

  private:
    void ResetAccumulator ();

    StudyInputs inputs_;
    SunSeries series_;
    SampleSet samples_;
    OcclusionAccumulator accumulator_ { 0, 0 };

    uint64_t generation_ = 0;
    size_t nextStep_ = 0;
    bool initialised_ = false;
};

} // namespace evp::sunstudy

#endif
