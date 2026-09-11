#ifndef EVP_GRASSHOPPER_GHWORKERPEERDETACH_HPP
#define EVP_GRASSHOPPER_GHWORKERPEERDETACH_HPP

// Asking an ATTACHED Grasshopper peer to let go, and waiting for it.
//
// WHY IT IS ITS OWN FILE: the same seam GhWorkerLocate.hpp describes.
// GhWorkerHost.cpp keeps a supervised process, a bridge, a lifecycle and a
// workflow controller behind one file-scoped set of statics on purpose, and it
// went back over its size cap when this handshake arrived. This function touches
// none of that state -- it talks to the bridge singleton and to the clock, and
// answers with a line for the caller to log.
//
// ⚠️ WHY THERE IS A HANDSHAKE AT ALL, AND WHY THE ORDER MATTERS MORE THAN THE
// MESSAGE. Closing the pipe on a peer works: it notices the disconnect and tears
// down. But it then tears down WHILE ARCHICAD IS ALREADY QUITTING, because
// Archicad quitting is the ordinary reason a peer is disconnected -- and its
// teardown disposes Grasshopper documents whose Archicad-facing components can
// call out over loopback. A call into an Archicad that is halfway through its own
// exit blocks until something times out, which is a user's Rhino frozen for the
// rest of the shutdown.
//
// So Archicad tells the peer FIRST, while it is still alive and still answering,
// and waits briefly for the pipe to drop. Shutdown to a peer means "we are done
// with you" -- TapiocaPeer.OnShutdownRequested disconnects and never closes
// Rhino, which is the asymmetry that makes reusing that message safe.
//
// ⚠️ AND THE WAIT HERE MUST BE THE LONGER OF THE TWO. The peer's teardown has a
// bounded wait of its own -- it closes Tapioca's sessions on Rhino's UI thread
// and waits for that, because the disposal is the part that can call back into
// Archicad. The first attempt gave Archicad the SHORTER wait, so it resumed
// exiting while the peer was still disposing and the freeze survived the fix
// entirely. See PeerDetachMs for the numbers and which one moved.
//
// ⚠️ THE WAIT EXPIRING IS NOT A FAILURE. A peer that has already gone, or one
// from an older build that does not answer, simply does not let go in time; the
// caller then closes the bridge, which is exactly what happened before this
// existed. Nothing here can refuse a shutdown.

#include "APIEnvir.h"
#include "ACAPinc.h"

namespace evp {
namespace grasshopper {

// Sends Shutdown to a connected peer and waits up to PeerDetachMs for it to drop
// the pipe. `note` always carries one line describing what happened, whether or
// not anything was sent. Returns true when the peer let go in time.
//
// Call with NO host lock held: this blocks for up to a second and a half.
bool RequestPeerDetach (GS::UniString& note);

} // namespace grasshopper
} // namespace evp

#endif
