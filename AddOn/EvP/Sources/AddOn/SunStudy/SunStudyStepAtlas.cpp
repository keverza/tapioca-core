#include "SunStudy/SunStudyStepAtlas.hpp"

#include "SunStudy/SunSeries.hpp"

namespace evp::sunstudy {

bool StepMaskAtlas::Lit (size_t texel, size_t step) const
{
    const size_t plane = static_cast<size_t> (width) * height;
    const size_t word = step >> 5;
    if (step >= steps || texel >= plane || word >= words)
        return false;
    return ((masks[word * plane + texel] >> (step & 31u)) & 1u) != 0u;
}

bool StepMaskAtlas::ShadowedBetween (size_t texel, uint32_t first, uint32_t last) const
{
    const size_t plane = static_cast<size_t> (width) * height;
    if (first >= last || texel >= plane)
        return false;
    const uint32_t lastWord = (last - 1u) >> 5;
    for (uint32_t word = first >> 5; word <= lastWord && word < words; ++word) {
        const uint32_t bits = masks[word * plane + texel];
        const uint32_t lo = (first > word * 32u ? first : word * 32u) - word * 32u;
        const uint32_t hi = (last < word * 32u + 32u ? last : word * 32u + 32u) - word * 32u;
        const uint32_t below = hi >= 32u ? 0xffffffffu : ((1u << hi) - 1u);
        const uint32_t range = below & ~((1u << lo) - 1u);
        if (((~bits) & range) != 0u)
            return true;
    }
    return false;
}

StepMaskAtlas PackStepMasks (size_t sampleCount, size_t stepCount, const std::function<bool (size_t, size_t)>& lit,
                             const std::vector<int64_t>& texelOfSample, uint32_t width, uint32_t height)
{
    StepMaskAtlas out;
    out.width = width;
    out.height = height;
    out.steps = static_cast<uint32_t> (stepCount);
    // ⚠️ AT LEAST ONE SLICE, even for a study of no steps: the shader always
    // binds the texture, and a zero-slice array is not a texture at all.
    out.words = static_cast<uint32_t> ((stepCount + 31) / 32);
    if (out.words == 0)
        out.words = 1;
    const size_t plane = static_cast<size_t> (width) * height;
    out.masks.assign (plane * out.words, 0u);
    if (!lit)
        return out;

    for (size_t sample = 0; sample < sampleCount && sample < texelOfSample.size (); ++sample) {
        const int64_t texel = texelOfSample[sample];
        if (texel < 0 || static_cast<size_t> (texel) >= plane)
            continue;
        for (size_t step = 0; step < stepCount; ++step) {
            if (lit (sample, step))
                out.masks[(step >> 5) * plane + static_cast<size_t> (texel)] |= 1u << (step & 31u);
        }
    }
    return out;
}

uint32_t SolarNoonStep (const SunSeries& series)
{
    uint32_t best = 0;
    for (size_t step = 1; step < series.StepCount (); ++step)
        if (series.Step (step).altitudeDegrees > series.Step (best).altitudeDegrees)
            best = static_cast<uint32_t> (step);
    return best;
}

std::vector<uint16_t> StepMinutes (const SunSeries& series)
{
    std::vector<uint16_t> out;
    out.reserve (series.StepCount ());
    for (size_t step = 0; step < series.StepCount (); ++step) {
        const TimeOfDay& time = series.Step (step).time;
        out.push_back (static_cast<uint16_t> (time.hour * 60 + time.minute));
    }
    return out;
}

} // namespace evp::sunstudy
