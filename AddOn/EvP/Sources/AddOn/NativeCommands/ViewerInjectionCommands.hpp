#ifndef EVP_NATIVECOMMANDS_VIEWERINJECTIONCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_VIEWERINJECTIONCOMMANDS_HPP

// The stage 5 and 6 injection verbs: the camera census, the projection oracle
// and the injected primitive itself.
//
// ⚠️ SPLIT OUT OF `ViewerGpuStateCommands` ALONG A REAL SEAM. That file
// is stage 1 to 3: the build pin, the hooked slots, the constant-buffer
// classifier, the device inventory -- the verbs that ask WHETHER Archicad's GPU
// state can be read at all, and which are now frozen discovery tools. These
// three are stage 5 and 6: they choose a camera, draw with it, and test it
// against Archicad's depth buffer. Two questions, two files, and the size cap
// was the messenger rather than the reason.

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

NativeCommandRegistrations GetViewerInjectionCommandRegistrations ();

}   // namespace geomsrv

#endif
