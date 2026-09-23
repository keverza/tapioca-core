#ifndef EVP_SUNSTUDY_SUNSTUDYSTEPATLAS_HPP
#define EVP_SUNSTUDY_SUNSTUDYSTEPATLAS_HPP

// SunStudy/SunStudyStepAtlas -- the study's per-TIMESTEP lit bits, laid out
// texel for texel like the hours atlas, for the shadow views.
//
// The hours atlas answers "how long"; the shadow views ask "WHEN": lit or
// shadowed at one moment (single shadow), and shadowed before or after solar
// noon (AM / PM). Both need every sample's whole day, which the hours atlas has
// already summed away.
//
// ⚠️ THE SAME TEXEL AS THE HOURS ATLAS, NOT A LAYOUT OF ITS OWN. The shader
// finds a pixel's texel once, through the face record, and reads both images
// there; a second layout would need a second mapping, and two mappings are how
// the hours and the shadow of one surface come to describe different places.
//
// Packed 32 steps to a uint32 word, one ARRAY SLICE per word: slice k holds
// steps 32k .. 32k+31, bit (step & 31). A 15-minute study of a long summer day
// is ~70 steps -- three slices.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace evp::sunstudy {

class SunSeries;

struct StepMaskAtlas {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t words = 0; // array slices
    uint32_t steps = 0;
    // Slice-major: masks[word * width * height + texel]. Texels no sample
    // lands on stay 0; the hours atlas's sentinel says so, not this.
    std::vector<uint32_t> masks;

    // The shader's read, for tests.
    bool Lit (size_t texel, size_t step) const;

    // Whether the texel was SHADOWED at any step in [first, last), a WORD at a
    // time. ⚠️ THE SAME ALGORITHM AS kArchVizSunTintPS's ShadowedIn, line for
    // line: the range masks at word boundaries are where a transliteration
    // goes wrong, so the arithmetic is pinned here against a per-step answer.
    bool ShadowedBetween (size_t texel, uint32_t first, uint32_t last) const;
};

// `lit(sample, step)` is the study's answer; `texelOfSample[i]` is where the
// hours atlas put sample i, or negative for none.
StepMaskAtlas PackStepMasks (size_t sampleCount, size_t stepCount, const std::function<bool (size_t, size_t)>& lit,
                             const std::vector<int64_t>& texelOfSample, uint32_t width, uint32_t height);

// The step at which the sun stands highest -- solar noon as the project itself
// reports it, the web study's AM / PM split. ⚠️ NOT 12:00: clock noon is off
// solar noon at most longitudes, and by an hour under daylight saving.
uint32_t SolarNoonStep (const SunSeries& series);

// Each step's time of day in minutes, for the HUD's time slider.
std::vector<uint16_t> StepMinutes (const SunSeries& series);

} // namespace evp::sunstudy

#endif // EVP_SUNSTUDY_SUNSTUDYSTEPATLAS_HPP
