#include "SunStudy/SunStudyOcclusion.hpp"

#include <algorithm>

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
}

bool OcclusionAccumulator::AccumulateStep (const ITraversal& traversal, const SampleSet& samples, size_t stepIndex,
                                           const double sunDirection[3], double tmin, double tmax, size_t maxParallel)
{
    if (stepIndex >= stepCount_ || sunDirection == nullptr)
        return false;
    if (samples.count != sampleCount_)
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

    // Re-resolving a step must not double-count, so its bit is cleared first.
    for (size_t s = 0; s < sampleCount_; ++s)
        bits_[s * wordsPerSample_ + word] &= ~bit;

    // ⚠️ BACK-FACING SAMPLES ARE CULLED BEFORE THE RAY, NOT AFTER. A surface
    // turned away from the sun is self-shadowed whatever the geometry does, so
    // tracing it is pure waste — and around half the samples are back-facing at
    // any one moment, which makes this a factor-of-two on the whole study rather
    // than a micro-optimisation. Compacting into a dense array keeps the
    // traversal's work contiguous.
    frontFacingOrigins_.clear ();
    frontFacingIndex_.clear ();
    frontFacingOrigins_.reserve (sampleCount_ * 3);
    frontFacingIndex_.reserve (sampleCount_);

    for (size_t s = 0; s < sampleCount_; ++s) {
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
    if (traced > 0) {
        occluded_.assign (traced, 0u);
        traversal.OccludeDirectional (frontFacingOrigins_.data (), traced, sunDirection, tmin, tmax, occluded_.data (),
                                      maxParallel);

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
                                              double tmax, size_t maxParallel)
{
    size_t done = 0;
    const size_t limit = std::min (series.StepCount (), stepCount_);
    for (size_t step = firstStep; step < limit && done < maxSteps; ++step) {
        if (!AccumulateStep (traversal, samples, step, series.Step (step).direction, tmin, tmax, maxParallel))
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
