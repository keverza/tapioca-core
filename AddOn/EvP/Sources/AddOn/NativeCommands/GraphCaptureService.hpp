#ifndef EVP_NATIVECOMMANDS_GRAPHCAPTURESERVICE_HPP
#define EVP_NATIVECOMMANDS_GRAPHCAPTURESERVICE_HPP

// The renderer half of the graph's capture seam.
//
// ⚠️ IT LIVES ON THIS SIDE OF THE BOUNDARY BECAUSE IT MUST. The interface it
// implements is NodeGraph/CaptureService.hpp, which is services-tier and
// therefore cannot name DiligentViewport at all; this file is features-tier and
// can. Exactly one translation unit knows both halves, which is the same
// arrangement ArchicadHostImpl.cpp has for ACAPI.
//
// Installed by add-on startup, like the Archicad host, and detached on teardown.

#include "NodeGraph/CaptureService.hpp"

namespace geomsrv {

evp::nodegraph::ICaptureService& GraphCaptureService ();

} // namespace geomsrv

#endif
