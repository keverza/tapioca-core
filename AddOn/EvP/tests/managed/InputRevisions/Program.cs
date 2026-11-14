using System.Collections.Concurrent;
using TapiocaGH2;

// Input revisions: a burst of change hints becomes one read, one atomic
// adoption and one solution. Time, the UI queue and the solver are all driven
// by the test, so each rule is checked rather than raced.

Guid a = Guid.NewGuid(), b = Guid.NewGuid(), c = Guid.NewGuid();

Run("a burst is read once, adopted once and solved once", (host, inputs) =>
{
    var consumer = new Consumer(host.Document);
    inputs.Invalidate(consumer, [a]);
    inputs.Invalidate(consumer, [b]);
    inputs.Invalidate(consumer, [a, c]);
    Check(consumer.Stages.Count == 0, "no read on the notifying stack or before the quiet interval");
    host.AdvanceWhenWaiting(ArchicadInputRevisions.SettleMs);
    Until(() => host.Posted == 1, "adoption posted");
    Check(consumer.Stages.Count == 1 && consumer.Stages.Single()!.SetEquals([a, b, c]), "one read of the union");
    Check(consumer.Commits == 0 && host.Starts == 0, "nothing adopted off the UI thread");
    host.Pump();
    Check(consumer.Commits == 1 && host.Starts == 1 && inputs.Revision(host.Document) == 1, "one adoption, one solution");
});

Run("identical rows do not solve", (host, inputs) =>
{
    var consumer = new Consumer(host.Document) { Changes = false };
    inputs.Invalidate(consumer, [a]);
    host.AdvanceWhenWaiting(ArchicadInputRevisions.SettleMs);
    Until(() => host.Posted == 1, "adoption posted");
    host.Pump();
    Check(consumer.Commits == 1 && host.Starts == 0 && inputs.Revision(host.Document) == 0, "an unrelated change costs no solution");
});

Run("components of one definition adopt together", (host, inputs) =>
{
    var first = new Consumer(host.Document);
    var second = new Consumer(host.Document);
    var other = new Consumer(new object());
    inputs.Invalidate(first, [a]);
    inputs.Invalidate(second, [b]);
    inputs.Invalidate(other, [c]);
    host.AdvanceWhenWaiting(ArchicadInputRevisions.SettleMs, waiters: 2);
    Until(() => host.Posted == 2, "one adoption per definition");
    host.Pump();
    Check(first.Commits == 1 && second.Commits == 1 && other.Commits == 1, "every affected component adopted");
    Check(host.StartsOf(host.Document) == 1 && host.Starts == 2, "one solution per definition, not per component");
});

Run("a newer change supersedes rows not yet adopted", (host, inputs) =>
{
    var consumer = new Consumer(host.Document);
    consumer.Hold();
    inputs.Invalidate(consumer, [a]);
    host.AdvanceWhenWaiting(ArchicadInputRevisions.SettleMs);
    Until(() => consumer.Stages.Count == 1, "first read started");
    inputs.Invalidate(consumer, [b]);
    consumer.Release();
    Until(() => consumer.Stages.Count == 2, "second read follows without a new quiet interval");
    Until(() => host.Posted == 1, "adoption posted");
    host.Pump();
    Check(consumer.Stages.ElementAt(1)!.SetEquals([b]), "only the newly changed row is read again");
    Check(consumer.Commits == 1 && host.Starts == 1, "the intermediate rows were never solved");
});

Run("sustained edits still publish within the staleness bound", (host, inputs) =>
{
    var consumer = new Consumer(host.Document);
    consumer.Hold();
    inputs.Invalidate(consumer, [a]);
    host.AdvanceWhenWaiting(ArchicadInputRevisions.SettleMs);
    Until(() => consumer.Stages.Count == 1, "first read in flight");
    inputs.Invalidate(consumer, [b]);
    consumer.ReleaseAndHold();
    Until(() => consumer.Stages.Count == 2, "second read in flight");
    Check(host.Posted == 0, "the first rows wait for the newer read");
    host.AdvanceWhenWaiting(ArchicadInputRevisions.MaxStalenessMs);
    Until(() => host.Posted == 1, "staged rows are published although a newer read continues");
    host.Pump();
    Check(consumer.Commits == 1 && host.Starts == 1, "a solution ran during the sustained burst");
    consumer.Release();
    Until(() => host.Posted == 2, "the newest rows follow");
    host.Pump();
    Check(consumer.Commits == 2 && host.Starts == 2, "and are solved last");
});

