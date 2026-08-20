#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Palette/ServerBand.hpp"
#include "Python/PathUtils.hpp" // StartupTrace — ACAPI-free, safe this early
#include "Server/HttpServer.hpp"
#include "Server/ServerState.hpp"

namespace evp {

namespace {

// U+25B6 BLACK RIGHT-POINTING TRIANGLE / U+25A0 BLACK SQUARE, as UTF-8. Built at
// runtime as UniString: the .grc is compiled through a 1252 codepage, which would
// mangle these, so they must not live in the resource.
GS::UniString PlayLabel ()
{
    return GS::UniString ("\xE2\x96\xB6  Start server", CC_UTF8);
}
GS::UniString StopLabel ()
{
    return GS::UniString ("\xE2\x96\xA0  Stop server", CC_UTF8);
}

// The address line that sits ABOVE the action row. Stopped, it still says something
// rather than emptying: a row that appears and disappears moves everything under it,
// and this one sits above the buttons, so it would move the whole panel.
//
// ⚠️ This text is a separate DG item ON PURPOSE. It was briefly a second line inside
// the toggle's own label; see the warning in ServerBand.hpp — a newline in a
// DG::PushCheck is not something DG supports, and it took the palette down with it.
GS::UniString AddressLine (bool running, unsigned short port)
{
    if (!running)
        return GS::UniString ("Server stopped.");
    return GS::UniString::Printf ("http://127.0.0.1:%d", (int) port);
}

} // namespace

ServerBand::ServerBand (DG::PushCheck& toggle, DG::LeftText& addressText)
    : toggle (toggle), addressText (addressText), server (geomsrv::SharedHttpServer ())
{
}

ServerBand::~ServerBand ()
{
}

bool ServerBand::IsRunning () const
{
    return server.IsRunning ();
}

unsigned short ServerBand::Port () const
{
    return IsRunning () ? (unsigned short) server.Port () : 0;
}

void ServerBand::Start ()
{
    server.Start ();
    Refresh ();
}

bool ServerBand::HandleCheckItemChanged (const DG::CheckItemChangeEvent& ev)
{
    if (ev.GetSource () != &toggle)
        return false;

    if (toggle.IsChecked ()) {
        server.Start ();
    }
    else {
        // Stopping also hands back everything we were holding — "go back to sleep".
        server.Stop ();
    }
    Refresh ();
    return true;
}

short ServerBand::PlaceAt (short top, short left, short right)
{
    addressText.SetRect (DG::Rect (left, top, right, (short) (top + 16)));
    return 20;
}

void ServerBand::Refresh ()
{
    StartupTrace ("ServerBand::Refresh enter");
    const bool running = IsRunning ();

    // PushCheck draws itself pressed/darker while checked -> "running" is obvious.
    if (running != toggle.IsChecked ()) {
        if (running)
            toggle.Check ();
        else
            toggle.Uncheck ();
    }
    StartupTrace ("ServerBand::Refresh setting toggle text");
    toggle.SetText (running ? StopLabel () : PlayLabel ());
    StartupTrace ("ServerBand::Refresh setting address text");
    addressText.SetText (AddressLine (running, Port ()));

    // The process-wide mirror the bus and the commands read. It is not this band's
    // state — it is the server's, said where a worker thread can see it without
    // touching DG.
    geomsrv::ServerState& st = geomsrv::ServerState::Get ();
    st.serverRunning.store (running);
    st.port.store (running ? (int) Port () : 0);
    StartupTrace ("ServerBand::Refresh done");
}

} // namespace evp
