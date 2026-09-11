#include "APIEnvir.h"
#include "ACAPinc.h"

#include "GhWorkerPeerDetach.hpp"

#include "GhBridge.hpp"
#include "GhProtocol.hpp"

#include <windows.h>

namespace evp {
namespace grasshopper {

namespace {

// How long Archicad waits for an attached peer to let go of the pipe.
//
// ⚠️ IT MUST OUTLAST THE PEER'S OWN CEILING, AND THE FIRST ATTEMPT DID NOT --
// WHICH IS WHY THE FREEZE SURVIVED IT. The peer's teardown closes Tapioca's
// sessions on Rhino's UI thread and waits up to four seconds for that
// (TapiocaPeer.SessionCloseWaitMs), because disposing those documents is the
// part that can call back into Archicad. A one-and-a-half second wait here
// expired first: Archicad carried on exiting while the peer was still disposing,
// which is exactly the situation the handshake exists to prevent, so the Rhino
// froze anyway and the ordering bought nothing.
//
// Six seconds is the peer's ceiling plus slack. It is a CEILING, not a cost: the
// peer drops the pipe as soon as it is done, which is milliseconds in the
// ordinary case, and this loop notices within one poll. The cost is paid only
// when the peer's UI thread is genuinely stuck -- and then it is paid by the
// application that is quitting rather than by the one the user keeps working in,
// which is the right way round.
//
// Still bounded, because a peer that never answers must not hold Archicad's exit
// open forever.
constexpr DWORD PeerDetachMs = 6000;

constexpr DWORD PollMs = 25;

} // namespace

bool RequestPeerDetach (GS::UniString& note)
{
    GhBridge& bridge = GhBridge::Get ();
    if (!bridge.IsConnected ()) {
        note = "No peer was connected, so there was nothing to detach.";
        return true;
    }

    GS::UniString sendError;
    if (!bridge.Send (protocol::MessageType::Shutdown, sendError)) {
        note = GS::UniString ("Could not ask the attached peer to detach: ") + sendError;
        return false;
    }

    // Measured rather than assumed: how long a peer actually takes to let go is
    // the number that says whether this ceiling is right, and it is the only
    // place it can be observed.
    const DWORD started = GetTickCount ();
    const DWORD deadline = started + PeerDetachMs;
    while (bridge.IsConnected () && GetTickCount () < deadline)
        Sleep (PollMs);

    const DWORD elapsed = GetTickCount () - started;

    if (bridge.IsConnected ()) {
        note = GS::UniString::Printf ("The attached peer had not let go after %u ms; closing the bridge anyway. "
                                      "Whatever its teardown is waiting on outlasted this wait -- grasshopper.log "
                                      "on the Rhino side says which step it reached.",
                                      (unsigned) elapsed);
        return false;
    }

    note =
        GS::UniString::Printf ("The attached peer detached itself in %u ms; closing the bridge.", (unsigned) elapsed);
    return true;
}

} // namespace grasshopper
} // namespace evp
