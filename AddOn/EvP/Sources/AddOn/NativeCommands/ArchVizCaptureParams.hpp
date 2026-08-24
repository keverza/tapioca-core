#ifndef EVP_NATIVECOMMANDS_ARCHVIZCAPTUREPARAMS_HPP
#define EVP_NATIVECOMMANDS_ARCHVIZCAPTUREPARAMS_HPP

// NativeCommands/ArchVizCaptureParams — the ObjectState -> struct readers for
// Tapioca.StartDiligentCapture.
//
// A separate translation unit because `ArchVizCommands.cpp` reached the
// ~1,000-line cap, and this is the seam that was already there rather than a
// place to cut. That file is a REGISTRY: a list of small command classes and the
// JSON schemas they answer to. These two functions are parameter DECODING, which
// is the one part of it that has real branching in it and the one part worth
// reading on its own.
//
// ⚠️ EVERY FIELD IS OPTIONAL AND EVERY DEFAULT IS "WHAT THE CALLER GOT BEFORE".
// A capture command that started failing because a new parameter went unread
// would break MassingFeasibility, which is the caller this whole path exists
// for. The schema in ArchVizCommands.cpp is what REJECTS a misspelling; these
// functions never do.

#include "ArchViz/DiligentViewport.hpp"

namespace GS {
class ObjectState;
}

namespace geomsrv {

// The camera a headless capture renders from. ⚠️ NOT VALIDATED HERE —
// DiligentViewport::StartCapture owns the "is this camera usable" rules and
// reports them as failures the caller can read.
archviz::CameraStart ReadCaptureCamera (const GS::ObjectState& params);

// What the capture draws BESIDES the model: currently the storey section
// overlay. Defaults are all off.
archviz::DiligentViewport::CaptureOverlays ReadCaptureOverlays (const GS::ObjectState& params);

} // namespace geomsrv

#endif // EVP_NATIVECOMMANDS_ARCHVIZCAPTUREPARAMS_HPP
