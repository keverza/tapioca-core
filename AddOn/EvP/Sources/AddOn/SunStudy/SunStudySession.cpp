#include "SunStudy/SunStudySession.hpp"

#include <algorithm>

namespace evp::sunstudy {

void SunStudySession::ResetAccumulator ()
{
    accumulator_ = OcclusionAccumulator (samples_.count, series_.StepCount ());
    nextStep_ = 0;
    ++generation_;
}

void SunStudySession::Sync (const StudyInputs& inputs, const SunSeries& series, const SampleSet& samples)
{
    const bool geometryChanged = inputs.geometryVersion != inputs_.geometryVersion;
    const bool sunChanged = inputs.sunVersion != inputs_.sunVersion;
    const bool gridChanged = inputs.gridVersion != inputs_.gridVersion;

    // ⚠️ THE SAMPLE COUNT IS CHECKED AS WELL AS THE GRID VERSION, and it is not
    // redundant. A caller that forgets to bump gridVersion but hands over a
    // differently sized sample set would otherwise index a stale accumulator --
    // out of bounds if it grew, silently mismatched if it shrank. The version is
    // the contract; this is the seatbelt.
    const bool countChanged = samples.count != samples_.count;

    if (!initialised_ || geometryChanged || sunChanged || gridChanged || countChanged) {
        inputs_ = inputs;
        series_ = series;
        samples_ = samples;
        initialised_ = true;
        ResetAccumulator ();
        return;
    }

    // Nothing that invalidates the result changed. Refresh the non-owning
    // pointers -- the caller may have moved its arrays without changing their
    // contents -- and keep every resolved step.
    samples_ = samples;
}

size_t SunStudySession::Advance (const ITraversal& traversal, size_t maxSteps, double tmin, double tmax,
                                 size_t maxParallel)
{
    if (!initialised_ || maxSteps == 0)
        return 0;
    if (samples_.count == 0 || series_.Empty ())
        return 0;

    // The dispatch guard. See the header: a caller that loses the race is
    // refused, not queued, and 0 already means "nothing to do".
    bool expected = false;
    if (!advancing_.compare_exchange_strong (expected, true, std::memory_order_acq_rel))
        return 0;

    const size_t resolved =
        accumulator_.AccumulateRange (traversal, samples_, series_, nextStep_, maxSteps, tmin, tmax, maxParallel);
    nextStep_ += resolved;

    advancing_.store (false, std::memory_order_release);
    return resolved;
}

StudyProgress SunStudySession::Progress () const
{
    StudyProgress progress;
    progress.generation = generation_;
    progress.resolvedSteps = accumulator_.ResolvedStepCount ();
    progress.totalSteps = series_.StepCount ();
    progress.sampleCount = samples_.count;
    progress.empty = (samples_.count == 0) || series_.Empty ();

    // ⚠️ AN EMPTY STUDY IS NOT A CONVERGED ONE. Both produce zeroes, and calling
    // the empty case converged would let a caller publish "0 hours everywhere"
    // for a model it never actually analysed.
    progress.converged = !progress.empty && accumulator_.Complete ();
    return progress;
}

std::vector<double> SunStudySession::SunHours () const
{
    return accumulator_.SunHours (series_.HoursPerStep ());
}

} // namespace evp::sunstudy
