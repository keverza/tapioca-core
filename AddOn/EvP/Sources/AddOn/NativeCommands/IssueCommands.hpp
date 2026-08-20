#ifndef EVP_NATIVECOMMANDS_ISSUECOMMANDS_HPP
#define EVP_NATIVECOMMANDS_ISSUECOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// E8 — issues (Archicad "Mark-Ups"): CreateIssue, AttachElementsToIssue.
// Both are writes; the caller supplies the undo scope.
// Returns the domain's ordered command registrations.
NativeCommandRegistrations GetIssueCommandRegistrations ();

} // namespace geomsrv

#endif