Run("one active solution; newer inputs wait as one pending revision", (host, inputs) =>
{
    var consumer = new Consumer(host.Document);
    host.Solving(host.Document, true);
    inputs.Invalidate(consumer, [a]);
    host.AdvanceWhenWaiting(ArchicadInputRevisions.SettleMs);
    Until(() => host.Posted == 1, "first adoption posted");
    host.Pump();
    Check(consumer.Commits == 0 && host.Starts == 0, "rows are not swapped under a running solution");
    inputs.Invalidate(consumer, [b]);
    host.AdvanceWhenWaiting(ArchicadInputRevisions.SettleMs);
    Until(() => host.Posted == 2, "second adoption posted");
    host.Pump();
    Check(consumer.Commits == 0 && host.IdleWaiters == 1, "still one waiter, not one per change");
    host.Solving(host.Document, false);
    host.Pump();
    Check(consumer.Commits == 1 && host.Starts == 1, "both changes adopted once when the solution stopped");
    host.Pump();
    Check(consumer.Commits == 1 && host.Starts == 1, "no trailing duplicate");
});

Run("a failed read is retried, then reported", (host, inputs) =>
{
    var consumer = new Consumer(host.Document) { Failures = 2 };
    inputs.Invalidate(consumer, [a]);
    host.AdvanceWhenWaiting(ArchicadInputRevisions.SettleMs);
    Until(() => consumer.Stages.Count == 1, "first attempt");
    host.Pump();
    Check(consumer.Commits == 0, "a failed read adopts nothing");
    host.AdvanceWhenWaiting(ArchicadInputRevisions.RetryMs);
    Until(() => consumer.Stages.Count == 2, "second attempt");
    host.AdvanceWhenWaiting(ArchicadInputRevisions.RetryMs);
    Until(() => consumer.Stages.Count == 3, "third attempt");
    Until(() => host.Posted == 1, "adoption posted");
    host.Pump();
    Check(consumer.LastAttempts.SequenceEqual([false, false, true]), "only the last attempt may record the failure");
    Check(consumer.Stages.All(ids => ids!.SetEquals([a])), "each retry names the same rows");
    Check(consumer.Commits == 1, "the outcome reaches the component once");
});

Run("a persistent failure stops after the last attempt", (host, inputs) =>
{
    var consumer = new Consumer(host.Document) { Failures = int.MaxValue };
    inputs.Invalidate(consumer, [a]);
    host.AdvanceWhenWaiting(ArchicadInputRevisions.SettleMs);
    for (int attempt = 1; attempt < ArchicadInputRevisions.MaxStageAttempts; attempt++)
    {
        Until(() => consumer.Stages.Count == attempt, "attempt " + attempt);
        host.AdvanceWhenWaiting(ArchicadInputRevisions.RetryMs);
    }
    Until(() => host.Posted == 1, "the stale state is adopted");
    host.Pump();
    host.Advance(10 * ArchicadInputRevisions.RetryMs);
    Thread.Sleep(50);
    Check(consumer.Stages.Count == ArchicadInputRevisions.MaxStageAttempts, "no unbounded retry loop");
});

Run("an unavailable UI scheduler does not lose staged rows", (host, inputs) =>
{
    var consumer = new Consumer(host.Document);
    host.FailPosts = 1;
    inputs.Invalidate(consumer, [a]);
    host.AdvanceWhenWaiting(ArchicadInputRevisions.SettleMs);
    Until(() => host.PostFailures == 1, "the first post was refused");
    host.AdvanceWhenWaiting(ArchicadInputRevisions.RetryMs);
    Until(() => host.Posted == 1, "posted again");
    host.Pump();
    Check(consumer.Commits == 1 && host.Starts == 1, "the rows were adopted after the retry");
});

Run("a connection reset abandons reads in flight", (host, inputs) =>
{
    var consumer = new Consumer(host.Document);
    consumer.Hold();
    inputs.Invalidate(consumer, [a]);
    host.AdvanceWhenWaiting(ArchicadInputRevisions.SettleMs);
    Until(() => consumer.Stages.Count == 1, "read in flight");
    inputs.Reset();
    Check(consumer.Cancelled.Wait(TimeSpan.FromSeconds(5)), "the read was cancelled");
    consumer.Release();
    Thread.Sleep(50);
    host.Pump();
    Check(consumer.Commits == 0 && host.Starts == 0, "nothing from the old connection is adopted");
    inputs.Invalidate(consumer, [b]);
    host.AdvanceWhenWaiting(ArchicadInputRevisions.SettleMs);
    Until(() => consumer.Stages.Count == 2, "the new connection reads again");
    Until(() => host.Posted == 1, "adoption posted");
    host.Pump();
    Check(consumer.Commits == 1 && host.Starts == 1, "and adopts");
});

