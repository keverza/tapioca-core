#ifndef EVP_NATIVECOMMANDS_PLANGEOMETRYCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_PLANGEOMETRYCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Archicad's OWN 2D plan geometry, read per element.
//
// ⚠️ THIS IS A DIFFERENT DOMAIN FROM THE 3D MODEL READS, AND THE DISTINCTION IS
// THE WHOLE POINT (PLAT-RE65). A floor plan is not a top view of the model —
// it is a separate representation Archicad maintains, and a wall's plan outline
// is its CONNECTION polygon, trimmed where it meets other walls. Deriving these
// from the mesh would be the rejected "stale 3D snapshot on the plan" approach
// in miniature, so nothing in this domain may reach for 3D geometry.
//
// Its consumer is the plan overlay's ANCHOR layer: outlines drawn purely so the
// analysis on top of them can be checked against something Archicad already
// drew.
//
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetPlanGeometryCommandRegistrations ();

} // namespace geomsrv

#endif
