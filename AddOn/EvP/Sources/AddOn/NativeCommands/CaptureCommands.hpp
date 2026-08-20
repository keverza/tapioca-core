#ifndef EVP_NATIVECOMMANDS_CAPTURECOMMANDS_HPP
#define EVP_NATIVECOMMANDS_CAPTURECOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Screenshots and 3D projection/window state:
// CaptureScreenshot, Get3DProjection, Get3DWindowSets, Set3DProjection, ModelToScreen.
NativeCommandRegistrations GetCaptureCommandRegistrations ();

// Register this domain's READ commands on Archicad's JSON port — see
// SnapshotCommands.hpp for why installation is per-domain.
GSErrCode InstallCaptureJsonCommands ();

} // namespace geomsrv

#endif
