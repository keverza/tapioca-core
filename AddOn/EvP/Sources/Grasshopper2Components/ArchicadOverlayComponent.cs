using Eto.Drawing;
using Eto.Forms;
using Grasshopper2.Components;
using Grasshopper2.Doc;
using Grasshopper2.Parameters;
using Grasshopper2.UI;
using GrasshopperIO;
using Rhino;
using Rhino.Geometry;
using Tapioca.Gh2Worker;
using GhColour = Grasshopper2.Types.Colour.Colour;

namespace TapiocaGH2;

// BOUND BY OVERLAY-INVARIANTS.md: an explicit attached-editor display action,
// not a Player accepted solution/Apply path. Process never sends an API request.
public abstract class ArchicadOverlayComponent : Component
{
    private sealed record Batch(ArchicadOverlayGeometry.Prepared Geometry, ArchicadOverlayBridge.Lease Lease,
        SolutionId Solution, Guid Producer, string Payload, string Fingerprint);
    private sealed record Published(Batch Batch, long Lifetime);
    private Published? publishedBatch;
    private Guid producer = Guid.NewGuid(); // runtime only: copies/reopens never share a layer
    private sealed class DocumentSession { internal readonly Guid Id = Guid.NewGuid(); }
    private static readonly System.Runtime.CompilerServices.ConditionalWeakTable<Document, DocumentSession> documents = new();
    private Guid documentSession;
    private long revision;
    private Batch? prepared;
    private ArchicadOverlayBridge.Lease? shownLease;
    private Document? boundDocument;
    private string message = "Waiting for prepared geometry.";
    private int busy;
    private long lifetime;
    private long producerLifetime;
    private volatile bool automatic = true;
    internal bool Automatic => automatic;
    public bool AutoMode
    {
        get => automatic;
        set
        {
            automatic = value;
            if (Parameters.InputCount == 6 && Parameters.Input(5) is Grasshopper2.Parameters.Standard.BooleanParameter legacy)
            { legacy.Set(value); legacy.Expire(); }
            Expire();
        }
    }
    private UITimer? autoTimer;
    private Batch? observedBatch, attemptedBatch;
    private Batch? completedBatch;

    protected abstract bool Plan { get; }
    protected ArchicadOverlayComponent(Nomen nomen) : base(nomen) => DocumentChanged += OnDocumentChanged;
    protected ArchicadOverlayComponent(IReader reader) : base(reader)
    {
        automatic = !reader.HasItem((Name)"OverlayMode") || reader.String((Name)"OverlayMode") != "manual";
        DocumentChanged += OnDocumentChanged;
    }

    public override void Store(IWriter writer)
    {
        base.Store(writer);
        writer.String((Name)"OverlayMode", automatic ? "auto" : "manual");
    }

    internal void ToggleMode()
    {
        AddUndoRecord(new Grasshopper2.Undo.VerbNoun("Change", "Overlay Mode"),
            new Grasshopper2.Undo.Actions.PropertyAction(this, nameof(AutoMode), autoExtend: false));
        AutoMode = !AutoMode;
        Document?.Solution.Start();
    }

    protected override IAttributes CreateAttributes() => new ArchicadOverlayAttributes(this, Show, Clear);

    protected override void AddInputs(InputAdder inputs)
    {
        inputs.AddGeneric("Geometry", "G", "Mixed points, curves, meshes, surfaces, Breps and SubD; surfaces are tessellated for preview.", Access.Twig, Requirement.MayBeMissing);
        inputs.AddColour("Tint", "T", "RGBA preview colour; accepts Preview Tint.").Set(Colors.CornflowerBlue);
        inputs.AddNumber("Width", "W", "Curve line width in pixels, greater than zero and at most 64.").Set(2.0);
    }

    protected override void AddOutputs(OutputAdder outputs) =>
        outputs.AddText("Status", "S", "Prepared/published status. Published is not a claim that pixels are visible.");

