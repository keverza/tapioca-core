#ifndef EVP_ARCHVIZ_DXGI_CAMERACHOICE_HPP
#define EVP_ARCHVIZ_DXGI_CAMERACHOICE_HPP

// See OVERLAY-INVARIANTS.md, section 1, finding 12, and section 4. The two
// decisions the census makes from its measurements: which reading of a group's
// pair wins, and which eligible group becomes the camera. Pure functions of the
// numbers, so a recorded table can be replayed through them with no Archicad.

#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace camerachoice {

// Readings 0..3 are the transposes a shader can declare; 4..7 are reversed
// multiplication orders, which none can (CameraShaderSource.hpp).
constexpr uint32_t kDrawableVariants = 4;

// ⚠️ VALID MOST OFTEN FIRST, then the one putting the MOST of the model on screen
// (finding 12), then the smaller centre error -- and ONLY A READING A SHADER CAN
// DRAW MAY WIN. Finding 12 said a magnifying reading loses on validity before the
// spread tie is reached; 2026-09-27 16:53:10 refuted it: v7 tied v0 at 95/95 valid,
// its mean spread jumped from 64 to 311 px in the last 24 calibration frames, it
// won -- and the injection refuses to draw a reversed order, so the overlay that
// had shown on the provisional pin vanished at the calibrated one.
inline uint32_t WinningVariant (const uint32_t valid[], const float spread[], const float centreError[],
                                uint32_t& winnerValid)
{
    uint32_t best = 0;
    uint32_t bestValid = 0;
    float bestSpread = 0.0f;
    float bestError = 0.0f;
    for (uint32_t variant = 0; variant < kDrawableVariants; ++variant) {
        if (valid[variant] == 0)
            continue;
        if (valid[variant] > bestValid || (valid[variant] == bestValid && spread[variant] > bestSpread) ||
            (valid[variant] == bestValid && spread[variant] == bestSpread && centreError[variant] < bestError)) {
            best = variant;
            bestValid = valid[variant];
            bestSpread = spread[variant];
            bestError = centreError[variant];
        }
    }
    winnerValid = bestValid;
    return best;
}

// What the ranking reads of an eligible group.
struct Standing {
    float coverage = 0.0f;
    float insideClip = 0.0f;
    float centreError = 0.0f;
    uint32_t variant = 0;
    uint32_t indexCount = 0;
};

// ⚠️ COVERAGE, THEN CLIP CONTAINMENT, THEN THE CANONICAL READING, THEN THE MOST
// GEOMETRY, AND CENTRE ERROR ONLY LAST. Error is the weak term -- a hand on a
// mouse does not put the orbit target on the anchor to the pixel -- and ranking on
// it promotes noise. The geometry term is the model's own draws over helpers: every
// camera-bearing draw of a frame is eligible once the camera is read from `b0` and
// `b1` (CameraLayout.hpp), the 24-index helper carries the PREVIOUS image's camera
// (500 of 500 moving images, 2026-09-27 16:56) -- refused while the camera turns,
// not while it only moves -- and the editing plane's 6-index quads exist only while
// its display is on. The model's largest draw is the model's camera.
inline bool Outranks (const Standing& candidate, const Standing* best)
{
    if (best == nullptr || candidate.coverage > best->coverage + 0.01f)
        return true;
    if (candidate.coverage >= best->coverage - 0.01f && candidate.insideClip > best->insideClip + 0.005f)
        return true;
    const bool tied = candidate.coverage >= best->coverage - 0.01f && candidate.insideClip >= best->insideClip - 0.005f;
    if (!tied)
        return false;
    if (candidate.variant == 0 && best->variant != 0)
        return true;
    if (candidate.variant != best->variant)
        return false;
    if (candidate.indexCount != best->indexCount)
        return candidate.indexCount > best->indexCount;
    return candidate.centreError < best->centreError;
}

// ⚠️ A GROUP IS ONE DRAW, AND THE INDEX COUNT SAYS WHICH. The census keys a group
// on its occurrence within a target, and other draws share that occurrence in
// some generations: with the editing plane hidden (2026-09-27 17:24:31) the first
// generation after learning numbered 2D screen-map draws at occurrences 0-6 of the
// scene target -- the model's own occurrences -- and the every-sample gate refused
// 9 of 10 groups. The model's draws never carry a screen map (the pose agreement
// decoded the 1512-index draw as a camera in 530 of 530 images), so the draw whose
// readbacks decode as a camera is the group's camera draw, and a readback that is
// not a camera and came from a DIFFERENT draw says nothing about the group.
// Before the first camera sample nothing counts: a group of screen maps alone never
// accumulates, and never qualifies.
inline bool CountsForGroup (bool decodedAsCamera, uint32_t sampleIndexCount, uint32_t cameraIndexCount)
{
    return decodedAsCamera || (cameraIndexCount != 0 && sampleIndexCount == cameraIndexCount);
}

// ⚠️ AND THE CAMERA IS COPIED ONLY FROM THAT DRAW. The snapshot matches the selected
// occurrence; a screen-map draw at the same occurrence would hand the overlay a
// pixel-to-NDC map for that frame. After an edit the model's draw changes count
// for as long as it takes one readback of it to name the new one.
inline bool IsCameraDraw (uint32_t indexCount, uint32_t cameraIndexCount)
{
    return cameraIndexCount == 0 || indexCount == cameraIndexCount;
}

} // namespace camerachoice
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
