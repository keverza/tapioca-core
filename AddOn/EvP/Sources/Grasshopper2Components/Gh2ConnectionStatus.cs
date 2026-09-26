using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using Tapioca.Gh2Worker;

namespace TapiocaGH2;

// Only the standalone peer writes this state. A GH2 component reads a copy;
// it never performs an Archicad request during graph evaluation.
internal static class Gh2ConnectionStatus
{
    private static readonly object Sync = new();
    private static string phase = "Disconnected";
    private static string endpoint = "(none)";
    private static string archicad = "(not connected)";
    private static string? lastIssue;
    private static Gh2PipeClient? client;
    internal static event Action? Updated;

    internal readonly record struct Snapshot(string Phase, string Health, string Endpoint,
        string Archicad, string LocalAddresses);

    internal static Snapshot Read()
    {
        string currentPhase;
        string currentEndpoint;
        string currentArchicad;
        string health;
        lock (Sync)
        {
            currentPhase = phase;
            currentEndpoint = endpoint;
            currentArchicad = archicad;
            health = client is null ? lastIssue ?? "No bridge connection" : client.HeartbeatHealth;
        }
        return new Snapshot(currentPhase, health, currentEndpoint, currentArchicad, LocalAddresses());
    }

    internal static bool RequestRefresh()
    {
        Gh2PipeClient? active;
        lock (Sync)
            active = client;
        return active?.RequestRefresh() ?? false;
    }

    internal static Task<string> ReadSelectionAsync()
    {
        lock (Sync)
            return client?.ReadSelectionAsync() ?? Task.FromException<string>(
                new InvalidOperationException("Attach to Archicad before capturing its selection."));
    }

    internal static Task<string> ReadProjectInfoAsync()
    {
        lock (Sync)
            return client?.ReadProjectInfoAsync() ?? Task.FromException<string>(
                new InvalidOperationException("Attach to Archicad before reading its project."));
    }

    internal static Task<string> ReplaceSelectionAsync(IReadOnlyList<Guid> ids)
    {
        lock (Sync)
            return client?.ReplaceSelectionAsync(ids) ?? Task.FromException<string>(
                new InvalidOperationException("Attach to Archicad before reselecting elements."));
    }

    internal static void Set(string state, string pipeName = "(none)",
        string project = "(not connected)", Gh2PipeClient? bridge = null, string? issue = null)
    {
        lock (Sync)
        {
            phase = state;
            endpoint = pipeName;
            archicad = project;
            client = bridge;
            lastIssue = issue;
        }
        Updated?.Invoke();
    }

    private static string LocalAddresses()
    {
        try
        {
            var addresses = new SortedSet<string>(StringComparer.OrdinalIgnoreCase)
            {
                IPAddress.Loopback.ToString(),
                IPAddress.IPv6Loopback.ToString()
            };
            foreach (NetworkInterface nic in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (nic.OperationalStatus != OperationalStatus.Up)
                    continue;
                foreach (UnicastIPAddressInformation entry in nic.GetIPProperties().UnicastAddresses)
                {
                    IPAddress address = entry.Address;
                    if (address.AddressFamily is AddressFamily.InterNetwork or AddressFamily.InterNetworkV6)
                        addresses.Add(address.ToString());
                }
            }
            return string.Join(", ", addresses);
        }
        catch (NetworkInformationException)
        {
            return "Local IP enumeration unavailable";
        }
    }
}
