#ifndef EVP_NATIVECOMMANDS_ARCHVIZCAPTURECOMMANDS_HPP
#define EVP_NATIVECOMMANDS_ARCHVIZCAPTURECOMMANDS_HPP

// The headless capture verbs: start one, start a batch, poll it, cancel it.
//
// ⚠️ SPLIT OUT OF ArchVizCommands.cpp BECAUSE THAT FILE REACHED THE
// ~1,000-LINE CAP - the same seam, and the same reason, as
// ArchVizCaptureParams.cpp before it. The capture verbs are the natural cut:
// they are the only ones there that own a multi-frame lifecycle rather than
// setting one value on the viewport.

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

NativeCommandRegistrations GetArchVizCaptureCommandRegistrations ();

} // namespace geomsrv

#endif
