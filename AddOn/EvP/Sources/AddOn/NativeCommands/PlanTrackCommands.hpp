#ifndef EVP_NATIVECOMMANDS_PLANTRACKCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_PLANTRACKCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Navigation sync for the plan overlay (§15): the model->pixel transform, the
// pan/zoom poll, and the model-space geometry the overlay draws.
//
// Separate from PlanOverlayCommands because that domain answers "can a window
// live there at all" (Phase 0, answered) and this one answers "does it stay
// registered to the drawing while the user works" (Phase 2D-2). They share the
// window itself, which lives in PlanOverlay/OverlayWindow.
//
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetPlanTrackCommandRegistrations ();

}   // namespace geomsrv

#endif
