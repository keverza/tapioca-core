using System.Runtime.CompilerServices;

namespace TapiocaGH2;

// One component's Archicad-backed inputs. Staging reads replacement rows while
// the rows the running solution reads stay untouched; committing adopts them.
internal interface IArchicadInputConsumer
{
    bool IsActive { get; }
    // The solve scope (the owning definition); null while detached or closing.
    object? InputGroup { get; }
    // Off the UI thread. `changed` null means every subscribed row. Returns false
    // when the read failed and may be retried; on `lastAttempt` the consumer
    // records the failure so the stale state becomes explicit at commit.
    Task<bool> StageInputsAsync(IReadOnlyCollection<Guid>? changed, bool lastAttempt, CancellationToken cancel);
    // UI thread, with no solution running in the group. Returns true when
    // visible data changed and the consumer expired itself.
    bool CommitInputs();
}

internal interface IArchicadSolveHost
{
    long NowMs { get; }
    Task Delay(int milliseconds, CancellationToken cancel);
    void Post(Action action);
    bool IsSolving(object group);
    // One-shot; the callback runs on the UI thread after the running solution stops.
    void WhenIdle(object group, Action callback);
    void Start(object group);
}

// Input revisions for one connection: a burst of change hints becomes one read
// of the affected rows, one atomic adoption across every affected component of
// a definition, and one solution. One solution is active at a time; newer
// inputs wait as the single pending revision and replace each other there.
// Nothing here calls Archicad or the solver from a notification callback.
internal sealed class ArchicadInputRevisions(IArchicadSolveHost host)
{
    internal const int SettleMs = 60;        // quiet interval before the first read of a burst
    internal const int MaxStalenessMs = 400; // staged rows are published this long after staging at the latest
    internal const int RetryMs = 250;
    internal const int MaxStageAttempts = 3;
    internal const int MaxPostAttempts = 5;
    private const int MaxDirtyGuids = 4096;  // beyond this a consumer is refreshed whole

    private sealed class Pending(HashSet<Guid>? ids, int attempt)
    {
        internal HashSet<Guid>? Ids = ids;
        internal int Attempt = attempt;
    }

    private sealed class Group(object key)
    {
        internal readonly object Key = key;
        internal readonly Dictionary<IArchicadInputConsumer, Pending> Dirty = [];
        internal readonly HashSet<IArchicadInputConsumer> Staged = [];
        internal bool Fetching, CommitPosted, WaitingIdle, FlushArmed;
        internal int Outstanding, PostAttempts;
        internal long Loop, Revision;
    }

    private readonly object sync = new();
    private readonly ConditionalWeakTable<object, Group> groups = new();
    private CancellationTokenSource lifetime = new();

    // Committed input revisions of a definition; a diagnostic counter.
    internal long Revision(object key)
    {
        lock (sync) return groups.TryGetValue(key, out Group? group) ? group.Revision : 0;
    }

    // The connection changed: abandon reads in flight and unread hints. Rows a
    // consumer already staged stay with it and are judged by its own tickets.
    internal void Reset()
    {
        CancellationTokenSource previous;
        lock (sync)
        {
            previous = lifetime;
            lifetime = new();
            foreach (var entry in groups)
            {
                entry.Value.Dirty.Clear();
                entry.Value.Fetching = false;
                ++entry.Value.Loop;
            }
        }
        previous.Cancel(); // outside the lock: continuations may run on this stack
    }

    // Any thread. Never reads, expires or solves on the caller's stack.
    internal void Invalidate(IArchicadInputConsumer consumer, IReadOnlyCollection<Guid>? changed)
    {
        if (!consumer.IsActive || consumer.InputGroup is not { } key) return;
        Group group;
        long loop;
        CancellationToken cancel;
        lock (sync)
        {
            group = groups.GetValue(key, static created => new Group(created));
            Merge(group, consumer, changed, 0);
            if (group.Fetching) return;
            group.Fetching = true;
            loop = ++group.Loop;
            cancel = lifetime.Token;
        }
        _ = Task.Run(() => FetchAsync(group, loop, cancel));
    }

    // A consumer's own first read of a new request. While any is outstanding,
    // adoption waits (bounded by MaxStalenessMs) so sibling reads land together.
    internal object? BeginFetch(IArchicadInputConsumer consumer)
    {
        if (consumer.InputGroup is not { } key) return null;
        lock (sync)
        {
            Group group = groups.GetValue(key, static created => new Group(created));
            ++group.Outstanding;
            return group;
        }
    }

    internal void EndFetch(object? handle, IArchicadInputConsumer consumer, bool staged)
    {
        if (handle is not Group group) return;
        lock (sync)
        {
            if (group.Outstanding > 0) --group.Outstanding;
            if (staged) group.Staged.Add(consumer);
        }
        Publish(group);
    }

    private static void Merge(Group group, IArchicadInputConsumer consumer, IReadOnlyCollection<Guid>? changed, int attempt)
    {
        if (!group.Dirty.TryGetValue(consumer, out Pending? pending))
            group.Dirty[consumer] = pending = new(changed is null ? null : [.. changed], attempt);
        else
        {
            if (changed is null) pending.Ids = null;
            else pending.Ids?.UnionWith(changed);
            // A fresh hint restarts the retry budget; a retry keeps the larger count.
            pending.Attempt = attempt == 0 ? 0 : Math.Max(pending.Attempt, attempt);
        }
        if (pending.Ids is { Count: > MaxDirtyGuids }) pending.Ids = null;
    }