    protected override void Process(IDataAccess access)
    {
        Volatile.Write(ref prepared, null);
        bool legacyPorts = Parameters.InputCount == 6;
        if (legacyPorts) { access.GetItem(5, out bool legacyAuto); automatic = legacyAuto; }
        access.SetItem(0, message);
        if (access.Iterations != 1)
        {
            access.AddWarning("Overlay branches", "Flatten Geometry and supply one Tint and Width. No partial branch will be published.");
            return;
        }
        if (!access.GetItem(legacyPorts ? 3 : 1, out GhColour tint) || !access.GetItem(legacyPorts ? 4 : 2, out double width)) return;
        access.GetPears<object>(0, out var items);
        object[] inputGeometry = items?.Select(pear => pear.Item).ToArray() ?? [];
        if (legacyPorts)
        {
            access.GetPears<Mesh>(1, out var meshes);
            access.GetPears<Point3d>(2, out var points);
            inputGeometry = inputGeometry.Concat(meshes?.Select(pear => (object)pear.Item) ?? [])
                .Concat(points?.Select(pear => (object)pear.Item) ?? []).ToArray();
            access.AddRemark("Legacy overlay ports", "Existing wiring is retained. Place a new overlay for the single Geometry input.");
        }
        try
        {
            ArchicadOverlayBridge.Lease lease = ArchicadOverlayBridge.Capture() ??
                throw new InvalidOperationException("Attach to Archicad, then solve again before showing geometry.");
            Color colour = tint.ToEto();
            if (!float.IsFinite(colour.R) || !float.IsFinite(colour.G) || !float.IsFinite(colour.B) || !float.IsFinite(colour.A))
                throw new ArgumentException("Tint channels must be finite.");
            static int Byte(float value) => (int)Math.Round(Math.Clamp(value, 0, 1) * 255);
            string rgba = $"{Byte(colour.R):X2}{Byte(colour.G):X2}{Byte(colour.B):X2}{Byte(colour.A):X2}";
            var geometry = ArchicadOverlayGeometry.PrepareMixed(inputGeometry,
                Plan, RhinoMath.UnitScale(RhinoDoc.ActiveDoc?.ModelUnitSystem ?? UnitSystem.Meters, UnitSystem.Meters), rgba, width);
            Guid batchProducer = producer;
            string payload = Gh2OverlayPayload.Encode(lease.Generation, batchProducer, Plan, geometry.Lines, geometry.Points, geometry.Meshes,
                lease.Epoch, documentSession, Interlocked.Increment(ref revision));
            string fingerprint = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(
                System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(geometry)));
            Volatile.Write(ref prepared, new(geometry, lease, access.Solution.Id, batchProducer, payload, fingerprint));
            access.SetItem(0, (automatic ? "Auto sync. " : "Manual. ") + message +
                (geometry.Empty ? " No geometry; Clear removes the last layer." :
                automatic ? " Prepared; waiting for a successful settled solution." : " Prepared; press Show to publish."));
            access.AddRemark("Overlay prepared", geometry.Empty ? "No geometry. Press Clear to remove the previous layer." :
                $"{geometry.Lines.Length} curves, {geometry.Meshes.Length} meshes, {geometry.Points.Length} point sets. " +
                (automatic ? "Auto sync enabled." : "Press Show to publish this solution."));
        }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException)
        {
            access.SetItem(0, "Not ready: " + error.Message + " Previous published geometry, if any, is unchanged.");
            access.AddWarning("Overlay not ready", error.Message);
        }
    }

    internal void Show()
    {
        Batch? batch = Volatile.Read(ref prepared);
        if (Volatile.Read(ref busy) != 0)
        {
            RhinoApp.WriteLine("Tapioca overlay: another display action is pending.");
            return;
        }
        if (batch is null || batch.Geometry.Empty || Document is null || State.Phase != Phase.Completed ||
            State.CurrentId != batch.Solution || !batch.Lease.Current)
        {
            PostResult("Show refused: attach and solve one valid, non-empty geometry batch first. See component warnings.",
                Interlocked.Read(ref lifetime));
            return;
        }
        if (Interlocked.CompareExchange(ref busy, 1, 0) != 0) return;
        shownLease = batch.Lease; // retain even after an uncertain response, so Clear remains available
        attemptedBatch = batch;
        long ticket = Interlocked.Read(ref lifetime);
        long producerTicket = Interlocked.Read(ref producerLifetime);
        Guid actionDocument = documentSession;
        _ = Task.Run(async () =>
        {
            string result;
            try
            {
                result = await ArchicadOverlayBridge.ShowAsync(batch.Lease, batch.Payload, batch.Producer, Plan,
                    () => ticket == Interlocked.Read(ref lifetime) && ReferenceEquals(Volatile.Read(ref prepared), batch));
                Volatile.Write(ref publishedBatch, new Published(batch, ticket));
            }
            catch (Exception error) { result = error.Message; }
            finally
            {
                if (producerTicket != Interlocked.Read(ref producerLifetime))
                    await ReleaseAsync(batch.Lease, batch.Producer, actionDocument);
                Interlocked.Exchange(ref busy, 0);
            }
            PostResult(result, ticket);
        });
    }

    internal void Clear()
    {
        ArchicadOverlayBridge.Lease? lease = shownLease;
        if (lease is null)
        {
            PostResult("No published layer belongs to this component.", Interlocked.Read(ref lifetime));
            return;
        }
        long ticket = Interlocked.Increment(ref lifetime); // invalidate an outstanding Show
        Volatile.Write(ref publishedBatch, null);
        Volatile.Write(ref prepared, null);
        long clearRevision = Interlocked.Increment(ref revision);
        Guid actionDocument = documentSession;
        Guid actionProducer = producer;
        _ = Task.Run(async () =>
        {
            string result;
            try
            {
                await ArchicadOverlayBridge.ClearAsync(lease, actionProducer, actionDocument, clearRevision);
                result = "This component's layer was cleared; other layers and overlay switches are unchanged.";
            }
            catch (Exception error) { result = error.Message; }
            PostResult(result, ticket);
        });
    }

    private void PostResult(string result, long ticket)
    {
        Application? app = Application.Instance;
        if (app is null) return;
        try { app.AsyncInvoke((System.Action)(() =>
        {
            if (Document is null || ticket != Interlocked.Read(ref lifetime)) return;
            message = result;
            // Acknowledgements must not trigger another automatic publication.
            if (automatic)
            {
                RhinoApp.WriteLine("Tapioca overlay: " + result);
                return;
            }
            Expire();
            Document.Solution.Start(); // status recompute only; Process has no publishing side effects
        })); }
        catch (ObjectDisposedException) { }
        catch (InvalidOperationException) { }
    }

    private void OnDocumentChanged(object? sender, EventArgs args)
    {
        if (boundDocument is not null)
        {
            boundDocument.StateChanged -= OnDocumentStateChanged;
            boundDocument.Solution.SolutionStarted -= OnSolutionStarted;
            boundDocument.Solution.SolutionCompleted -= OnSolutionCompleted;
        }
        Release();
        boundDocument = Document;
        documentSession = boundDocument is null ? Guid.Empty : documents.GetOrCreateValue(boundDocument).Id;
        if (boundDocument is not null)
        {
            boundDocument.StateChanged += OnDocumentStateChanged;
            boundDocument.Solution.SolutionStarted += OnSolutionStarted;
            boundDocument.Solution.SolutionCompleted += OnSolutionCompleted;
        }
        if (boundDocument is not null && Application.Instance is not null)
        {
            autoTimer = new UITimer { Interval = 0.075 };
            autoTimer.Elapsed += (_, _) => PublishSettled();
            autoTimer.Start();
        }
    }

    private void PublishSettled()
    {
        Batch? batch = Volatile.Read(ref prepared);
        if (!automatic || batch is null || !ReferenceEquals(batch, Volatile.Read(ref completedBatch)) ||
            ReferenceEquals(batch, attemptedBatch) ||
            Volatile.Read(ref busy) != 0 || Document is null || State.Phase != Phase.Completed ||
            State.CurrentId != batch.Solution || !batch.Lease.Current)
            return;
        // One latest pending batch, after a quiet interval. No solver or pipe
        // work runs on this UI timer; Show dispatches the immutable payload.
        if (!ReferenceEquals(batch, observedBatch)) { observedBatch = batch; return; }
        attemptedBatch = batch;
        Published? published = Volatile.Read(ref publishedBatch);
        if (published is not null && published.Lifetime == Interlocked.Read(ref lifetime) &&
            published.Batch.Lease == batch.Lease && published.Batch.Producer == batch.Producer &&
            published.Batch.Fingerprint == batch.Fingerprint) return;
        // An empty upstream query may be awaiting its asynchronous snapshot.
        // Preserve the last good preview; explicit Clear always remains available.
        if (!batch.Geometry.Empty) Show();
    }

    private void OnSolutionStarted(object? sender, SolutionEventArgs args) =>
        Volatile.Write(ref completedBatch, null);

    private void OnSolutionCompleted(object? sender, SolutionEventArgs args)
    {
        Batch? batch = Volatile.Read(ref prepared);
        if (batch is not null && batch.Solution == args.SolutionId)
            Volatile.Write(ref completedBatch, batch);
    }

    private void OnDocumentStateChanged(object? sender, EventArgs args)
    {
        if (boundDocument?.State is DocumentState.Closing or DocumentState.Closed) Release();
    }

    private void Release()
    {
        autoTimer?.Stop();
        autoTimer?.Dispose();
        autoTimer = null;
        observedBatch = attemptedBatch = null;
        Volatile.Write(ref completedBatch, null);
        Interlocked.Increment(ref producerLifetime);
        Volatile.Write(ref publishedBatch, null);
        Interlocked.Increment(ref lifetime);
        Volatile.Write(ref prepared, null);
        ArchicadOverlayBridge.Lease? lease = shownLease;
        shownLease = null;
        Guid previousProducer = producer;
        Guid previousDocument = documentSession;
        producer = Guid.NewGuid(); // late cleanup must not clear an undone/re-added instance's new layer
        if (lease is not null) _ = Task.Run(() => ReleaseAsync(lease, previousProducer, previousDocument));
    }

    private static async Task ReleaseAsync(ArchicadOverlayBridge.Lease lease, Guid producer, Guid document)
    {
        try { await ArchicadOverlayBridge.ClearAsync(lease, producer, document, long.MaxValue); }
        // Cleanup can finish after RhinoCore teardown. Never call Rhino UI/native
        // APIs from this continuation; the native bridge also releases the session.
        catch (Exception error) { System.Diagnostics.Trace.WriteLine("Tapioca overlay cleanup: " + error.Message); }
    }
}
