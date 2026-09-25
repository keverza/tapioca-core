using System.Globalization;
using Tapioca.Gh2Worker;

namespace TapiocaGH2;

// The installed Rhino owns its process and documents. This peer owns only one
// bridge connection, and never passes it to a GH2 component or a solve.
internal static class Gh2Peer
{
    private const string PipeDirectory = @"\\.\pipe\";
    private static readonly string Prefix = "Tapioca.Gh2.v" + Gh2Wire.Version + ".";

    internal readonly record struct Candidate(string Name, uint Generation);

    internal static Candidate[] Discover()
    {
        var found = new List<Candidate>();
        foreach (string path in Directory.GetFiles(PipeDirectory))
        {
            string name = Path.GetFileName(path);
            if (!name.StartsWith(Prefix, StringComparison.Ordinal))
                continue;
            string[] parts = name[Prefix.Length..].Split('.');
            if (parts.Length != 2 ||
                !uint.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out uint pid) || pid == 0 ||
                !uint.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out uint generation) || generation == 0)
                continue;
            found.Add(new Candidate(name, generation));
        }
        return found.ToArray();
    }

    // Invoked only from a background task, including enumeration and the
    // blocking handshake/read. No Rhino UI thread waits on Archicad IO.
    internal static void AttachOnce(Action<string> report)
    {
        Gh2ConnectionStatus.Set("Searching");
        Candidate[] candidates = Discover();
        if (!TrySelect(candidates, out Candidate selected, out string refusal))
        {
            Gh2ConnectionStatus.Set("Disconnected", issue: refusal);
            report(refusal);
            return;
        }

        AttachSelected(selected, report);
    }

    internal static bool TrySelect(Candidate[] candidates, out Candidate selected, out string refusal)
    {
        selected = default;
        refusal = candidates.Length == 0
            ? "No Archicad GH2 bridge is waiting. Start a GH2 connection in Archicad first."
            : $"{candidates.Length} Archicad GH2 bridges are waiting ({string.Join(", ", candidates.Select(c => c.Name))}); refusing to choose one.";
        if (candidates.Length != 1)
            return false;
        selected = candidates[0];
        refusal = string.Empty;
        return true;
    }

    internal static void AttachSelected(Candidate selected, Action<string> report)
    {
        Gh2ConnectionStatus.Set("Connecting", selected.Name);
        Gh2PipeClient? bridge = null;
        string? issue = null;
        try
        {
            bridge = Gh2PipeClient.Connect(selected.Name);
            string project = Gh2Ping.ConfirmStartup(bridge, selected.Generation, "Attached Rhino 9 / GH2 ready");
            Gh2ConnectionStatus.Set("Connected", selected.Name, project, bridge);
            report($"GH2 attached to {selected.Name}. Waiting for Archicad to detach.");
            bridge.WaitForShutdown(acknowledgeShutdown: false);
            report("Archicad requested GH2 detach; Rhino remains open.");
        }
        catch (EndOfStreamException error)
        {
            if (bridge is not null)
                Gh2Ping.AcknowledgeStartupFailure(bridge, error);
            report("Archicad disconnected GH2; Rhino remains open.");
        }
        catch (IOException error) when (bridge?.StartupAcknowledged == true)
        {
            issue = error.Message;
            report("Archicad disconnected GH2: " + error.Message + ". Rhino remains open.");
        }
        catch (Exception error)
        {
            issue = error.Message;
            if (bridge is not null)
                Gh2Ping.AcknowledgeStartupFailure(bridge, error);
            throw;
        }
        finally
        {
            Gh2ConnectionStatus.Set("Disconnected", issue: issue);
            bridge?.Dispose();
        }
    }
}
