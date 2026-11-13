using System.Text.Json;
using Eto.Forms;
using Grasshopper2.Doc;
using Tapioca.Gh2Worker;

namespace TapiocaGH2;

internal interface IArchicadChangeConsumer
{
    bool IsActive { get; }
    Document? InvalidateArchicadInputs();
}

// Host wakes, independent scoped cursors, one active drain plus one newest wake.
// No component timer polls Archicad; weak owners do not retain closed definitions.
internal static class ArchicadChangeDelivery
{
    private sealed class Scope(IArchicadChangeConsumer owner, string key, Guid[] ids, int fields)
    {
        internal readonly WeakReference<IArchicadChangeConsumer> Owner = new(owner);
        internal readonly string Key = key;
        internal readonly Guid[] Ids = ids;
        internal readonly int Fields = fields;
        internal long Revision;
        internal bool Initialized;
    }
    private static readonly object sync = new();
    private static readonly List<Scope> scopes = [];
    private static Gh2PipeClient? client;
    private static long epoch, generation;
    private static bool pending, running;
    private static readonly HashSet<IArchicadChangeConsumer> uiPending = [];
    private static bool uiPosted;
    private static string status = "Automatic input refresh awaits a matched native connection.";
    internal static string Status => Volatile.Read(ref status);
    private static void ScheduleOnUi(Action action) =>
        (Application.Instance ?? throw new InvalidOperationException("GH2 UI scheduler is unavailable.")).AsyncInvoke(action);
    private static Action<Action> dispatch = ScheduleOnUi;

    internal static void Attach(Gh2PipeClient bridge, Action<Action>? schedule = null)
    {
        if (!bridge.SupportsElementChanges)
        {
            Volatile.Write(ref status, "Native change delivery is unavailable in this add-on build; use manual Refresh.");
            return;
        }
        lock (sync)
        {
            if (client is not null) client.ElementChangesAvailable -= Wake;
            client = bridge;
            Volatile.Write(ref status, "Waiting for the native change-source baseline.");
            dispatch = schedule ?? ScheduleOnUi;
            using var json = JsonDocument.Parse(bridge.LastProjectInfoJson!);
            epoch = ArchicadProjectOptions.Data(json.RootElement).GetProperty("projectEpoch").GetInt64();
            ++generation;
            uiPending.Clear(); uiPosted = false;
            foreach (Scope scope in scopes) { scope.Revision = 0; scope.Initialized = false; }
            bridge.ElementChangesAvailable += Wake;
        }
        Wake();
    }

    internal static void Detach(Gh2PipeClient? bridge)
    {
        lock (sync)
        {
            if (!ReferenceEquals(client, bridge)) return;
            if (client is not null) client.ElementChangesAvailable -= Wake;
            client = null; ++generation; uiPending.Clear(); uiPosted = false;
            Volatile.Write(ref status, "Disconnected; automatic input refresh is paused.");
        }
    }

    internal static void Subscribe(IArchicadChangeConsumer owner, string key, Guid[] ids, int fields)
    {
        bool added = false;
        lock (sync)
        {
            scopes.RemoveAll(scope => !scope.Owner.TryGetTarget(out var target) || !target.IsActive);
            if (scopes.Any(scope => scope.Key == key && scope.Owner.TryGetTarget(out var target) && ReferenceEquals(owner, target))) return;
            // Match the query cache's bounded working set. Old scopes are hints,
            // never a reason to retain unbounded GUID subscriptions.
            var owned = scopes.Where(scope => scope.Owner.TryGetTarget(out var target) && ReferenceEquals(owner, target)).ToArray();
            if (owned.Length >= 32) foreach (Scope scope in owned) scopes.Remove(scope);
            if (scopes.Count >= 1024) throw new InvalidOperationException("Archicad subscription capacity reached.");
            scopes.Add(new(owner, key, ids.Distinct().ToArray(), fields));
            added = true;
        }
        if (added) Wake();
    }

