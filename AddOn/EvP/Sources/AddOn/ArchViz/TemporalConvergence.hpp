#ifndef EVP_ARCHVIZ_TEMPORALCONVERGENCE_HPP
#define EVP_ARCHVIZ_TEMPORALCONVERGENCE_HPP

// ArchViz/TemporalConvergence -- what the viewer does with a still camera
// (issues #14 P1, #247, #248).
//
// TAA alone never settles. It averages about ten jittered frames and clamps the
// history to the current frame's neighbourhood, so a still image keeps moving by
// a few levels at every edge, reflection and soft shadow. The passes drawn after
// the resolve (the sun-study tint, section back faces and caps, wireframe, edges,
// outlines) are not averaged at all and move by up to half a pixel every frame.
//
// With the camera and the scene unchanged, this decides:
//
//   Moving       -> the camera or the content changed this frame: present TAA
//   Settling     -> still, but TAA's history still holds the motion: present TAA
//   Accumulating -> fold TAA's output into a running mean, present the mean
//   Converged    -> the mean is final: present it, and STOP THE JITTER, so the
//                   passes after the resolve draw against an unjittered depth
//                   buffer and stop moving as well
//
// ⚠️ THE MEAN IS OF TAA'S OUTPUT, NOT THE RAW FRAMES. The first accumulated
// sample is then exactly the picture that was already on screen, so starting the
// mean causes no visible step, and the converged image still has every jitter
// position of the Halton cycle in it (the target is a whole number of cycles).
//
// ⚠️ ANY CHANGE RESTARTS IT, AND "ANY" IS A FINGERPRINT THE CALLER BUILDS. The
// camera key is the unjittered view and projection plus the viewport size; the
// content key is whatever the renderer folded in while drawing. A setting left
// out of that key keeps the converged mean on screen after the change, so the
// renderer feeds it everything its HDR passes read (DiligentSceneDraw.cpp).
//
// PURE: no Diligent, no ACAPI -- the offline suite runs it directly.

#include <cstddef>
#include <cstdint>
#include <string>

namespace geomsrv {
namespace archviz {

enum class TemporalPhase : uint8_t {
    Off,          // not running: TAA off, the feature switched off, or a traced view owns the image
    Moving,       // the camera or the content changed this frame
    Settling,     // still, waiting for TAA's history to forget the motion
    Accumulating, // still, building the mean
    Converged,    // still, the mean is final and the jitter is off
};

const char* TemporalPhaseName (TemporalPhase phase);

// What last threw a still image back to Moving: the HUD readout and the log, so a view that keeps
// restarting on its own says which input moves (#247 live: "stable for a few seconds, then jitters").
enum class TemporalRestart : uint8_t {
    None,
    Camera,     // the view, the projection or the viewport size changed
    Content,    // the content key changed: a setting, a material, an element, an effect switching on
    Unresolved, // TAA or its coverage produced nothing this frame
    Reset,      // an explicit history break (Reset)
};

const char* TemporalRestartName (TemporalRestart reason);

// FNV-1a, 64-bit. The content key: fold in every value the frame's HDR image depends on.
class ContentKey {
  public:
    void Add (const void* data, size_t bytes);
    void Add (float value);
    void Add (uint64_t value);
    void Add (bool value)
    {
        Add (uint64_t (value ? 1u : 0u));
    }
    uint64_t Value () const
    {
        return hash_;
    }