Run("first reads of sibling components land in one solution", (host, inputs) =>
{
    var first = new Consumer(host.Document);
    var second = new Consumer(host.Document);
    object? one = inputs.BeginFetch(first), two = inputs.BeginFetch(second);
    inputs.EndFetch(one, first, staged: true);
    Check(host.Posted == 0, "the first arrival waits for its sibling");
    inputs.EndFetch(two, second, staged: true);
    Until(() => host.Posted == 1, "adoption posted");
    host.Pump();
    Check(first.Commits == 1 && second.Commits == 1 && host.Starts == 1, "one solution for both");
});

Run("a slow sibling cannot hold rows past the staleness bound", (host, inputs) =>
{
    var first = new Consumer(host.Document);
    var slow = new Consumer(host.Document);
    object? one = inputs.BeginFetch(first), two = inputs.BeginFetch(slow);
    inputs.EndFetch(one, first, staged: true);
    host.AdvanceWhenWaiting(ArchicadInputRevisions.MaxStalenessMs);
    Until(() => host.Posted == 1, "published without the slow sibling");
    host.Pump();
    Check(first.Commits == 1 && slow.Commits == 0 && host.Starts == 1, "only the staged component adopted");
    inputs.EndFetch(two, slow, staged: false);
    Thread.Sleep(20);
    Check(host.Posted == 1, "a read that staged nothing posts nothing");
});

Run("detached and foreign consumers are skipped", (host, inputs) =>
{
    var inactive = new Consumer(host.Document) { Active = false };
    var detached = new Consumer(null);
    inputs.Invalidate(inactive, [a]);
    inputs.Invalidate(detached, [a]);
    Check(inputs.BeginFetch(detached) is null, "no group without a definition");
    host.Advance(ArchicadInputRevisions.SettleMs);
    Thread.Sleep(50);
    Check(inactive.Stages.Count == 0 && detached.Stages.Count == 0 && host.Posted == 0, "nothing was read or posted");
    var moved = new Consumer(host.Document);
    inputs.Invalidate(moved, [a]);
    moved.Group = new object(); // the component left the definition before its read
    host.AdvanceWhenWaiting(ArchicadInputRevisions.SettleMs);
    Thread.Sleep(50);
    Check(moved.Stages.Count == 0, "a component is never read for a definition it left");
});

Run("an unbounded change set collapses to a whole refresh", (host, inputs) =>
{
    var consumer = new Consumer(host.Document);
    inputs.Invalidate(consumer, Enumerable.Range(0, 5000).Select(_ => Guid.NewGuid()).ToArray());
    host.AdvanceWhenWaiting(ArchicadInputRevisions.SettleMs);
    Until(() => consumer.Stages.Count == 1, "read");
    Check(consumer.Stages.Single() is null, "the dirty set is bounded");
});

Console.WriteLine("GH2 input revisions: coalescing, selective solve, supersession, staleness bound, one active solution, retries and reset passed.");
return 0;

static void Run(string name, Action<Host, ArchicadInputRevisions> body)
{
    var host = new Host();
    try { body(host, new ArchicadInputRevisions(host)); }
    catch (Exception error) { throw new Exception(name + ": " + error.Message, error); }
    Console.WriteLine("  ok  " + name);
}

static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }

static void Until(Func<bool> condition, string message)
{
    if (!SpinWait.SpinUntil(condition, TimeSpan.FromSeconds(5))) throw new TimeoutException(message);
}

sealed class Consumer(object? group) : IArchicadInputConsumer
{
    private volatile TaskCompletionSource? gate;
    internal object? Group = group;
    internal bool Active = true, Changes = true;
    internal int Failures, Commits;
    internal readonly ConcurrentQueue<HashSet<Guid>?> Stages = new();
    internal readonly ConcurrentQueue<bool> LastAttempts = new();
    internal readonly ManualResetEventSlim Cancelled = new();
    public bool IsActive => Active;
    public object? InputGroup => Group;
    internal void Hold() => gate = new(TaskCreationOptions.RunContinuationsAsynchronously);
    internal void Release() { TaskCompletionSource? held = gate; gate = null; held?.TrySetResult(); }
    internal void ReleaseAndHold() { TaskCompletionSource? held = gate; Hold(); held?.TrySetResult(); }

