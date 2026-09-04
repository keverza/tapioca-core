#ifndef EVP_NATIVECOMMANDS_SUNSTUDYCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// The sun study's bus surface: StartSunStudy, AdvanceSunStudy, SunStudyState,
// GetSunStudyResults, CancelSunStudy.
//
// ⚠️ THE SPLIT BETWEEN START AND ADVANCE IS THE WHOLE DESIGN, not a convenience.
// Starting needs the host's main thread, because the sun position comes from
// Archicad and nothing else may compute it. Advancing must NOT hold that thread,
// because it is the expensive part and holding it is what would stutter the
// application. So `StartSunStudy` is a main-thread command that gathers the day's
// sun vectors once, and `AdvanceSunStudy` is gate-free and does the analysis.
//
// A caller therefore starts once, advances in as many bounded slices as it
// likes, and reads progress between them. That is what makes a long study
// interruptible and a partial result available, and it is why no single command
// here ever runs a whole study.
NativeCommandRegistrations GetSunStudyCommandRegistrations ();

} // namespace geomsrv

#endif
