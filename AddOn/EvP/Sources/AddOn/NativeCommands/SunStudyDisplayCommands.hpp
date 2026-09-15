#ifndef EVP_NATIVECOMMANDS_SUNSTUDYDISPLAYCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYDISPLAYCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Tapioca.ShowSunStudy - the one sun study verb that talks to the RENDERER.
//
// EXTRACTED OUT OF SunStudyCommands.cpp (2026-09-15) for the two reasons its
// neighbours in this directory were extracted for. It pushed that file past the
// size cap; and it is the only verb in the domain that must name ArchViz,
// because a study's atlas and its per-element side buffers are GEOMETRY, and
// geometry has always reached the renderer through SceneCmdQueue rather than
// through a command's return value.
//
// A DOMAIN OF ITS OWN RATHER THAN A SECOND FILE OF THE SAME ONE, because that is
// what check_cpp.py's REGISTRY rule requires of any NativeCommands/*Commands.cpp
// and what ArchVizCaptureCommands.cpp did when it was extracted for the same
// reason. The practical effect is the one that matters: exactly one translation
// unit in the sun study's surface crosses into the renderer, and the five
// analysis verbs beside it stay renderer-free.
NativeCommandRegistrations GetSunStudyDisplayCommandRegistrations ();

} // namespace geomsrv

#endif
