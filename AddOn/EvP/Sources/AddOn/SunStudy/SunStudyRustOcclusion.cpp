#include "SunStudy/SunStudyRust.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"

#include <algorithm>
#include <array>

namespace evp::sunstudy::rust {

using namespace detail;

namespace {

// The C++ side of a traversal call: the ITraversal and the caller's cancel
// check, reached through the vtable's context pointer for one call.
struct TraversalShim {
    const ITraversal* traversal = nullptr;
    const std::function<bool ()>* isCancelled = nullptr;
};

uint8_t ShimOccludeDirectional (void* context, const double* origins, size_t count, const double* direction,
                                double tmin, double tmax, uint8_t* out, size_t maxParallel)
{
    try {
        const auto* shim = static_cast<const TraversalShim*> (context);
        static const std::function<bool ()> kNever;
        const auto& cancel = shim->isCancelled != nullptr ? *shim->isCancelled : kNever;
        return shim->traversal->OccludeDirectionalCancellable (origins, count, direction, tmin, tmax, out, maxParallel,
                                                               cancel)
                   ? 1
                   : 0;
    }
    catch (...) {
        return 2; // never unwind into Rust
    }
}

uint64_t ShimSceneVersion (void* context)
{
    try {
        return static_cast<const TraversalShim*> (context)->traversal->SceneVersion ();
    }
    catch (...) {
        return 0;
    }
}

uint8_t ShimIsCancelled (void* context)
{
    try {
        const auto* shim = static_cast<const TraversalShim*> (context);
        return shim->isCancelled != nullptr && *shim->isCancelled && (*shim->isCancelled) () ? 1 : 0;
    }
    catch (...) {
        return 1;
    }
}

TapiocaTraversalVtable VtableOf (TraversalShim& shim)
{
    return { &shim, &ShimOccludeDirectional, &ShimSceneVersion, &ShimIsCancelled };
}

TapiocaOcclusionTrace TraceOf (const TapiocaTraversalVtable& vtable, const SampleSet& samples, double tmin, double tmax,
                               size_t maxParallel)
{
    TapiocaOcclusionTrace trace {};
    trace.traversal = &vtable;
    trace.positions = { samples.positions, samples.positions == nullptr ? 0 : samples.count * 3 };
    trace.normals = { samples.normals, samples.normals == nullptr ? 0 : samples.count * 3 };
    trace.sampleCount = samples.count;
    trace.tmin = tmin;
    trace.tmax = tmax;
    trace.maxParallel = maxParallel;
    return trace;
}

} // namespace

RustOcclusionAccumulator::RustOcclusionAccumulator (size_t sampleCount, size_t stepCount)
{
    Invoke (
        [&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_create (sampleCount, stepCount, &handle_, m, n); },
        error_);
}

RustOcclusionAccumulator::~RustOcclusionAccumulator ()
{
    std::string ignored;
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_destroy (handle_, m, n); }, ignored);
}

RustOcclusionAccumulator::RustOcclusionAccumulator (RustOcclusionAccumulator&& other) noexcept
    : handle_ (other.handle_), error_ (std::move (other.error_))
{
    other.handle_ = nullptr;
}

RustOcclusionAccumulator& RustOcclusionAccumulator::operator= (RustOcclusionAccumulator&& other) noexcept
{
    if (this != &other) {
        std::string ignored;
        Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_destroy (handle_, m, n); }, ignored);
        handle_ = other.handle_;
        error_ = std::move (other.error_);
        other.handle_ = nullptr;
    }
    return *this;
}

void RustOcclusionAccumulator::Reset ()
{
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_reset (handle_, m, n); }, error_);
}

bool RustOcclusionAccumulator::SeedReusable (const RustOcclusionAccumulator& source,
                                             const std::vector<size_t>& sourceSamples,
                                             const std::vector<uint64_t>& dirtySteps)
{
    const TapiocaOcclusionSeedRequest request { source.handle_, sourceSamples.data (), sourceSamples.size (),
                                                dirtySteps.data (), dirtySteps.size () };
    uint8_t accepted = 0;
    if (!Invoke (
            [&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_seed (handle_, &request, &accepted, m, n); },
            error_))
        return false;
    return accepted != 0;
}