    public async Task<bool> StageInputsAsync(IReadOnlyCollection<Guid>? changed, bool lastAttempt, CancellationToken cancel)
    {
        LastAttempts.Enqueue(lastAttempt);
        Stages.Enqueue(changed is null ? null : [.. changed]);
        if (gate is { } held)
        {
            using var registration = cancel.Register(() => { Cancelled.Set(); held.TrySetCanceled(cancel); });
            await held.Task.ConfigureAwait(false);
        }
        if (Failures <= 0) return true;
        --Failures;
        return false;
    }

    public bool CommitInputs() { ++Commits; return Changes; }
}

sealed class Host : IArchicadSolveHost
{
    private readonly object sync = new();
    private readonly List<(long Due, TaskCompletionSource Source)> delays = [];
    private readonly Queue<Action> queue = new();
    private readonly Dictionary<object, bool> solving = new(ReferenceEqualityComparer.Instance);
    private readonly Dictionary<object, List<Action>> idle = new(ReferenceEqualityComparer.Instance);
    private readonly Dictionary<object, int> starts = new(ReferenceEqualityComparer.Instance);
    private long now;
    private int posted, postFailures;
    internal readonly object Document = new();
    internal int FailPosts;
    internal int Posted => Volatile.Read(ref posted);
    internal int PostFailures => Volatile.Read(ref postFailures);
    internal int Starts { get { lock (sync) return starts.Values.Sum(); } }
    internal int StartsOf(object group) { lock (sync) return starts.GetValueOrDefault(group); }
    internal int IdleWaiters { get { lock (sync) return idle.Values.Sum(list => list.Count); } }
    public long NowMs { get { lock (sync) return now; } }

    public Task Delay(int milliseconds, CancellationToken cancel)
    {
        var source = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        cancel.Register(() => source.TrySetCanceled(cancel));
        lock (sync) delays.Add((now + milliseconds, source));
        return source.Task;
    }

    // Move the clock; everything due completes.
    internal void Advance(int milliseconds)
    {
        TaskCompletionSource[] due;
        lock (sync)
        {
            now += milliseconds;
            due = delays.Where(delay => delay.Due <= now).Select(delay => delay.Source).ToArray();
            delays.RemoveAll(delay => delay.Due <= now);
        }
        foreach (TaskCompletionSource source in due) source.TrySetResult();
    }

    // The scheduler reaches its next wait on another thread; advance once it has.
    internal void AdvanceWhenWaiting(int milliseconds, int waiters = 1)
    {
        if (!SpinWait.SpinUntil(() => { lock (sync) return delays.Count(delay => !delay.Source.Task.IsCompleted) >= waiters; },
                TimeSpan.FromSeconds(5)))
            throw new TimeoutException("the scheduler never waited");
        Advance(milliseconds);
    }

    public void Post(Action action)
    {
        lock (sync)
        {
            if (FailPosts > 0)
            {
                --FailPosts;
                Interlocked.Increment(ref postFailures);
                throw new InvalidOperationException("Injected UI scheduling failure.");
            }
            queue.Enqueue(action);
        }
        Interlocked.Increment(ref posted);
    }

    // The UI thread's turn.
    internal void Pump()
    {
        while (true)
        {
            Action? next;
            lock (sync) if (!queue.TryDequeue(out next)) return;
            next();
        }
    }

    public bool IsSolving(object group) { lock (sync) return solving.GetValueOrDefault(group); }

    internal void Solving(object group, bool value)
    {
        Action[] waiters = [];
        lock (sync)
        {
            solving[group] = value;
            if (!value && idle.Remove(group, out List<Action>? list)) waiters = [.. list];
        }
        foreach (Action waiter in waiters) Post(waiter);
    }

    public void WhenIdle(object group, Action callback)
    {
        lock (sync)
        {
            if (!idle.TryGetValue(group, out List<Action>? list)) idle[group] = list = [];
            list.Add(callback);
        }
    }

    public void Start(object group) { lock (sync) starts[group] = starts.GetValueOrDefault(group) + 1; }
}
