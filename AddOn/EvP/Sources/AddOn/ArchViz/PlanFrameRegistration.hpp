#ifndef EVP_ARCHVIZ_PLANFRAMEREGISTRATION_HPP
#define EVP_ARCHVIZ_PLANFRAMEREGISTRATION_HPP

// ArchViz/PlanFrameRegistration -- how far one presented floor-plan frame moved
// from the one before it, measured from the pixels alone.
//
// WHY IT EXISTS. The floor plan has no camera on the GPU (OVERLAY-INVARIANTS.md
// frozen finding 13): Archicad maps the model to pixels on the CPU and the only
// description of that mapping an add-on can read is ACAPI's, on the main thread.
// Whether an overlay composed at the plan's Present can know the transform its
// frame was drawn with is therefore a question about TIME -- which ACAPI read,
// taken when, describes the frame -- and bookkeeping cannot answer it (§7: what
// reaches the screen is measured from pixels). This measures the answer's ground
// truth: between two consecutive frames of the plan, the similarity that carries
// the current frame onto the previous one. Scoring each candidate ACAPI read
// against it is the diagnostic's job, not this file's.
//
// THE METHOD. Nine patches on a 3x3 grid of the current frame are found in the
// previous one: an exhaustive search at a quarter resolution, a +-4 sample
// refinement at full resolution, then a similarity fit (scale, rotation,
// translation) through the patch centres with outliers dropped. A final pass
// re-measures every patch against the previous frame WARPED by that fit, so a
// zoom step -- where no patch is a pure translation -- is measured as well as a
// pan.
//
// ⚠️ A PATCH WITH NOTHING IN IT IS SKIPPED, NOT MATCHED. A plan is mostly paper;
// an empty patch matches everywhere equally well and would vote for a random
// displacement. Texture and the sharpness of the match are both gated.
//
// ⚠️ NO WINDOWS, NO D3D, NO ACAPI. Pure arithmetic over grey images, tested
// offline in tests/cpp/test_planframeregistration.cpp with frames whose motion is
// known exactly.

#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace planframes {

// A borrowed 8-bit grey image; `stride` is bytes per row.
struct GreyImage {
    const uint8_t* pixels = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
};

// Where the CURRENT frame's content was in the PREVIOUS frame, in samples of the
// images passed:
//
//     previous.x = a * current.x - b * current.y + offsetX
//     previous.y = b * current.x + a * current.y + offsetY
//
// `a = scale * cos(rotation)`, `b = scale * sin(rotation)`. A pan is a = 1, b = 0;
// a zoom in makes the previous content larger on screen, so a < 1.
struct FrameMotion {
    bool valid = false;
    // Why not, for a log a human reads. Static text; never freed.
    const char* why = "";
    double a = 1.0;
    double b = 0.0;
    double offsetX = 0.0;
    double offsetY = 0.0;
    // RMS distance of the kept patch correspondences from the fit, in samples.
    double residual = 0.0;
    uint32_t patchesTextured = 0; // patches with enough texture to be searched
    uint32_t patchesUsed = 0;     // of those, the ones the final fit kept
};

// The images must be the same size and at least 128 x 96 samples. The search
// reaches +-64 samples; a frame that moved further is reported as unmatched
// rather than matched to the wrong place, as far as the gates can tell.
FrameMotion MeasureFrameMotion (const GreyImage& previous, const GreyImage& current);

// The displacement the fit gives the point (x, y): previous minus current.
void MotionAt (const FrameMotion& motion, double x, double y, double& dx, double& dy);

} // namespace planframes
} // namespace archviz
} // namespace geomsrv

#endif
