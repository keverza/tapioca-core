using Grasshopper2.Components;
using Grasshopper2.UI;
using GrasshopperIO;

namespace TapiocaGH2;

[IoId("c512fd81-a693-47c4-95bf-30e3fa60b411")]
public sealed class ArchicadConnectionStatus : Component
{
    public ArchicadConnectionStatus()
        : base(new Nomen("Archicad Connection Status",
            "Inspect the local Tapioca GH2 bridge. Use the Rhino command TapiocaGh2Attach to connect.",
            "Tapioca", "Archicad"))
    {
    }

    public ArchicadConnectionStatus(IReader reader) : base(reader)
    {
    }

    protected override void AddInputs(InputAdder inputs)
    {
        // A defaulted input creates one GH2 iteration even with no wires.
        // This does not trigger a connection or any Archicad read.
        inputs.AddBoolean("Refresh", "R", "Solve again to inspect current local connection state.").Set(false);
    }

    protected override void AddOutputs(OutputAdder outputs)
    {
        outputs.AddText("Status", "S", "Detached, connecting, connected, or disconnected.");
        outputs.AddText("Health", "H", "Heartbeat/write health at this solve.");
        outputs.AddText("Endpoint", "E", "Local named pipe, not a network IP endpoint.");
        outputs.AddText("Archicad", "A", "Last validated project Ping for this active connection.");
        outputs.AddText("Local IP addresses", "IP", "This machine's active IPv4 and IPv6 interfaces; not remote Archicad targets.");
        outputs.AddText("Tapioca GH2 version", "V", "Plugin version; restart Rhino to load an updated .rhp.");
    }

    protected override void Process(IDataAccess access)
    {
        Gh2ConnectionStatus.Snapshot state = Gh2ConnectionStatus.Read();
        access.SetItem(0, state.Phase);
        access.SetItem(1, state.Health);
        access.SetItem(2, state.Endpoint);
        access.SetItem(3, state.Archicad);
        access.SetItem(4, state.LocalAddresses);
        access.SetItem(5, typeof(ArchicadConnectionStatus).Assembly.GetName().Version?.ToString() ?? "unknown");
    }
}