  private:
    uint64_t hash_ = 1469598103934665603ull;
};

struct TemporalConvergenceSettings {
    bool enabled = true;
    // Still frames before the mean starts, so TAA's history has forgotten the motion and AO's
    // and SSR's own histories are close to converged: whatever the mean starts with, it keeps.
    // ⚠️ MEASURED, NOT GUESSED (RenderBench --temporal, bathroom mirror, every history reset,
    // the worst case): the converged mean against the steady image is +0.47 levels of 255
    // overall (+2.2 on the mirror) at 8, +0.26 (+1.2) at 24, +0.13 (+0.67) at 48. Below one
    // level everywhere; 16 trades a little of that for freezing sooner.
    uint32_t settleFrames = 16;
    // A whole number of DiligentFX's 16-position Halton cycles, so every jitter position is
    // in the mean the same number of times.
    uint32_t targetSamples = 64;
    // #14 moire: the pixel filter the still image is the mean of, as a tent's radius in pixels. ⚠️ A
    // MEAN OF SAMPLES INSIDE THE PIXEL (+/-0.5, DiligentFX's Halton) IS A 1-PIXEL BOX FILTER, and a
    // box passes 23 % of a pattern at 0.8 of the pixel frequency -- 30 mm louvres a few pixels apart
    // fold into the wide arcs the live run showed in every settled style. A tent of radius 1 passes
    // 5 % there. Only the still image's own samples move (Accumulating); TAA while moving keeps its
    // in-pixel jitter. The Ray traced style's samples use the same tent. <= 0.5 is the old box.
    float filterRadiusPixels = 1.0f;
    // #248: on a jittered frame, redraw the depth buffer without the jitter after the resolve and
    // draw everything after it (tint, section, wireframe, edges, outlines, previews, labels) with
    // the unjittered camera, so those marks stand still while TAA and the mean are still moving.
    // Independent of `enabled`: it costs one depth-only pass and needs no still camera.
    bool stableOverlays = true;
    // #247: once converged, a frame whose camera and content key are unchanged draws nothing of the
    // scene again -- no shadow map, no G-buffer, no geometry, no SSR, no TAA -- and presents the
    // mean over a depth-only redraw, so the overlays and the HUD stay live (DiligentSceneTemporal.cpp).
    bool idleWhenConverged = true;
};

// A sample offset in pixels from the pixel's centre, drawn from a tent of `radius` pixels: the
// inverse of its cumulative distribution at `u` in [0, 1]. Monotonic, so a stratified `u` (a Halton
// position) stays stratified. `radius` <= 0.5 is the plain in-pixel box, u - 0.5.
float TentOffsetPixels (float u, float radius);

// The readout (scene and viewport stats, the HUD's "still image" line, RenderBench).
struct TemporalStatus {
    TemporalPhase phase = TemporalPhase::Off;
    uint32_t samples = 0;
    uint32_t targetSamples = 0;
    bool idle = false;     // this frame presented the mean without drawing the scene
    uint32_t restarts = 0; // interruptions of a still image (Settling or later)
    TemporalRestart lastRestart = TemporalRestart::None;
    uint32_t restartsBy[5] = {}; // `restarts` by TemporalRestart
};

// #14: the still image over the last few seconds, for the log. A camera change and an explicit
// Reset restart the mean without a log line of their own, so a view that "flickers rhythmically and
// never settles" in Archicad but settles in RenderBench needs these counts to say which one it is.
class TemporalActivityLog {
  public:
    explicit TemporalActivityLog (double windowSeconds = 10.0) : window_ (windowSeconds)
    {
    }
    // Once a frame, after BeginFrame. Returns a line when a window closes, else empty. A window in
    // which nothing temporal ran says nothing.
    std::string Note (const TemporalStatus& status, bool idle, double nowSeconds);

  private:
    double window_;
    double started_ = -1.0;
    uint32_t frames_ = 0;
    uint32_t phaseFrames_[5] = {};
    uint32_t idleFrames_ = 0;
    uint32_t restartsAtStart_[5] = {};
    bool haveStart_ = false;
};

// What the renderer does with the accumulator this frame.
struct TemporalStep {
    bool accumulate = false; // blend this frame's TAA output into the mean ...
    float weight = 0.0f;     // ... with this weight (1 on the first sample: the mean is overwritten)
    bool present = false;    // show the mean instead of this frame's TAA output
};

class TemporalConvergence {
  public:
    // Start of the frame, before the jitter is chosen. `running` is false when nothing temporal
    // runs this frame (TAA off, a debug view, an orthographic camera); the state then drops to
    // Off and the next frame that runs starts from Moving.
    void BeginFrame (const float view[16], const float projection[16], uint32_t width, uint32_t height, bool running,
                     const TemporalConvergenceSettings& settings);

    // Whether this frame is jittered. False only once the mean is converged.
    bool JitterThisFrame () const
    {
        return phase_ != TemporalPhase::Converged;
    }

    // After TAA. `resolved` is false when TAA (or its coverage) produced nothing this frame,
    // which leaves nothing to fold in; the state restarts as if the camera had moved.
    TemporalStep EndFrame (uint64_t contentKey, bool resolved);

    // An explicit history break (camera cut, projection toggle, resize, capture), or a change the
    // caller found itself (an idle frame's content key), named by `reason`.
    void Reset (TemporalRestart reason = TemporalRestart::Reset);

    TemporalPhase Phase () const
    {
        return phase_;
    }
    uint32_t Samples () const
    {
        return samples_;
    }
    uint32_t TargetSamples () const
    {
        return settings_.targetSamples;
    }
    TemporalStatus Status () const
    {
        TemporalStatus status;
        status.phase = phase_;
        status.samples = samples_;
        status.targetSamples = settings_.targetSamples;
        status.restarts = restarts_;
        status.lastRestart = lastRestart_;
        for (int i = 0; i < 5; ++i)
            status.restartsBy[i] = restartsBy_[i];
        return status;
    }

  private:
    void Restart (TemporalPhase phase, TemporalRestart reason);

    TemporalConvergenceSettings settings_;
    TemporalPhase phase_ = TemporalPhase::Off;
    uint64_t cameraKey_ = 0;
    uint64_t contentKey_ = 0;
    bool haveCamera_ = false;
    bool haveContent_ = false;
    bool cameraMovedThisFrame_ = true;
    uint32_t stillFrames_ = 0;
    uint32_t samples_ = 0;
    uint32_t restarts_ = 0;
    uint32_t restartsBy_[5] = {};
    TemporalRestart lastRestart_ = TemporalRestart::None;
};

} // namespace archviz
} // namespace geomsrv

#endif
