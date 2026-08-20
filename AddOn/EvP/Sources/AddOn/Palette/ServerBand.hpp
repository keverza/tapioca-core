#ifndef GEOMETRYSERVER_PALETTE_SERVERBAND_HPP
#define GEOMETRYSERVER_PALETTE_SERVERBAND_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"

namespace geomsrv {
class HttpServer;
HttpServer& SharedHttpServer ();
} // namespace geomsrv

namespace evp {

// The server, and the two items that are its whole UI: the toggle button, and one
// text line above it carrying the address.
//
// PLAT-7 — this band used to be three things: a toggle button and two text rows that
// said what the toggle already implied. The second row is still gone. The first one
// is back, ABOVE the action row rather than below it, because the address briefly
// lived on a SECOND LINE INSIDE the button and that is not a thing DG offers:
//
// ⚠️ NEVER PUT A NEWLINE IN A DG::PushCheck's TEXT. `SetText` takes a plain
// GS::UniString (DGItemProperty.hpp) and nothing in DG documents multi-line button
// text. It was tried in f819f34 and the palette did not survive its own constructor
// — `ServerBand::Refresh` runs there, and the session died between it and the
// `Rescan` that would have written logs\scan.log. The address goes on its own item.
//
// It owns the HttpServer, because starting one is not a layout decision and the two
// were only ever together in the shell by history. What the shell keeps is both
// items' PLACE — the toggle shares its row with Rescan and Run, which are the
// shell's — so the .grc items are bound there and LENT here, the same arrangement as
// the command list. This object never places either of them.
//
// Stopping hands back everything the session was holding: an idle server should cost
// nothing, and "stop" is the only moment the user tells us they are done.
class ServerBand {
  public:
    ServerBand (DG::PushCheck& toggle, DG::LeftText& addressText);
    ~ServerBand ();

    bool IsRunning () const;
    unsigned short Port () const;

    // Start it for a reason other than the button — a runtime="external" command's
    // only route back to Archicad is the bus, so a run starts the server rather than
    // failing on it. Refreshes the button, since the state it shows just changed.
    void Start ();

    // The toggle was clicked. True when the event was ours; the shell routes every
    // check-item event through its sub-objects and takes the first that claims one.
    bool HandleCheckItemChanged (const DG::CheckItemChangeEvent& ev);

    // Place the address line at `top` and return the height it used. The TOGGLE is
    // not placed here: it shares the action row with Rescan and Run, which are the
    // shell's, so that row is laid out as one thing where it is written down.
    short PlaceAt (short top, short left, short right);

    // Put the button, the address line and the process-wide ServerState mirror back
    // in step with the server. Cheap, and safe to call whenever something might have
    // changed it.
    void Refresh ();

  private:
    // NOT owned: the .grc toggle and address line, bound by the shell and lent here.
    DG::PushCheck& toggle;
    DG::LeftText& addressText;
    geomsrv::HttpServer& server;
};

} // namespace evp

#endif
