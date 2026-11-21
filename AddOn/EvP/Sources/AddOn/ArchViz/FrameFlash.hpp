#pragma once

// ArchViz/FrameFlash -- tells a one- or two-frame flash from a real change of the picture (#14).
//
// Live: single frames in Technical with every surface near white (224-255), the clear colour, the
// edges and the camera unchanged. The light constants were logged per frame and did not move
// (NoteLightChange), and RenderBench never reproduced it, so the frame itself is measured: its mean
// level (DiligentFrameFlashWatch, a few patches read back a frame or two late) goes in here.
//
// A frame more than kJump levels from the running baseline is held. If the picture is back near the
// baseline within kMaxFlashFrames, those frames were a flash and are reported; if it stays, it was a
// real change (a style switch, a cut, a model arriving) and becomes the new baseline, unreported.

#include <cmath>
#include <cstdint>

namespace geomsrv {
namespace archviz {

struct FrameFlashDetector {
    static constexpr float kJump = 30.0f;          // levels of 255 away from the baseline
    static constexpr uint32_t kMaxFlashFrames = 2; // longer is a change, not a flash
    static constexpr float kFollow = 0.3f;         // the baseline's step towards an ordinary frame

    float baseline = -1.0f;
    uint32_t held = 0;      // frames off the baseline in a row
    float heldPeak = 0.0f;  // the farthest of them, signed
    uint64_t heldFirst = 0; // the first of them

    struct Result {
        bool flash = false; // the frames [first, first + frames) were a flash
        uint64_t first = 0;
        uint32_t frames = 0;
        float baseline = 0.0f;
        float peak = 0.0f; // the flash's level
    };

    Result Add (uint64_t frame, float level)
    {
        Result r;
        if (baseline < 0.0f) {
            baseline = level;
            return r;
        }
        const float off = level - baseline;
        if (std::abs (off) > kJump) {
            if (held == 0) {
                heldFirst = frame;
                heldPeak = off;
            }
            else if (std::abs (off) > std::abs (heldPeak))
                heldPeak = off;
            if (++held > kMaxFlashFrames) { // it stayed: the picture changed
                baseline = level;
                held = 0;
            }
            return r;
        }
        if (held > 0) {
            r.flash = true;
            r.first = heldFirst;
            r.frames = held;
            r.baseline = baseline;
            r.peak = baseline + heldPeak;
            held = 0;
        }
        baseline += kFollow * (level - baseline);
        return r;
    }
};

} // namespace archviz
} // namespace geomsrv
