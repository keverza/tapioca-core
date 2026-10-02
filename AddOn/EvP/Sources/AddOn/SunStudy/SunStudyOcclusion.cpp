#include "SunStudy/SunStudyOcclusion.hpp"

#include <algorithm>
#include <chrono>

namespace evp::sunstudy {

namespace {

constexpr size_t kBitsPerWord = 64;

size_t WordsFor (size_t stepCount)
{
    return (stepCount + kBitsPerWord - 1) / kBitsPerWord;
}

} // namespace

OcclusionAccumulator::OcclusionAccumulator (size_t sampleCount, size_t stepCount)
    : sampleCount_ (sampleCount), stepCount_ (stepCount), wordsPerSample_ (WordsFor (stepCount))
{
    bits_.assign (sampleCount_ * wordsPerSample_, 0ull);
    stepResolved_.assign (stepCount_, 0u);
}

void OcclusionAccumulator::Reset ()
{
    std::fill (bits_.begin (), bits_.end (), 0ull);
    std::fill (stepResolved_.begin (), stepResolved_.end (), uint8_t { 0 });
    resolvedCount_ = 0;
    selective_ = false;
    activeSamples_.clear ();
}

bool OcclusionAccumulator::SeedReusable (const OcclusionAccumulator& source, const std::vector<size_t>& sourceSamples)
{
    if (&source == this || !source.Complete () || source.stepCount_ != stepCount_ ||
        sourceSamples.size () != sampleCount_ || resolvedCount_ != 0)
        return false;
    for (const size_t sample : sourceSamples) {
        if (sample != kNoReuse && sample >= source.sampleCount_)
            return false;
    }
    activeSamples_.clear ();
    std::fill (bits_.begin (), bits_.end (), 0ull);
    for (size_t sample = 0; sample < sampleCount_; ++sample) {
        const size_t from = sourceSamples[sample];
        if (from == kNoReuse)
            activeSamples_.push_back (static_cast<uint32_t> (sample));
        else
            std::copy_n (source.bits_.begin () + from * wordsPerSample_, wordsPerSample_,
                         bits_.begin () + sample * wordsPerSample_);
    }
    selective_ = true;
    if (activeSamples_.empty ()) {
        std::fill (stepResolved_.begin (), stepResolved_.end (), uint8_t { 1 });
        resolvedCount_ = stepCount_;
    }
    return true;
}

bool OcclusionAccumulator::AccumulateStep (const ITraversal& traversal, const SampleSet& samples, size_t stepIndex,
                                           const double sunDirection[3], double tmin, double tmax, size_t maxParallel,
                                           const std::function<bool ()>& isCancelled)
{
    if (stepIndex >= stepCount_ || sunDirection == nullptr)
        return false;
    if (samples.count != sampleCount_)
        return false;
    if (isCancelled && isCancelled ())
        return false;
    if (sampleCount_ == 0) {
        if (stepResolved_[stepIndex] == 0) {
            stepResolved_[stepIndex] = 1;
            ++resolvedCount_;
        }
        return true;
    }
    if (samples.positions == nullptr)
        return false;

    const size_t word = stepIndex / kBitsPerWord;
    const uint64_t bit = 1ull << (stepIndex % kBitsPerWord);
    const auto started = std::chrono::steady_clock::now ();
    traceMilliseconds_ = 0.0;

    // ⚠️ BACK-FACING SAMPLES ARE CULLED BEFORE THE RAY, NOT AFTER. A surface
    // turned away from the sun is self-shadowed whatever the geometry does, so
    // tracing it is pure waste — and around half the samples are back-facing at
    // any one moment, which makes this a factor-of-two on the whole study rather
    // than a micro-optimisation. Compacting into a dense array keeps the
    // traversal's work contiguous.
    frontFacingOrigins_.clear ();
    frontFacingIndex_.clear ();
    frontFacingOrigins_.reserve (ActiveSampleCount () * 3);
    frontFacingIndex_.reserve (ActiveSampleCount ());

    for (size_t i = 0; i < ActiveSampleCount (); ++i) {
        if (i % 4096 == 0 && isCancelled && isCancelled ())
            return false;
        const size_t s = selective_ ? activeSamples_[i] : i;
        if (samples.normals != nullptr) {
            const double* n = &samples.normals[s * 3];
            const double incidence = n[0] * sunDirection[0] + n[1] * sunDirection[1] + n[2] * sunDirection[2];
            if (incidence <= 0.0)
                continue; // facing away: self-shadowed, bit stays clear
        }
        const double* p = &samples.positions[s * 3];
        frontFacingOrigins_.push_back (p[0]);
        frontFacingOrigins_.push_back (p[1]);
        frontFacingOrigins_.push_back (p[2]);
        frontFacingIndex_.push_back (static_cast<uint32_t> (s));
    }

    const size_t traced = frontFacingIndex_.size ();
    const auto compacted = std::chrono::steady_clock::now ();
    compactMilliseconds_ = std::chrono::duration<double, std::milli> (compacted - started).count ();
    if (traced > 0) {
        occluded_.assign (traced, 0u);
        if (!traversal.OccludeDirectionalCancellable (frontFacingOrigins_.data (), traced, sunDirection, tmin, tmax,
                                                      occluded_.data (), maxParallel, isCancelled))
            return false;
        traceMilliseconds_ =
            std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - compacted).count ();
    }
    if (isCancelled && isCancelled ())
        return false;
    // Commit only after a complete packet set. Cancellation/re-resolution must
    // not clear an existing step or corrupt any reused complete-day samples.
    for (size_t i = 0; i < ActiveSampleCount (); ++i) {
        const size_t s = selective_ ? activeSamples_[i] : i;
        bits_[s * wordsPerSample_ + word] &= ~bit;
    }
    if (traced > 0) {
        for (size_t i = 0; i < traced; ++i) {
            if (occluded_[i] != 0)
                continue; // something in the way
            const size_t s = frontFacingIndex_[i];
            bits_[s * wordsPerSample_ + word] |= bit;
        }
    }

