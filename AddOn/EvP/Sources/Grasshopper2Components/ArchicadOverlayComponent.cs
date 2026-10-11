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
        SolutionId Solution, Guid Producer, string Payload);
    private Guid producer = Guid.NewGuid(); // runtime only: copies/reopens never share a layer
    private Batch? prepared;
    private ArchicadOverlayBridge.Lease? shownLease;
    private Document? boundDocument;
    private string message = "Solve to prepare geometry, then press Show.";
    private int busy;
    private long lifetime;

    protected abstract bool Plan { get; }
    protected ArchicadOverlayComponent(Nomen nomen) : base(nomen) => DocumentChanged += OnDocumentChanged;
    protected ArchicadOverlayComponent(IReader reader) : base(reader) => DocumentChanged += OnDocumentChanged;

    protected override IAttributes CreateAttributes() => new ArchicadOverlayAttributes(this, Show, Clear);

    protected override void AddInputs(InputAdder inputs)
    {
        inputs.AddCurve("Curves", "C", "Preview curves; non-polylines are sampled into 128 segments.", Access.Twig, Requirement.MayBeMissing);
        inputs.AddMesh("Meshes", "M", "Preview meshes; quads are triangulated without modifying the source.", Access.Twig, Requirement.MayBeMissing);
        inputs.AddPoint("Points", "P", "Preview point markers.", Access.Twig, Requirement.MayBeMissing);
        inputs.AddColour("Tint", "T", "RGBA preview colour; accepts Preview Tint.").Set(Colors.CornflowerBlue);
        inputs.AddNumber("Width", "W", "Curve line width in pixels, greater than zero and at most 64.").Set(2.0);
    }

    protected override void AddOutputs(OutputAdder outputs) =>
        outputs.AddText("Status", "S", "Prepared/published status. Published is not a claim that pixels are visible.");

    protected override void Process(IDataAccess access)
    {
        Volatile.Write(ref prepared, null);
        access.SetItem(0, message);
        if (access.Iterations != 1)
        {
            access.AddWarning("Overlay branches", "Use one iteration: flatten geometry inputs and supply one Tint/Width. No partial branch will be published.");
            return;
        }
        if (!access.GetItem(3, out GhColour tint) || !access.GetItem(4, out double width)) return;
        access.GetPears<Curve>(0, out var curves);
        access.GetPears<Mesh>(1, out var meshes);
        access.GetPears<Point3d>(2, out var points);
        try
        {
            ArchicadOverlayBridge.Lease lease = ArchicadOverlayBridge.Capture() ??
                throw new InvalidOperationException("Attach to Archicad, then solve again before showing geometry.");
            Color colour = tint.ToEto();
            if (!float.IsFinite(colour.R) || !float.IsFinite(colour.G) || !float.IsFinite(colour.B) || !float.IsFinite(colour.A))
                throw new ArgumentException("Tint channels must be finite.");
            static int Byte(float value) => (int)Math.Round(Math.Clamp(value, 0, 1) * 255);
            string rgba = $"{Byte(colour.R):X2}{Byte(colour.G):X2}{Byte(colour.B):X2}{Byte(colour.A):X2}";
            var geometry = ArchicadOverlayGeometry.Prepare(curves?.Select(pear => pear.Item).ToArray() ?? [],
                meshes?.Select(pear => pear.Item).ToArray() ?? [], points?.Select(pear => pear.Item).ToArray() ?? [],
                Plan, RhinoMath.UnitScale(RhinoDoc.ActiveDoc?.ModelUnitSystem ?? UnitSystem.Meters, UnitSystem.Meters), rgba, width);
            Guid batchProducer = producer;
            string payload = Gh2OverlayPayload.Encode(lease.Generation, batchProducer, Plan, geometry.Lines, geometry.Points, geometry.Meshes);
            Volatile.Write(ref prepared, new(geometry, lease, access.Solution.Id, batchProducer, payload));
            access.SetItem(0, message + (geometry.Empty ? " No geometry; Clear removes the last layer." :
                " Current solution prepared; Show publishes it. Solving alone leaves the last layer unchanged."));
            access.AddRemark("Overlay prepared", geometry.Empty ? "No geometry. Press Clear to remove the previous layer." :
                $"{geometry.Lines.Length} curves, {geometry.Meshes.Length} meshes, {points?.Length ?? 0} points. Press Show to publish this solution.");
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
        long ticket = Interlocked.Read(ref lifetime);
        _ = Task.Run(async () =>
        {
            string result;
            try
            {
                result = await ArchicadOverlayBridge.ShowAsync(batch.Lease, batch.Payload, batch.Producer, Plan,
                    () => ticket == Interlocked.Read(ref lifetime) && ReferenceEquals(Volatile.Read(ref prepared), batch));
            }
            catch (Exception error) { result = error.Message; }
            finally
            {
                if (ticket != Interlocked.Read(ref lifetime))
                    await ReleaseAsync(batch.Lease, batch.Producer);
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
        if (Interlocked.CompareExchange(ref busy, 1, 0) != 0)
        {
            RhinoApp.WriteLine("Tapioca overlay: another display action is pending.");
            return;
        }
        long ticket = Interlocked.Read(ref lifetime);
        Guid actionProducer = producer;
        _ = Task.Run(async () =>
        {
            string result;
            try
            {
                await ArchicadOverlayBridge.ClearAsync(lease, actionProducer);
                result = "This component's layer was cleared; other layers and overlay switches are unchanged.";
            }
            catch (Exception error) { result = error.Message; }
            finally { Interlocked.Exchange(ref busy, 0); }
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
            Expire();
            Document.Solution.Start(); // status recompute only; Process has no publishing side effects
        })); }
        catch (ObjectDisposedException) { }
        catch (InvalidOperationException) { }
    }

    private void OnDocumentChanged(object? sender, EventArgs args)
    {
        if (boundDocument is not null) boundDocument.StateChanged -= OnDocumentStateChanged;
        Release();
        boundDocument = Document;
        if (boundDocument is not null) boundDocument.StateChanged += OnDocumentStateChanged;
    }

    private void OnDocumentStateChanged(object? sender, EventArgs args)
    {
        if (boundDocument?.State is DocumentState.Closing or DocumentState.Closed) Release();
    }

    private void Release()
    {
        Interlocked.Increment(ref lifetime);
        Volatile.Write(ref prepared, null);
        ArchicadOverlayBridge.Lease? lease = shownLease;
        shownLease = null;
        Guid previousProducer = producer;
        producer = Guid.NewGuid(); // late cleanup must not clear an undone/re-added instance's new layer
        if (lease is not null) _ = Task.Run(() => ReleaseAsync(lease, previousProducer));
    }

    private static async Task ReleaseAsync(ArchicadOverlayBridge.Lease lease, Guid producer)
    {
        try { await ArchicadOverlayBridge.ClearAsync(lease, producer); }
        // Cleanup can finish after RhinoCore teardown. Never call Rhino UI/native
        // APIs from this continuation; the native bridge also releases the session.
        catch (Exception error) { System.Diagnostics.Trace.WriteLine("Tapioca overlay cleanup: " + error.Message); }
    }
}
