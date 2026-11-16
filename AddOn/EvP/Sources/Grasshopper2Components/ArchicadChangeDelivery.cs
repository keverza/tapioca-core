using System.Text.Json;
using Tapioca.Gh2Worker;

namespace TapiocaGH2;

// Host wakes, independent scoped cursors, one active drain plus one newest wake.
// No component timer polls Archicad; weak owners do not retain closed definitions.
// A drain only names what changed: reading the rows, adopting them and solving
// belong to ArchicadInputRevisions.
internal static class ArchicadChangeDelivery
{
    private sealed class Scope(IArchicadInputConsumer owner, string key, Guid[] ids, int fields)
    {
        internal readonly WeakReference<IArchicadInputConsumer> Owner = new(owner);
        internal readonly string Key = key;
        internal readonly Guid[] Ids = ids;
        internal readonly int Fields = fields;
        internal long Revision;
        internal bool Initialized;
        internal readonly TaskCompletionSource Baseline = new(TaskCreationOptions.RunContinuationsAsynchronously);
    }
    private static readonly object sync = new();
    private static readonly List<Scope> scopes = [];
    private static Gh2PipeClient? client;
    private static long epoch, generation;
    private static bool pending, running;
    private static ArchicadInputRevisions? revisions;
    private static string status = "Automatic input refresh awaits a matched native connection.";
    internal static string Status => Volatile.Read(ref status);

    internal static void Attach(Gh2PipeClient bridge, ArchicadInputRevisions inputs)
    {
        inputs.Reset(); // reads against the previous connection are abandoned
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
            revisions = inputs;
            using var json = JsonDocument.Parse(bridge.LastProjectInfoJson!);
            epoch = ArchicadProjectOptions.Data(json.RootElement).GetProperty("projectEpoch").GetInt64();
            ++generation;
            foreach (Scope scope in scopes)
            {
                scope.Baseline.TrySetResult();
                scope.Revision = 0; scope.Initialized = false;
            }
            bridge.ElementChangesAvailable += Wake;
        }
        Wake();
    }

    internal static void Detach(Gh2PipeClient? bridge)
    {
        ArchicadInputRevisions? inputs;
        lock (sync)
        {
            if (!ReferenceEquals(client, bridge)) return;
            if (client is not null) client.ElementChangesAvailable -= Wake;
            client = null; ++generation; inputs = revisions;
            foreach (Scope scope in scopes) scope.Baseline.TrySetResult();
            Volatile.Write(ref status, "Disconnected; automatic input refresh is paused.");
        }
        inputs?.Reset();
    }

    // New readers await this barrier before requesting rows. Completion releases
    // the read, not a freshness guarantee: unavailable sources fall back to manual
    // reads and retain an uninitialized cursor for a later source-start resync.
    internal static Task Subscribe(IArchicadInputConsumer owner, string key, Guid[] ids, int fields,
        bool beforeFirstRead = false)
    {
        bool added = false;
        Scope subscription;
        lock (sync)
        {
            foreach (Scope scope in scopes.Where(scope => !scope.Owner.TryGetTarget(out var target) || !target.IsActive).ToArray())
            {
                scope.Baseline.TrySetResult();
                scopes.Remove(scope);
            }
            Scope? existing = scopes.Find(scope => scope.Key == key && scope.Owner.TryGetTarget(out var target) && ReferenceEquals(owner, target));
            if (existing is not null) return existing.Baseline.Task;
            // Match the query cache's bounded working set. Old scopes are hints,
            // never a reason to retain unbounded GUID subscriptions.
            var owned = scopes.Where(scope => scope.Owner.TryGetTarget(out var target) && ReferenceEquals(owner, target)).ToArray();
            if (owned.Length >= 32) foreach (Scope scope in owned)
            {
                scope.Baseline.TrySetResult();
                scopes.Remove(scope);
            }
            if (scopes.Count >= 1024) throw new InvalidOperationException("Archicad subscription capacity reached.");
            subscription = new(owner, key, ids.Distinct().ToArray(), fields);
            if (!beforeFirstRead || client is null) subscription.Baseline.TrySetResult();
            scopes.Add(subscription);
            added = true;
        }
        if (added) Wake();
        return subscription.Baseline.Task;
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
                var dirty = new Dictionary<IArchicadInputConsumer, HashSet<Guid>>();
                try
                {
                    foreach (Scope scope in batch)
                    {
                        if (!scope.Owner.TryGetTarget(out var owner) || !owner.IsActive)
                        {
                            scope.Baseline.TrySetResult();
                            continue;
                        }
                        long next = long.MaxValue;
                        bool whole = false;
                        var changed = new HashSet<Guid>();
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
                            // A first baseline or a lost history says nothing about which
                            // rows moved: every row of this scope is re-read, and only this scope.
                            whole |= !scope.Initialized || data.GetProperty("resyncRequired").GetBoolean();
                            foreach (JsonElement change in data.GetProperty("changes").EnumerateArray())
                                changed.Add(change.GetProperty("guid").GetGuid());
                        }
                        lock (sync)
                        {
                            if (token != generation) break;
                            if (next != long.MaxValue) { scope.Revision = next; scope.Initialized = true; }
                            // A waiting first read starts after this cursor, so it
                            // already includes the baseline's hints. Inactive/error
                            // fallback leaves Initialized false for a later resync.
                            if (!scope.Baseline.Task.IsCompleted)
                            {
                                whole = false;
                                changed.Clear();
                                scope.Baseline.TrySetResult();
                            }
                        }
                        if (whole) changed.UnionWith(scope.Ids);
                        if (changed.Count == 0) continue;
                        if (dirty.TryGetValue(owner, out HashSet<Guid>? known)) known.UnionWith(changed);
                        else dirty[owner] = changed;
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
            lock (sync)
            {
                foreach (Scope scope in scopes) scope.Baseline.TrySetResult();
                running = false; retry = pending && client is not null;
            }
            if (retry) Wake(); // preserve a wake that arrived during the failed read
        }
    }

    private static void QueueInvalidations(Dictionary<IArchicadInputConsumer, HashSet<Guid>> dirty, long token)
    {
        ArchicadInputRevisions? inputs;
        lock (sync)
        {
            if (token != generation) return;
            inputs = revisions;
        }
        foreach (var (owner, ids) in dirty) inputs?.Invalidate(owner, ids);
    }
}