    if (stepResolved_[stepIndex] == 0) {
        stepResolved_[stepIndex] = 1;
        ++resolvedCount_;
    }
    return true;
}

size_t OcclusionAccumulator::AccumulateRange (const ITraversal& traversal, const SampleSet& samples,
                                              const SunSeries& series, size_t firstStep, size_t maxSteps, double tmin,
                                              double tmax, size_t maxParallel,
                                              const std::function<bool ()>& isCancelled)
{
    size_t done = 0;
    const size_t limit = std::min (series.StepCount (), stepCount_);
    for (size_t step = firstStep; step < limit && done < maxSteps; ++step) {
        if (!AccumulateStep (traversal, samples, step, series.Step (step).direction, tmin, tmax, maxParallel,
                             isCancelled))
            break;
        ++done;
    }
    return done;
}

bool OcclusionAccumulator::StepResolved (size_t stepIndex) const
{
    return stepIndex < stepCount_ && stepResolved_[stepIndex] != 0;
}

bool OcclusionAccumulator::Lit (size_t sampleIndex, size_t stepIndex) const
{
    if (sampleIndex >= sampleCount_ || stepIndex >= stepCount_)
        return false;
    const uint64_t word = bits_[sampleIndex * wordsPerSample_ + stepIndex / kBitsPerWord];
    return (word & (1ull << (stepIndex % kBitsPerWord))) != 0;
}

size_t OcclusionAccumulator::LitStepCount (size_t sampleIndex) const
{
    if (sampleIndex >= sampleCount_)
        return 0;

    size_t lit = 0;
    const size_t base = sampleIndex * wordsPerSample_;
    for (size_t w = 0; w < wordsPerSample_; ++w) {
        uint64_t word = bits_[base + w];
        while (word != 0) {
            word &= word - 1; // clear the lowest set bit
            ++lit;
        }
    }
    return lit;
}

std::vector<double> OcclusionAccumulator::SunHours (double hoursPerStep) const
{
    std::vector<double> hours (sampleCount_, 0.0);
    for (size_t s = 0; s < sampleCount_; ++s)
        hours[s] = static_cast<double> (LitStepCount (s)) * hoursPerStep;
    return hours;
}

} // namespace evp::sunstudy