    private static void Wake()
    {
        lock (sync)
        {
            pending = true;
            if (running || client is null) return;
            running = true;
        }
        _ = Task.Run(Drain);
    }

    private static async Task Drain()
    {
        try
        {
            while (true)
            {
                Scope[] batch;
                Gh2PipeClient bridge;
                long token, expectedEpoch;
                lock (sync)
                {
                    if (!pending || client is null) { running = false; return; }
                    pending = false; bridge = client; token = generation; expectedEpoch = epoch;
                    batch = scopes.ToArray();
                }
                var dirty = new HashSet<IArchicadChangeConsumer>();
                try
                {
                    foreach (Scope scope in batch)
                    {
                        if (!scope.Owner.TryGetTarget(out var owner) || !owner.IsActive) continue;
                        long next = long.MaxValue;
                        bool invalidate = false;
                        foreach (Guid[] page in scope.Ids.Chunk(64))
                        {
                            lock (sync) if (token != generation) break;
                            using var json = JsonDocument.Parse(await bridge.ReadElementChangesAsync(page, expectedEpoch, scope.Revision, scope.Fields));
                            var data = ArchicadProjectOptions.Data(json.RootElement);
                            if (data.GetProperty("projectEpoch").GetInt64() != expectedEpoch)
                                throw new InvalidDataException("Archicad project changed; refresh the connection before querying references.");
                            bool active = data.GetProperty("sourceActive").GetBoolean();
                            Volatile.Write(ref status, active ? "Automatic input refresh: GUID-scoped ModelWatch hints." :
                                "ModelWatch is inactive; automatic input refresh is paused. Use manual Refresh.");
                            // An inactive source is not proof of freshness. Preserve the
                            // cache and wait for the source-start wake/manual refresh.
                            if (!active) continue;
                            next = Math.Min(next, data.GetProperty("revision").GetInt64());
                            invalidate |= !scope.Initialized || data.GetProperty("resyncRequired").GetBoolean() ||
                                data.GetProperty("changes").GetArrayLength() != 0;
                        }
                        lock (sync)
                        {
                            if (token != generation) break;
                            if (next != long.MaxValue) { scope.Revision = next; scope.Initialized = true; }
                        }
                        if (invalidate) dirty.Add(owner);
                    }
                }
                finally
                {
                    // Earlier scopes already advanced their cursors. A later
                    // request failure must not discard those invalidations.
                    if (dirty.Count != 0) QueueInvalidations(dirty, token);
                }
            }
        }
        catch (Exception error)
        {
            Volatile.Write(ref status, "Automatic input refresh paused: " + error.Message);
            System.Diagnostics.Trace.WriteLine("Archicad change delivery: " + error.Message);
            bool retry;
            lock (sync) { running = false; retry = pending && client is not null; }
            if (retry) Wake(); // preserve a wake that arrived during the failed read
        }
    }

    private static void QueueInvalidations(HashSet<IArchicadChangeConsumer> dirty, long token)
    {
        lock (sync)
        {
            if (token != generation) return;
            uiPending.UnionWith(dirty);
            if (uiPosted) return;
            uiPosted = true;
        }
        try { dispatch(() =>
        {
            IArchicadChangeConsumer[] targets;
            lock (sync)
            {
                if (token != generation) return;
                targets = uiPending.ToArray(); uiPending.Clear(); uiPosted = false;
            }
            var documents = new HashSet<Document>();
            foreach (var owner in targets)
                if (owner.IsActive && owner.InvalidateArchicadInputs() is { } document) documents.Add(document);
            foreach (Document document in documents) document.Solution.Start();
        }); }
        catch
        {
            lock (sync)
            {
                if (token == generation)
                {
                    foreach (Scope scope in scopes)
                        if (scope.Owner.TryGetTarget(out var owner) && uiPending.Contains(owner)) scope.Initialized = false;
                    uiPosted = false; uiPending.Clear();
                }
            }
            throw;
        }
    }
}
