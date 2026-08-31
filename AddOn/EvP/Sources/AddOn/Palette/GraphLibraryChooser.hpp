#ifndef EVP_PALETTE_GRAPHLIBRARYCHOOSER_HPP
#define EVP_PALETTE_GRAPHLIBRARYCHOOSER_HPP

// The workflow chooser: the ordinary Windows Save/Open dialog, confined to the
// workflow library.
//
// ⚠️ THIS IS NOT A NATIVE COMMAND, AND THAT IS DELIBERATE. A file dialog is
// MODAL: it does not return until the user acts. MainThreadGate.hpp forbids
// Invoke-ing such a job, because the gate then reports a bogus timeout while
// the user is merely still reading the dialog - so a Tapioca.* verb that opened
// one would be a foot-gun for every caller that is NOT already on the main
// thread (Python's workers, the Grasshopper bridge). Registering it would put
// that hazard in the API surface for one client's benefit.
//
// A chooser is UI, not runtime. It runs where the palette already is - on the
// main thread, inside the browser bridge's own callback - and it returns a
// NAME. GraphStore stays name-addressed and sandboxed; nothing here can widen
// what the runtime is allowed to read or write.
//
// The confinement is enforced here rather than trusted to the dialog: the user
// can always navigate out of the starting folder, so a selection whose parent
// is not the library root is REFUSED with an explanation rather than saved
// somewhere the library will never list.

#include "UniString.hpp"

namespace evp {

// Runs the modal chooser and returns the same `EvP.call` envelope shape every
// other bridge answer uses. MUST be called on Archicad's main thread.
//
// params: {"mode":"save"|"load","name":"<suggestion>"}
// data:   {"ok","status","error","name","location"}
//         status is one of ok | cancelled | outsideLibrary | invalid | noLocation.
GS::UniString RunGraphLibraryChooser (const GS::UniString& paramsJson);

// The verb the browser bridges intercept before handing off to DispatchApiCall.
extern const char* const kGraphLibraryBrowseCommand;

} // namespace evp

#endif