    private async Task FetchAsync(Group group, long loop, CancellationToken cancel)
    {
        try
        {
            await host.Delay(SettleMs, cancel).ConfigureAwait(false);
            while (true)
            {
                KeyValuePair<IArchicadInputConsumer, Pending>[] take;
                lock (sync)
                {
                    if (group.Loop != loop) return;
                    take = [.. group.Dirty];
                    group.Dirty.Clear();
                }
                bool retry = false;
                foreach (var (consumer, pending) in take)
                {
                    cancel.ThrowIfCancellationRequested();
                    if (!consumer.IsActive || !ReferenceEquals(consumer.InputGroup, group.Key)) continue;
                    bool last = pending.Attempt + 1 >= MaxStageAttempts;
                    bool complete;
                    try { complete = await consumer.StageInputsAsync(pending.Ids, last, cancel).ConfigureAwait(false); }
                    catch (OperationCanceledException) { throw; }
                    catch (Exception error)
                    {
                        System.Diagnostics.Trace.WriteLine("Archicad input staging: " + error.Message);
                        complete = false;
                    }
                    lock (sync)
                    {
                        if (group.Loop != loop) return;
                        if (complete || last) group.Staged.Add(consumer);
                        else { Merge(group, consumer, pending.Ids, pending.Attempt + 1); retry = true; }
                    }
                }
                bool fresh;
                lock (sync)
                {
                    if (group.Loop != loop) return;
                    fresh = group.Dirty.Values.Any(pending => pending.Attempt == 0);
                    if (group.Dirty.Count == 0) group.Fetching = false;
                }
                Publish(group);
                lock (sync) if (group.Loop != loop || !group.Fetching) return;
                if (retry && !fresh) await host.Delay(RetryMs, cancel).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) { }
        finally
        {
            lock (sync) if (group.Loop == loop) group.Fetching = false;
        }
    }

    // Adopt now when nothing newer is on its way; otherwise hold the staged rows
    // so the newest revision supersedes them, for at most MaxStalenessMs.
    private void Publish(Group group, bool force = false)
    {
        bool post = false, arm = false;
        CancellationToken cancel;
        lock (sync)
        {
            cancel = lifetime.Token;
            if (group.Staged.Count == 0) return;
            bool more = group.Outstanding > 0 || group.Dirty.Values.Any(pending => pending.Attempt == 0);
            if (more && !force)
            {
                arm = !group.FlushArmed;
                group.FlushArmed = true;
            }
            else if (!group.CommitPosted) post = group.CommitPosted = true;
        }
        if (arm) _ = FlushAsync(group, cancel);
        if (post) PostCommit(group);
    }

    private async Task FlushAsync(Group group, CancellationToken cancel)
    {
        try { await host.Delay(MaxStalenessMs, cancel).ConfigureAwait(false); }
        catch (OperationCanceledException) { }
        lock (sync) group.FlushArmed = false;
        // Staged rows belong to their consumers and outlive a connection reset.
        Publish(group, force: true);
    }

    private void PostCommit(Group group)
    {
        try
        {
            host.Post(() => Commit(group));
            lock (sync) group.PostAttempts = 0;
        }
        catch (Exception error)
        {
            // The UI scheduler refused. Staged rows stay with their consumers.
            System.Diagnostics.Trace.WriteLine("Archicad input adoption: " + error.Message);
            bool retry;
            CancellationToken cancel;
            lock (sync)
            {
                group.CommitPosted = false;
                retry = ++group.PostAttempts < MaxPostAttempts;
                if (!retry) group.PostAttempts = 0;
                cancel = lifetime.Token;
            }
            if (retry) _ = RetryPostAsync(group, cancel);
        }
    }

    private async Task RetryPostAsync(Group group, CancellationToken cancel)
    {
        try { await host.Delay(RetryMs, cancel).ConfigureAwait(false); }
        catch (OperationCanceledException) { }
        lock (sync)
        {
            if (group.CommitPosted || group.Staged.Count == 0) return;
            group.CommitPosted = true;
        }
        PostCommit(group);
    }

    // UI thread.
    private void Commit(Group group)
    {
        lock (sync) group.CommitPosted = false;
        if (host.IsSolving(group.Key))
        {
            // One active solution. The staged rows keep absorbing newer reads
            // and are adopted once, when it stops.
            lock (sync)
            {
                if (group.WaitingIdle) return;
                group.WaitingIdle = true;
            }
            try
            {
                host.WhenIdle(group.Key, () =>
                {
                    lock (sync) group.WaitingIdle = false;
                    Commit(group);
                });
            }
            catch
            {
                lock (sync) group.WaitingIdle = false;
                throw;
            }
            return;
        }
        IArchicadInputConsumer[] targets;
        lock (sync)
        {
            targets = [.. group.Staged];
            group.Staged.Clear();
        }
        bool changed = false;
        foreach (IArchicadInputConsumer target in targets)
        {
            if (!target.IsActive || !ReferenceEquals(target.InputGroup, group.Key)) continue;
            try { changed |= target.CommitInputs(); }
            catch (Exception error) { System.Diagnostics.Trace.WriteLine("Archicad input commit: " + error.Message); }
        }
        if (!changed) return; // unrelated or identical data: no expiry, no solution
        lock (sync) ++group.Revision;
        try { host.Start(group.Key); }
        catch (Exception error) { System.Diagnostics.Trace.WriteLine("Archicad input solve: " + error.Message); }
    }
}
