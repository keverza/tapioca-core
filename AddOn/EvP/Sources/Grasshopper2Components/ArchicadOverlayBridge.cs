using Tapioca.Gh2Worker;
using System.Text.Json;

namespace TapiocaGH2;

// An explicit display action capability, not a live-query capability for Process.
internal static class ArchicadOverlayBridge
{
    internal sealed record Lease(Gh2PipeClient Client, uint Generation, string Project)
    {
        internal long Epoch { get { using var json = JsonDocument.Parse(Project); return json.RootElement.GetProperty("Epoch").GetInt64(); } }
        internal bool Current => ReferenceEquals(Client, Gh2ConnectionStatus.OverlayClient()) &&
            Project == ArchicadProjectOptions.CurrentIdentity();
    }

    internal static Lease? Capture()
    {
        var (client, endpoint) = Gh2ConnectionStatus.OverlayConnection();
        string? project = ArchicadProjectOptions.CurrentIdentity();
        if (client is null || project is null || !uint.TryParse(endpoint.Split('.').Last(), out uint generation) || generation == 0)
            return null;
        var lease = new Lease(client, generation, project);
        return lease.Current ? lease : null;
    }

    internal static async Task<string> ShowAsync(Lease lease, string payload, Guid producer, bool plan, Func<bool> alive)
    {
        if (!lease.Current) throw new InvalidOperationException("Archicad project changed; solve again.");
        if (!alive()) throw new OperationCanceledException("Overlay solution changed or its component/document closed. Solve and Show again.");
        ArchicadSelectionInput.ValidateReply(await lease.Client.SetOverlayAsync(payload, () => lease.Current && alive()), "overlay content");
        return "Layer published. Camera readiness and visibility are reported by Archicad's overlay HUD.";
    }

    internal static async Task ClearAsync(Lease lease, Guid producer, Guid document, long revision)
    {
        if (!ReferenceEquals(lease.Client, Gh2ConnectionStatus.OverlayClient()))
            throw new InvalidOperationException("The peer detached; its overlay layers have been released.");
        ArchicadSelectionInput.ValidateReply(await lease.Client.ClearOverlayAsync(lease.Generation, producer, lease.Epoch, document, revision), "overlay clear");
    }

}
