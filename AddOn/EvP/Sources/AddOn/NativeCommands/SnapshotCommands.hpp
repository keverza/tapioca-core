#ifndef EVP_NATIVECOMMANDS_SNAPSHOTCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_SNAPSHOTCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Snapshot lifecycle + status: BuildSnapshot, ReleaseSnapshot, GetSnapshotInfo,
// GetStatus.
NativeCommandRegistrations GetSnapshotCommandRegistrations ();

// Register this domain's READ commands on Archicad's JSON port.
//
// Why a per-domain function rather than the registry doing it: installation needs
// GS::NewOwned<ConcreteCommand>(), and the concrete classes are private to the
// domain .cpp (anonymous namespace) — which is the point of the split. The
// alternative, adapting a registration maker's unique_ptr into a GS::Owner, would mean
// inventing a conversion against an ownership contract the DevKit does not
// document. So each domain installs its own.
//
// The writes-never-on-the-JSON-port policy is enforced at the CALL SITE in
// CommandRegistry.cpp, where the comment explaining it lives.
GSErrCode InstallSnapshotJsonCommands ();

} // namespace geomsrv

#endif
