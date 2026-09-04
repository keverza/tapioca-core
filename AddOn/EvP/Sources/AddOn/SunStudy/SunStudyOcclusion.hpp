#ifndef EVP_SUNSTUDY_SUNSTUDYOCCLUSION_HPP
#define EVP_SUNSTUDY_SUNSTUDYOCCLUSION_HPP

// SunStudy/SunStudyOcclusion — how many hours of sun each sample gets.
//
// The accumulator holds ONE BIT PER (SAMPLE, TIMESTEP): set means the sample saw
// the sun at that step. Everything the study reports is a read of that bitset —
// total hours, the shadow at one instant, an interval fan, a compliance
// threshold — so switching between them costs nothing and recomputes nothing.
//
// ⚠️ THE BITSET IS THE WHOLE DESIGN, NOT AN OPTIMISATION. Storing only a count
// per sample would make "how many hours" free and every other question a fresh
// study. The previous implementation learned this the expensive way and reached
// the same shape from the other side, by packing per-step masks into the bits of
// a float texture so one read served every mode.
//
// ⚠️ ONE STEP AT A TIME, ON PURPOSE. AccumulateStep is the unit of progress: a
// caller advances as many steps as its frame or slice budget allows and comes
// back later, so a long study never blocks the thread that asked for it. That is
// also why the scratch buffers live in the accumulator rather than in the call —
// a per-step allocation would dominate a study made of thousands of small
// slices.

#include "SunStudy/ITraversal.hpp"
#include "SunStudy/SunSeries.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace evp::sunstudy {

// The analysis points. Non-owning: the caller keeps the arrays alive.
struct SampleSet {
    // xyz interleaved, `count` positions, already offset off their surface.
    const double* positions = nullptr;

    // xyz interleaved unit normals, or null for "no orientation".
    //
    // ⚠️ WITHOUT NORMALS EVERY SAMPLE IS TREATED AS FACING THE SUN, which is
    // right for a bare ground grid and wrong for a facade: the back of a wall
    // would count the sun that strikes its front. Supply them for anything with
    // an orientation.
    const double* normals = nullptr;

    size_t count = 0;
};

class OcclusionAccumulator final {
  public:
    OcclusionAccumulator (size_t sampleCount, size_t stepCount);

    size_t SampleCount () const
    {
        return sampleCount_;
    }
    size_t StepCount () const
    {
        return stepCount_;
    }

    // Clears every bit and every step's done-flag.
    void Reset ();

    // Resolve one timestep for every sample.
    //
    // Returns false for an out-of-range step or a mismatched sample set, having
    // changed nothing. A step already resolved is re-resolved rather than
    // refused, so a caller that loses track cannot corrupt the total.
    //
    // `tmin` skips the surface the sample sits on; `tmax <= 0` is unbounded.
    bool AccumulateStep (const ITraversal& traversal, const SampleSet& samples, size_t stepIndex,
                         const double sunDirection[3], double tmin = 0.001, double tmax = 0.0, size_t maxParallel = 0);

    // Convenience: resolve `[firstStep, firstStep + maxSteps)` of a series.
    // Returns how many it actually resolved.
    size_t AccumulateRange (const ITraversal& traversal, const SampleSet& samples, const SunSeries& series,
                            size_t firstStep, size_t maxSteps, double tmin = 0.001, double tmax = 0.0,
                            size_t maxParallel = 0);

    bool StepResolved (size_t stepIndex) const;
    size_t ResolvedStepCount () const
    {
        return resolvedCount_;
    }
    bool Complete () const
    {
        return stepCount_ == 0 || resolvedCount_ == stepCount_;
    }

    // Was this sample sunlit at this step?
    bool Lit (size_t sampleIndex, size_t stepIndex) const;

    // How many resolved steps this sample was sunlit for.
    size_t LitStepCount (size_t sampleIndex) const;

    // Sun hours per sample, from the steps resolved SO FAR.
    //
    // ⚠️ AN INCOMPLETE ACCUMULATOR YIELDS A NUMBER THAT IS TOO LOW, NOT A WRONG
    // ONE — but too low is indistinguishable from a shadier site, so a caller
    // must publish an hours figure only when Complete() is true. Check the flag;
    // do not infer it from the values.
    std::vector<double> SunHours (double hoursPerStep) const;

    // The raw per-sample bitset: `WordsPerSample()` 64-bit words per sample,
    // least-significant bit of word 0 being step 0. Handed out so a client can
    // ship it whole rather than one query at a time.
    const std::vector<uint64_t>& Bits () const
    {
        return bits_;
    }
    size_t WordsPerSample () const
    {
        return wordsPerSample_;
    }

  private:
    size_t sampleCount_ = 0;
    size_t stepCount_ = 0;
    size_t wordsPerSample_ = 0;
    size_t resolvedCount_ = 0;

    std::vector<uint64_t> bits_;
    std::vector<uint8_t> stepResolved_;

    // Reused across steps; see the header note on per-step allocation.
    mutable std::vector<double> frontFacingOrigins_;
    mutable std::vector<uint32_t> frontFacingIndex_;
    mutable std::vector<uint8_t> occluded_;
};

} // namespace evp::sunstudy

#endif
