#include "SunStudy/SunStudyRust.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"

namespace evp::sunstudy::rust {

using namespace detail;

namespace {

// Forwards the Rust observer callback to the wrapper's std::function.
void ForwardObservation (void* context, size_t step, double wallMs, const TapiocaOcclusionState* state)
{
    try {
        const auto* observer = static_cast<const RustSunStudySession::StepObserver*> (context);
        if (observer != nullptr && *observer && state != nullptr)
            (*observer) (step, *state, wallMs);
    }
    catch (...) {
        // Diagnostics must not turn a completed step into a failed slice, and
        // nothing may unwind into Rust.
    }
}

} // namespace

RustSunStudySession::RustSunStudySession ()
{
    std::string error;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_session_create (&handle_, m, n); }, error))
        SetError (error);
}

RustSunStudySession::~RustSunStudySession ()
{
    std::string ignored;
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_session_destroy (handle_, m, n); }, ignored);
}

std::string RustSunStudySession::LastError () const
{
    std::lock_guard<std::mutex> lock (errorMutex_);
    return error_;
}

void RustSunStudySession::SetError (const std::string& error) const
{
    std::lock_guard<std::mutex> lock (errorMutex_);
    error_ = error;
}

void RustSunStudySession::Sync (const StudyInputs& inputs, const SunSeries& series, const SampleSet& samples)
{
    std::vector<TapiocaSunStep> steps;
    steps.reserve (series.StepCount ());
    for (const SunStep& step : series.Steps ())
        steps.push_back (ToPod (step));
    TapiocaSessionSyncRequest request {};
    request.versions[0] = inputs.geometryVersion;
    request.versions[1] = inputs.sunVersion;
    request.versions[2] = inputs.gridVersion;
    request.steps = { steps.data (), steps.size () };
    request.timestepMinutes = series.TimestepMinutes ();
    request.sourceStepCount = series.SourceStepCount ();
    request.seriesVersion = series.Version ();
    request.sampleCount = samples.count;
    std::string error;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_session_sync (handle_, &request, m, n); }, error))
        SetError (error);
    samples_ = samples; // non-owning, as the C++ session keeps them
}

bool RustSunStudySession::SeedReusable (const RustSunStudySession& source, const std::vector<size_t>& sourceSamples,
                                        const std::vector<uint64_t>& dirtySteps)
{
    const TapiocaSessionSeedRequest request { source.handle_, sourceSamples.data (), sourceSamples.size (),
                                              dirtySteps.data (), dirtySteps.size () };
    uint8_t accepted = 0;
    std::string error;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_session_seed (handle_, &request, &accepted, m, n); },
                 error)) {
        SetError (error);
        return false;
    }
    return accepted != 0;
}

void RustSunStudySession::SetStepObserver (StepObserver observer)
{
    observer_ = std::move (observer);
}

size_t RustSunStudySession::Advance (const ITraversal& traversal, size_t maxSteps, double tmin, double tmax,
                                     size_t maxParallel, const std::function<bool ()>& isCancelled)
{
    TraversalShim shim { &traversal, &isCancelled };
    const TapiocaTraversalVtable vtable = VtableOf (shim);
    TapiocaSessionAdvanceRequest request {};
    request.trace = TraceOf (vtable, samples_, tmin, tmax, maxParallel);
    request.maxSteps = maxSteps;
    if (observer_)
        request.observer = { &observer_, &ForwardObservation };
    size_t resolved = 0;
    std::string error;
    if (!Invoke (
            [&] (char* m, size_t n) { return tapioca_sunstudy_session_advance (handle_, &request, &resolved, m, n); },
            error)) {
        SetError (error);
        return 0;
    }
    return resolved;
}

TapiocaSessionProgress RustSunStudySession::RawProgress () const
{
    TapiocaSessionProgress progress {};
    std::string error;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_session_progress (handle_, &progress, m, n); },
                 error))
        SetError (error);
    return progress;
}

bool RustSunStudySession::Advancing () const
{
    return RawProgress ().advancing != 0;
}

StudyProgress RustSunStudySession::Progress () const
{
    const TapiocaSessionProgress raw = RawProgress ();
    StudyProgress progress;
    progress.generation = raw.generation;
    progress.resolvedSteps = raw.resolvedSteps;
    progress.totalSteps = raw.totalSteps;
    progress.sampleCount = raw.sampleCount;
    progress.converged = raw.converged != 0;
    progress.empty = raw.empty != 0;
    return progress;
}

uint64_t RustSunStudySession::Generation () const
{
    return RawProgress ().generation;
}

std::vector<double> RustSunStudySession::SunHours () const
{
    std::vector<double> hours (RawProgress ().accumulator.sampleCount);
    TapiocaOcclusionReadOutput output {};
    output.sunHours = hours.data ();
    output.hoursCapacity = hours.size ();
    std::string error;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_session_read (handle_, &output, m, n); }, error))
        SetError (error);
    return hours;
}

std::vector<uint64_t> RustSunStudySession::Bits () const
{
    const TapiocaOcclusionState state = RawProgress ().accumulator;
    std::vector<uint64_t> bits (state.sampleCount * state.wordsPerSample);
    TapiocaOcclusionReadOutput output {};
    output.bits = bits.data ();
    output.bitCapacity = bits.size ();
    std::string error;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_session_read (handle_, &output, m, n); }, error))
        SetError (error);
    return bits;
}

std::vector<uint8_t> RustSunStudySession::StepsResolved () const
{
    std::vector<uint8_t> flags (RawProgress ().accumulator.stepCount);
    TapiocaOcclusionReadOutput output {};
    output.stepsResolved = flags.data ();
    output.stepCapacity = flags.size ();
    std::string error;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_session_read (handle_, &output, m, n); }, error))
        SetError (error);
    return flags;
}

} // namespace evp::sunstudy::rust
