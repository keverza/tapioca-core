#ifndef EVP_NATIVECOMMANDS_TESTCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_TESTCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {
NativeCommandRegistrations GetTestCommandRegistrations ();
GSErrCode InstallTestJsonCommands ();
// Main-thread lifecycle hooks. Reset never joins; shutdown joins only pure copied-data work.
void ResetTestJobs ();
void ShutdownTestJobs ();
} // namespace geomsrv

#endif
