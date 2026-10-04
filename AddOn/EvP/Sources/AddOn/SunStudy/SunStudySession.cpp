#include "SunStudy/SunStudySession.hpp"

#include <algorithm>
#include <chrono>

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

bool SunStudySession::SeedReusable (const OcclusionAccumulator& source, const std::vector<size_t>& sourceSamples,
                                    const std::vector<uint64_t>& dirtySteps)
{
    if (!initialised_ || nextStep_ != 0 || !accumulator_.SeedReusable (source, sourceSamples, dirtySteps))
        return false;
    if (accumulator_.Complete ())
        nextStep_ = series_.StepCount ();
    return true;
}

size_t SunStudySession::Advance (const ITraversal& traversal, size_t maxSteps, double tmin, double tmax,
                                 size_t maxParallel, const std::function<bool ()>& isCancelled)
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

    struct DispatchRelease {
        std::atomic<bool>& flag;
        ~DispatchRelease ()
        {
            flag.store (false, std::memory_order_release);
        }
    } release { advancing_ };

    size_t resolved = 0;
    while (resolved < maxSteps && nextStep_ < series_.StepCount ()) {
        if (isCancelled && isCancelled ())
            break;
        const auto started = std::chrono::steady_clock::now ();
        if (accumulator_.AccumulateRange (traversal, samples_, series_, nextStep_, 1, tmin, tmax, maxParallel,
                                          isCancelled) == 0)
            break;
        ++nextStep_;
        ++resolved;
        if (stepObserver_) {
            const double elapsed =
                std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
            // Diagnostics must not turn a completed step into a failed slice.
            try {
                stepObserver_ (nextStep_ - 1, accumulator_, elapsed);
            }
            catch (...) {
            }
        }
    }

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
