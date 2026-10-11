using Tapioca.Gh2Worker;

namespace TapiocaGH2;

// An explicit display action capability, not a live-query capability for Process.
internal static class ArchicadOverlayBridge
{
    internal sealed record Lease(Gh2PipeClient Client, uint Generation, string Project)
    {
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
        await VerifyProjectAsync(lease);
        if (!alive()) throw new OperationCanceledException("Overlay solution changed or its component/document closed. Solve and Show again.");
        var outcome = ArchicadSelectionInput.ValidateReply(await lease.Client.EnableOverlayAsync(plan), "overlay start");
        if (outcome.GetProperty("refused").GetBoolean())
            throw new InvalidOperationException("Archicad refused the overlay: " + outcome.GetProperty("message").GetString());
        await VerifyProjectAsync(lease);
        if (!alive()) throw new OperationCanceledException("Overlay solution changed or its component/document closed. Solve and Show again.");
        ArchicadSelectionInput.ValidateReply(await lease.Client.SetOverlayAsync(payload), "overlay content");
        try
        {
            await VerifyProjectAsync(lease);
            if (!alive()) throw new OperationCanceledException("Overlay solution changed or its component/document closed. Solve and Show again.");
        }
        catch
        {
            // This exact old connection only; never clear a reconnected peer.
            try { await lease.Client.ClearOverlayAsync(lease.Generation, producer); }
            catch (Exception) { } // bridge teardown also removes this generation's layers
            throw;
        }
        return outcome.GetProperty("running").GetBoolean()
            ? "Layer published. Camera readiness and visibility are reported by Archicad's overlay HUD."
            : "Layer published; waiting for its Archicad view to come forward.";
    }

    internal static async Task ClearAsync(Lease lease, Guid producer)
    {
        if (!ReferenceEquals(lease.Client, Gh2ConnectionStatus.OverlayClient()))
            throw new InvalidOperationException("The peer detached; its overlay layers have been released.");
        ArchicadSelectionInput.ValidateReply(await lease.Client.ClearOverlayAsync(lease.Generation, producer), "overlay clear");
    }

    private static async Task VerifyProjectAsync(Lease lease)
    {
        if (!lease.Current || ArchicadProjectOptions.ParseIdentity(await lease.Client.ReadProjectInfoAsync()) != lease.Project)
            throw new InvalidOperationException("Archicad connection/project changed; solve again before showing geometry.");
    }
}
