#ifndef EVP_ARCHVIZ_CAMERASYNCMODE_HPP
#define EVP_ARCHVIZ_CAMERASYNCMODE_HPP

// The one seam that selects HOW the overlay follows Archicad's camera
// (PLAT-RE81).
//
// WHY A RUNTIME MODE AND NOT A COMPILE-TIME BRANCH. The camera-sync ladder
// (PLAT-RE54 and its successors) compares several mechanisms against the same
// gestures, and the verdict is a human looking at the screen. If switching
// mechanism cost a build/sync/restart cycle, a two-way comparison would cost the
// user twenty minutes and the comparison would be made from memory. It costs one
// command call instead.
//
// It is also what makes the ladder REVERSIBLE. Every mechanism above `Legacy`
// carries some risk -- a Windows hook, and eventually a detour on Archicad's own
// D3D11 present path -- and each must be retractable in the same session that
// armed it, without a rebuild. `Off` is the panic button; `Legacy` is exactly
// today's behaviour and is never removed, so it stays a live fallback rather
// than a code path somebody would have to restore.
//
// ⚠️ THE MODE IS PROCESS STATE AND IS NEVER PERSISTED. An Archicad restart
// always comes back on `Legacy`. That is the second half of reversibility: the
// worst case for a bad experiment is "restart Archicad", never "repair a
// settings file". Anything experimental additionally goes through
// `ArchViz/ExperimentGuard`, which covers the case where Archicad cannot be
// restarted successfully at all.
//
// ⚠️ MAIN THREAD ONLY. Arming and disarming touch Win32 timers and hooks, which
// belong to the thread that pumps the queue -- the same reason
// `SetDiligentCameraSync` does not `Post`. `Current()` is a plain read and is
// safe anywhere.

#include <cstdint>
#include <string>

namespace geomsrv {
namespace archviz {

enum class CameraSyncMode {
    Off,        // no sync; the overlay holds its last pose
    Legacy,     // today's SetTimer poll -- the default, and the fallback
    Wake,       // coalesced input wake signal (PLAT-RE75)
    Predict,    // extrapolation to the next present (PLAT-RE76/RE80)
    WakePredict,// BOTH: input-driven sampling AND extrapolation (PLAT-RE92). The
                // 2026-08-13 frame clock showed Archicad redrawing at ~53 Hz while
                // our poll ran at 32-44 Hz, and the measured lag tracked the POLL
                // interval rather than the frame time -- we are one sample behind
                // because we sample less often than Archicad draws. Sampling on
                // input shortens that interval; predicting across it removes what
                // is left. Kept separate from `predict` so the two are still
                // comparable one at a time.
    HideOnNav,  // legacy poll with blanking forced on (PLAT-RE83). ⚠️ KEPT ONLY
                // AS A NAME: blanking is now the independent `hideOnNav` switch
                // below, and this mode is exactly `legacy` with that switch
                // pinned true. Every probe and script that already sends
                // `mode: "hideonnav"` keeps meaning what it meant.
    HookDiag,   // DXGI Present detour, log only (PLAT-RE78)
    HookDraw    // DXGI Present detour, compositing (PLAT-RE79), on the SAME
                // sampling and prediction as `wakepredict` -- see PLAT-RE116 and
                // the arm switch in the .cpp for why it may not be anything else.
};

// False if `name` is not one of the mode names.
// "off", "legacy", "hideonnav", "wake", "predict", "wakepredict", "hookdiag",
// "hookdraw".
bool        ParseCameraSyncMode (const std::string& name, CameraSyncMode& mode);
const char* CameraSyncModeName (CameraSyncMode mode);

// True for the modes that go through the experiment guard and can, if they go
// wrong, take Archicad down with them.
bool IsExperimental (CameraSyncMode mode);

// Tear down whatever is live and arm `mode`. Returns false with `error` filled,
// and ⚠️ LEAVES THE CURRENT MODE UNTOUCHED, when the request cannot be honoured
// -- an unimplemented mode or a typo must not silently stop a sync that was
// working. Only a request that will succeed tears anything down.
//
// `hideOnNav` is INDEPENDENT OF THE MODE and composes with all of them -- see
// `CurrentHideOnNav` below. `CameraSyncMode::HideOnNav` ignores the argument and
// pins it true.
bool SetCameraSyncMode (CameraSyncMode mode, uint32_t intervalMs, double predictionScale,
                        bool hideOnNav, std::string& error);

CameraSyncMode CurrentCameraSyncMode ();
uint32_t       CurrentCameraSyncIntervalMs ();

// Whether the overlay blanks itself while the view is moving (PLAT-RE83).
//
// ⚠️ IT IS A SWITCH AND NOT A MODE ANY MORE (PLAT-RE116). Blanking is orthogonal
// to WHERE the camera comes from (timer or input wake), to WHETHER it is
// extrapolated, and to WHERE the pixels land (our window or Archicad's back
// buffer) -- so expressing it as one more name in an enum of mechanisms made
// every useful pairing unreachable: `hideonnav` could only ever be had at the
// price of dropping back to the starved legacy poll. That combinatorial trap is
// the same one that made `hookdraw` measure against the worst sample stream in
// the tree.
//
// ⚠️ DEFAULT ON. The overlay's whole claim is that the two pictures agree, and a
// dependent that reads a mid-drag frame is shown a pose that is wrong by
// construction. Blanking is the only answer that is never wrong; being briefly
// empty is a state a caller can see and wait out, being subtly stale is not.
//
// ⚠️ IT DEGRADES, IT DOES NOT FAIL. Blanking on the input itself needs the wake
// hook, which only the hook-installing modes have. Under `legacy`/`predict` the
// timer fallback in `ApplyHideOnNavigation` blanks on the CONSEQUENCE instead
// and is a tick late by construction -- documented there, not a bug to fix here.
bool CurrentHideOnNav ();

// How far ahead the predictor aims, as a MULTIPLE of the measured inter-sample
// interval. 1.0 means "predict exactly one sample ahead".
//
// ⚠️ IT IS A KNOB BECAUSE THE RIGHT VALUE IS AN EMPIRICAL QUESTION, and every
// previous attempt to answer it by reasoning was wrong. The horizon that
// actually cancels the lag is the distance from the sample to the PRESENT that
// uses it -- one poll interval, plus however deep the render and composition
// pipeline is. The 2026-08-13 wakepredict run measured 25 ms of lag falling to
// 15 ms at scale 1.0: prediction is working, and one sample ahead is not far
// enough. Rather than pick another constant, sweep it -- CLAUDE.md's probe rule,
// which exists precisely for conventions like this one.
double         CurrentPredictionScale ();

// Force everything down, ignoring errors. For viewport teardown and add-on exit,
// where there is nothing useful to do with a failure.
void ShutDownCameraSync ();

}   // namespace archviz
}   // namespace geomsrv

#endif
