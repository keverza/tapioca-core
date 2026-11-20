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

struct StepMaskAtlas {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t words = 0; // array slices
    uint32_t steps = 0;
    // Slice-major: masks[word * width * height + texel]. Texels no sample
    // lands on stay 0; the hours atlas's sentinel says so, not this.
    std::vector<uint32_t> masks;

    // The shader's reads of these masks (Lit, ShadowedBetween, FanRank) are
    // pinned against kArchVizSunTintPS in the Rust core's step_atlas tests.
};

// The fan's selection travels in one float4 of the constant buffer: three
// words of step bits, so the fan covers the first 96 steps -- a whole day at
// 15 minutes.
constexpr uint32_t kFanWords = 3;
constexpr uint32_t kFanMaxSteps = 32 * kFanWords;

} // namespace evp::sunstudy

#endif // EVP_SUNSTUDY_SUNSTUDYSTEPATLAS_HPP
