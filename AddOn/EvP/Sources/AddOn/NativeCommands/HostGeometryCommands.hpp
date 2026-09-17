#ifndef EVP_NATIVECOMMANDS_HOSTGEOMETRYCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_HOSTGEOMETRYCOMMANDS_HPP

// Asking Archicad for its model, as a verb of its own.
//
// ⚠️ SPLIT OUT OF `ViewerInjectionCommands` ALONG THE LIFECYCLE SEAM THIS WHOLE
// STAGE IS ABOUT. Host extraction and overlay synchronisation must not share a
// lifecycle: run fifty-four forced an extraction by opening the Diligent
// overlay, which changes the camera sync mode, whose teardown calls
// `census::Shutdown` and drops the camera fingerprint that had just been
// selected. The fix was a verb that starts the extraction worker directly and
// touches no camera state -- and a verb that exists to be independent of the
// injection verbs does not belong in their file.
//
// The size cap was the messenger; this is the reason.

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

NativeCommandRegistrations GetHostGeometryCommandRegistrations ();

} // namespace geomsrv

#endif
