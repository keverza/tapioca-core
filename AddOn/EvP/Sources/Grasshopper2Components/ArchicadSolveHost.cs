using Eto.Forms;
using Grasshopper2.Doc;

namespace TapiocaGH2;

// The GH2 side of input revisions: the UI scheduler and one definition's solver.
internal sealed class ArchicadSolveHost : IArchicadSolveHost
{
    public long NowMs => Environment.TickCount64;
    public Task Delay(int milliseconds, CancellationToken cancel) => Task.Delay(milliseconds, cancel);

    // A headless fixture has no Eto message loop; it supplies its own queue.
    internal static Action<Action>? PostOverride = null;

    public void Post(Action action)
    {
        if (Volatile.Read(ref PostOverride) is { } post) { post(action); return; }
        (Application.Instance ?? throw new InvalidOperationException("GH2 UI scheduler is unavailable.")).AsyncInvoke(action);
    }

    // CurrentlyRunning is cleared by every ending: completed, cancelled or faulted.
    public bool IsSolving(object group) => ((Document)group).Solution.State.CurrentlyRunning is not null;

    public void WhenIdle(object group, Action callback)
    {
        SolutionServer server = ((Document)group).Solution;
        int fired = 0;
        EventHandler<SolutionEventArgs>? stopped = null;
        void Fire()
        {
            if (Interlocked.Exchange(ref fired, 1) != 0) return;
            server.SolutionStopped -= stopped;
            Post(callback);
        }
        // Every started solution raises SolutionStopped, on the solver's thread.
        stopped = (_, _) => Fire();
        server.SolutionStopped += stopped;
        // It may have stopped between the caller's check and this subscription.
        if (server.State.CurrentlyRunning is null) Fire();
    }

    public void Start(object group)
    {
        var document = (Document)group;
        if (document.State is DocumentState.Closing or DocumentState.Closed) return;
        document.Solution.Start();
    }
}

internal static class ArchicadInputs
{
    internal static readonly ArchicadInputRevisions Shared = new(new ArchicadSolveHost());
}
