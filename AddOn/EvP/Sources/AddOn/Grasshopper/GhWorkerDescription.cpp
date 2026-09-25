#include "GhWorkerDescription.hpp"

#include "GhBridge.hpp"
#include "GhLog.hpp"
#include "GhProtocol.hpp"

namespace evp {
namespace grasshopper {

GS::UniString DescribeWorker (HostState state, uint32_t generation, PeerOwnership ownership, bool gh2,
                              uint32_t archicadPort, const GS::UniString& workerPath, const GS::UniString& lastMessage,
                              const std::string& failure, const GhBridge& bridge)
{
    GS::UniString text = GS::UniString::Printf ("Grasshopper worker: %s", DescribeHostState (state));
    text += gh2 ? "\nEngine: Rhino 9 / GH2" : "\nEngine: Rhino 8 / GH1";
    text += GS::UniString::Printf ("\nRestart generation: %u", (unsigned int) generation);
    text += GS::UniString ("\nPeer: ") + GS::UniString (DescribePeerOwnership (ownership));

    const uint32_t pid = bridge.WorkerProcessId ();
    if (pid != 0)
        text += GS::UniString::Printf ("\nWorker process: %u", (unsigned int) pid);
    else
        text += GS::UniString ("\nWorker process: none");

    if (!workerPath.IsEmpty ())
        text += GS::UniString ("\nWorker executable: ") + workerPath;

    const GS::UniString pipeName = bridge.PipeName ();
    if (!pipeName.IsEmpty ())
        text += GS::UniString ("\nBridge: \\\\.\\pipe\\") + pipeName +
                (bridge.IsConnected () ? GS::UniString (" (connected)") : GS::UniString (" (waiting for the worker)"));
    else
        text += GS::UniString ("\nBridge: not listening");

    if (bridge.IsConnected ())
        text +=
            GS::UniString::Printf ("\nLast heartbeat: %u ms ago", (unsigned int) bridge.MillisecondsSinceHeartbeat ());

    // Only GH1 has the separate Tapir JSON connection. GH2 reads the controlled
    // pre-solve snapshot over this bridge instead of advertising a port.
    if (!gh2 && archicadPort != 0)
        text += GS::UniString::Printf ("\nArchicad JSON port (Tapir ConnectArchicad): %u", (unsigned int) archicadPort);
    else if (!gh2 && state == HostState::Running)
        text += GS::UniString ("\nArchicad JSON port: unavailable - a Tapir ConnectArchicad component will "
                               "need one entered by hand");

    if (!failure.empty ())
        text += GS::UniString ("\nLast failure: ") + GS::UniString (failure.c_str ());
    if (!lastMessage.IsEmpty ())
        text += GS::UniString ("\nLast message: ") + lastMessage;
    const GS::UniString workerMessage = bridge.LastWorkerMessage ();
    if (!workerMessage.IsEmpty ())
        text += GS::UniString ("\nLast worker message: ") + workerMessage;

    const GS::UniString logPath = LogPath ();
    if (!logPath.IsEmpty ())
        text += GS::UniString ("\nLog: ") + logPath;
    return text;
}

GS::UniString DescribeRunResult (const protocol::RunReportPayload& report)
{
    GS::UniString text (protocol::DescribeRunReport (report).c_str (), CC_UTF8);
    // This warning belongs to the report every time: GH1's external components
    // can already have written directly to Archicad before the result arrives.
    text += GS::UniString ("\n\nNote: Tapir components reach Archicad on their own connection, which Tapioca "
                           "does not gate. Anything this definition wrote to the project is already written.");
    return text;
}

} // namespace grasshopper
} // namespace evp
