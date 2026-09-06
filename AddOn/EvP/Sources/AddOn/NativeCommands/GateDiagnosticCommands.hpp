#ifndef EVP_NATIVECOMMANDS_GATEDIAGNOSTICCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_GATEDIAGNOSTICCOMMANDS_HPP

// Tapioca.MainThreadGateState - why the main-thread gate is or is not
// dispatching, and who is holding Archicad's UI thread.
//
// ⚠️ A DIAGNOSTIC, NOT A PROBE, in this repository's sense of both words:
// it is a permanent supported inspection tool for a recurring class of failure,
// not a temporary empirical question with a closure date. See CLEANUP-
// GOVERNANCE-PLAN.md section 7.
//
// It exists because "the main-thread gate stopped dispatching" is a true
// sentence that names neither the cause nor its duration. Three live capture
// failures were spent inferring it; this makes the fourth a reading.

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

NativeCommandRegistrations GetGateDiagnosticCommandRegistrations ();

} // namespace geomsrv

#endif
