#ifndef EVP_NATIVECOMMANDS_NOTIFYCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_NOTIFYCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// E25 — "has the model changed since I last looked": EvP.WatchModel arms the
// element observer, EvP.GetChangeToken reads the counter it bumps.
//
// Its own domain rather than a corner of ProjectCommands because the two halves
// sit on opposite sides of the gate — arming is main-thread ACAPI, polling must
// be gate-free so a viewer can ask every 300 ms — and because the state they
// share (Notify/ChangeTracker) is written from ARCHICAD'S OWN THREAD inside an
// edit. That is a boundary worth keeping visible in the file list.
//
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetNotifyCommandRegistrations ();

} // namespace geomsrv

#endif
