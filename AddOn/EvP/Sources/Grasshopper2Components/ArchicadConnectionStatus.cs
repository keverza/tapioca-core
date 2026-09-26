using Grasshopper2.Components;
using Grasshopper2.Parameters;
using Grasshopper2.UI;
using GrasshopperIO;

namespace TapiocaGH2;

[IoId("c512fd81-a693-47c4-95bf-30e3fa60b411")]
public sealed class ArchicadConnectionStatus : Component
{
    private int refreshHeld;

    public ArchicadConnectionStatus()
        : base(new Nomen("Archicad Connection Status",
            "Inspect the local Tapioca GH2 bridge. Use the Rhino command TapiocaGh2Attach to connect.",
            "Tapioca", "Archicad"))
    { ArchicadPresetBindings.Register(this); }

    public ArchicadConnectionStatus(IReader reader) : base(reader)
    { ArchicadPresetBindings.Register(this); }

    protected override void AddInputs(InputAdder inputs)
    {
        // A defaulted input creates one GH2 iteration even with no wires.
        // This does not trigger a connection or any Archicad read.
        inputs.AddBoolean("Refresh", "R", "Toggle on to check Archicad project choices. Toggle off before another check.").Set(false);
        inputs.AddText("Description", "D", "Definition description for the future Tapioca GH2 Player panel.").Set("");
    }

    protected override void AddOutputs(OutputAdder outputs)
    {
        outputs.AddText("Connection", "Info", "Local connection and project snapshot diagnostics.", Access.Twig);
    }

    protected override void Process(IDataAccess access)
    {
        access.GetItem(0, out bool refresh);
        access.GetItem(1, out string description);
        // A true value held through later solutions requests only once. Toggle
        // false then true to ask for another comparison with Archicad.
        if (refresh)
        {
            if (Interlocked.Exchange(ref refreshHeld, 1) == 0)
                Gh2ConnectionStatus.RequestRefresh();
        }
        else
            Interlocked.Exchange(ref refreshHeld, 0);
        Gh2ConnectionStatus.Snapshot state = Gh2ConnectionStatus.Read();
        ArchicadProjectOptions.Status options = ArchicadProjectOptions.ReadStatus();
        string freshness = options.Checking ? "Checking project choices" : options.Stale
            ? "Stale: " + options.Issue
            : $"Checked {options.CheckedAt:O}; revision {options.Revision} (model changes sampled every 5 seconds)";
        access.SetTwig(0, new[]
        {
            "Status: " + state.Phase,
            "Health: " + state.Health,
            "Endpoint: " + state.Endpoint,
            "Archicad: " + state.Archicad,
            "Local IP addresses: " + state.LocalAddresses,
            "Tapioca GH2 version: " + (typeof(ArchicadConnectionStatus).Assembly.GetName().Version?.ToString() ?? "unknown"),
            "Snapshot: " + freshness,
            "Change monitor: " + (state.Phase != "Connected" ? "Not connected" : options.Monitoring
                ? "5-second selection-aware stamp checks; one stale alert until refresh"
                : "Change hint unavailable; install the matched Archicad add-on"),
            "Last refreshed lists: " + options.LastChanged,
            "Description: " + description
        }, null, null);
        if (options.Stale)
            access.AddWarning("Project snapshot stale", options.Issue);
    }
}
