#ifndef EVP_SUNSTUDY_SUNSTUDYPROGRESS_HPP
#define EVP_SUNSTUDY_SUNSTUDYPROGRESS_HPP

// SunStudy/SunStudyProgress -- how far a study has got, and the three input
// versions (geometry, sun, grid) a study is synced against.

#include <cstddef>
#include <cstdint>

namespace evp::sunstudy {

// What a caller needs to draw a progress readout and decide whether to publish.
struct StudyProgress {
    uint64_t generation = 0;
    size_t resolvedSteps = 0;
    size_t totalSteps = 0;
    size_t sampleCount = 0;

    // ⚠️ THE ONLY HONEST GATE ON AN HOURS FIGURE.
    bool converged = false;

    // No samples, or no sun above the horizon. Distinguished from converged
    // because "the study finished" and "there was nothing to study" produce the
    // same empty numbers and mean very different things.
    bool empty = true;
};

// What changed since the last call. Zero means "no such input" rather than
// "unchanged" for every one of them, matching ITraversal::SceneVersion and
// SunSeries::Version.
struct StudyInputs {
    uint64_t geometryVersion = 0;
    uint64_t sunVersion = 0;
    uint64_t gridVersion = 0;
};

} // namespace evp::sunstudy

#endif
