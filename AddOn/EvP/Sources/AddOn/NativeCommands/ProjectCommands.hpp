#ifndef EVP_NATIVECOMMANDS_PROJECTCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_PROJECTCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Reads about the PROJECT rather than about elements: GetStories, GetProjectInfo.
//
// Split out of ElementReadCommands when the mesh read kind pushed that file past
// its size ceiling — and it was the right seam anyway: neither of these takes a
// guid. Stories and project-info fields are the two things a command needs to
// know about the document it is running inside.
//
// This .cpp ALSO implements the facade's geomsrv::ProjectInfoField
// (AddOnCommands.hpp) — its only helper, ReadProjectInfoFields, is file-local
// here and is the same field scan GetProjectInfo exposes.
//
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetProjectCommandRegistrations ();

} // namespace geomsrv

#endif