bool RustOcclusionAccumulator::AccumulateStep (const ITraversal& traversal, const SampleSet& samples, size_t stepIndex,
                                               const double sunDirection[3], double tmin, double tmax,
                                               size_t maxParallel, const std::function<bool ()>& isCancelled)
{
    if (sunDirection == nullptr)
        return false;
    TraversalShim shim { &traversal, &isCancelled };
    const TapiocaTraversalVtable vtable = VtableOf (shim);
    TapiocaOcclusionStepRequest request {};
    request.trace = TraceOf (vtable, samples, tmin, tmax, maxParallel);
    request.step = stepIndex;
    std::copy (sunDirection, sunDirection + 3, request.sun);
    uint8_t outcome = 1;
    if (!Invoke (
            [&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_step (handle_, &request, &outcome, m, n); },
            error_))
        return false;
    return outcome == 0;
}

size_t RustOcclusionAccumulator::AccumulateRange (const ITraversal& traversal, const SampleSet& samples,
                                                  const SunSeries& series, size_t firstStep, size_t maxSteps,
                                                  double tmin, double tmax, size_t maxParallel,
                                                  const std::function<bool ()>& isCancelled)
{
    TraversalShim shim { &traversal, &isCancelled };
    const TapiocaTraversalVtable vtable = VtableOf (shim);
    std::vector<double> directions;
    directions.reserve (series.StepCount () * 3);
    for (const SunStep& step : series.Steps ())
        directions.insert (directions.end (), step.direction, step.direction + 3);
    TapiocaOcclusionRangeRequest request {};
    request.trace = TraceOf (vtable, samples, tmin, tmax, maxParallel);
    request.directions = { directions.data (), directions.size () };
    request.first = firstStep;
    request.maxSteps = maxSteps;
    size_t done = 0;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_range (handle_, &request, &done, m, n); },
                 error_))
        return 0;
    return done;
}

TapiocaOcclusionState RustOcclusionAccumulator::State () const
{
    TapiocaOcclusionState state {};
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_state (handle_, &state, m, n); }, error_);
    return state;
}

size_t RustOcclusionAccumulator::SampleCount () const
{
    return State ().sampleCount;
}
size_t RustOcclusionAccumulator::StepCount () const
{
    return State ().stepCount;
}
size_t RustOcclusionAccumulator::WordsPerSample () const
{
    return State ().wordsPerSample;
}
size_t RustOcclusionAccumulator::ResolvedStepCount () const
{
    return State ().resolvedSteps;
}
size_t RustOcclusionAccumulator::ActiveSampleCount () const
{
    return State ().activeSamples;
}
size_t RustOcclusionAccumulator::LastRayCount () const
{
    return State ().lastRayCount;
}
bool RustOcclusionAccumulator::Complete () const
{
    return State ().complete != 0;
}

std::vector<uint64_t> RustOcclusionAccumulator::Bits () const
{
    const TapiocaOcclusionState state = State ();
    std::vector<uint64_t> bits (state.sampleCount * state.wordsPerSample);
    TapiocaOcclusionReadOutput output {};
    output.bits = bits.data ();
    output.bitCapacity = bits.size ();
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_read (handle_, &output, m, n); }, error_);
    return bits;
}

std::vector<uint8_t> RustOcclusionAccumulator::StepsResolved () const
{
    std::vector<uint8_t> flags (State ().stepCount);
    TapiocaOcclusionReadOutput output {};
    output.stepsResolved = flags.data ();
    output.stepCapacity = flags.size ();
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_read (handle_, &output, m, n); }, error_);
    return flags;
}

std::vector<double> RustOcclusionAccumulator::SunHours (double hoursPerStep) const
{
    std::vector<double> hours (State ().sampleCount);
    TapiocaOcclusionReadOutput output {};
    output.sunHours = hours.data ();
    output.hoursCapacity = hours.size ();
    output.hoursPerStep = hoursPerStep;
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_read (handle_, &output, m, n); }, error_);
    return hours;
}

} // namespace evp::sunstudy::rust
