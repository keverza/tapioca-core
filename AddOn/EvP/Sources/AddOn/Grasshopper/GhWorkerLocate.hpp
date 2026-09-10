#ifndef EVP_GRASSHOPPER_GHWORKERLOCATE_HPP
#define EVP_GRASSHOPPER_GHWORKERLOCATE_HPP

// Where the Grasshopper worker's executable is, how long it may go quiet, and
// the two string conversions the host does everywhere.
//
// WHY IT IS ITS OWN FILE: GhWorkerHost.cpp holds a supervised process, a
// bridge, a lifecycle and a workflow controller behind one file-scoped set of
// statics -- deliberately, so nothing else can reach them -- and it went over
// its size cap when the attach mode arrived. Everything in HERE touches none of
// that state: it reads the environment, the module's own path and the file
// system, and answers. That is the seam, and it is the only one in that file
// that does not require exposing the state the file exists to hide.
//
// ⚠️ NOT THE ARCHICAD PORT, WHICH LOOKS LIKE IT BELONGS AND DOES NOT.
// ArchicadJsonPort logs its failure through the host's own generation-stamped
// logger, so moving it here would either drag that state along or make it log
// somewhere else.
//
// Win32 and the DevKit are both in play, so nothing here is offline-testable;
// it is extracted for size and clarity, not for coverage.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include <cstdint>
#include <string>

namespace evp {
namespace grasshopper {
namespace locate {

GS::UniString FromWide (const std::wstring& text);
GS::UniString FromUtf8Std (const std::string& text);

// TAPIOCA_GH_WORKER_DIR first, so a developer can point Archicad at a worker
// built somewhere else without reinstalling the add-on; then the staged folder
// beside the .apx, which is what a shipped installation has.
bool ResolveWorker (std::wstring& executable, std::wstring& workingDirectory);

// How long a connected worker may go without a heartbeat before the supervisor
// treats it as wedged. TAPIOCA_GH_HEARTBEAT_MS overrides it; a nonsense value is
// IGNORED rather than believed, because this deadline is the only thing that
// notices a worker stuck inside a component.
uint64_t HeartbeatDeadlineMs ();

} // namespace locate
} // namespace grasshopper
} // namespace evp

#endif
