using System.Text.Json;
using Eto.Forms;
using Grasshopper2.Components;
using Grasshopper2.Doc;
using Grasshopper2.Parameters;
using Grasshopper2.UI;
using GrasshopperIO;
using Rhino;
using Rhino.Geometry;

namespace TapiocaGH2;

// The solver only prepares a bounded immutable request. Archicad is changed
// solely by a deliberate canvas click, never by document load or a GH2 solve.
[IoId("f35e5e29-e424-40c9-8493-34e682e9a22b")]
public sealed class CreateArchicadTerrainMesh : Component
{
    internal sealed record Prepared(double[] Outline, double[] PolyZ, double BaseLevel,
        double[] RidgeCoords, int[] RidgeCounts, string Layer, string Project);

    private Prepared? prepared;
    private string message = "Connect to Archicad, then press Create Mesh.";
    private int busy;

    public CreateArchicadTerrainMesh() : base(new Nomen("CreateArchicadTerrainMesh",
        "Create a new Archicad terrain mesh only when the canvas button is pressed; never on solve.",
         "Tapioca", "Design")) { ArchicadPresetBindings.Register(this); }
    public CreateArchicadTerrainMesh(IReader reader) : base(reader) { ArchicadPresetBindings.Register(this); }

    protected override Grasshopper2.Doc.IAttributes CreateAttributes() => new CreateArchicadTerrainMeshAttributes(this, Create);

    protected override void AddInputs(InputAdder inputs)
    {
        inputs.AddCurve("Outline", "C", "Closed Rhino polyline; XY boundary and vertex elevations. Unwired: outline around ridges or a 10×10 m test square.");
        inputs.AddCurve("Ridges", "R", "Open Rhino polylines with XYZ vertices; each becomes one Archicad mesh level line.", Access.Twig);
        inputs.AddText("Layer", "L", "Optional Archicad layer; blank uses Mesh tool defaults.").Set("");
    }

    protected override void AddOutputs(OutputAdder outputs) { }

    protected override void Process(IDataAccess access)
    {
        access.GetItem(0, out Curve? outline);
        access.GetPears<Curve>(1, out var ridgePears);
        access.GetItem(2, out string? layer);
        string? project = ArchicadProjectOptions.CurrentIdentity();
        try
        {
            prepared = project is null ? null : Prepare(outline,
                ridgePears?.Select(pear => pear.Item).ToArray() ?? [], layer ?? "", project,
                RhinoMath.UnitScale(RhinoDoc.ActiveDoc?.ModelUnitSystem ?? UnitSystem.Meters, UnitSystem.Meters));
            if (prepared is not null)
            {
                var stories = ArchicadProjectOptions.Read().Stories;
                if (!stories.Any(story => story.Index == 0))
                    throw new InvalidOperationException("Archicad story 0 is unavailable; refresh project choices before creating a terrain mesh.");
                prepared = prepared with { BaseLevel = prepared.BaseLevel -
                    stories.First(story => story.Index == 0).Level };
            }
            if (prepared is not null && Volatile.Read(ref busy) == 0)
                message = $"Ready: {prepared.Outline.Length / 2} boundary vertices, {prepared.RidgeCounts.Length} ridge polylines. Press Create Mesh.";
        }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException)
        {
            prepared = null;
            access.AddWarning("Mesh input", error.Message);
            message = error.Message;
        }
        if (prepared is null) access.AddWarning("Mesh not ready", "Attach to Archicad and provide valid polylines.");
        else access.AddRemark("Mesh ready", message);
    }

    internal static Prepared Prepare(Curve? outline, Curve[] ridgeLines, string layer, string project, double scale)
    {
        if (layer.Length > 128 || !double.IsFinite(scale) || scale <= 0 || ridgeLines.Length > 128 ||
            ridgeLines.Any(curve => curve is null || !curve.IsValid))
            throw new ArgumentException("Layer, document units or ridge polylines are invalid (limit 128 ridges).");
        var lines = ridgeLines.Select(curve =>
        {
            if (!curve.TryGetPolyline(out Polyline polyline) || polyline.Count < 2 || polyline.Count > 257)
                throw new ArgumentException("Each ridge must be a polyline of 2–256 distinct vertices.");
            Point3d[] vertices = polyline.ToArray();
            if (curve.IsClosed && vertices[0].DistanceTo(vertices[^1]) < 1e-9)
                vertices = vertices[..^1];
            if (vertices.Length is < 2 or > 256 || vertices.Any(point => !point.IsValid) ||
                vertices.Zip(vertices.Skip(1)).Any(pair => pair.First.DistanceTo(pair.Second) < 1e-9))
                throw new ArgumentException("Ridge vertices must be finite and distinct.");
            return vertices;
        }).ToArray();
        if (lines.Sum(line => line.Length) > 512)
            throw new ArgumentException("Ridges exceed 512 vertices in total.");
        Point3d[] vertices;
        bool preset = outline is null && lines.Length == 0;
        if (outline is null)
        {
            if (preset)
                vertices = [new(0, 0, 0), new(10 / scale, 0, 0),
                    new(10 / scale, 10 / scale, 0), new(0, 10 / scale, 0)];
            else
            {
                var points = lines.SelectMany(line => line).ToArray();
                double minX = points.Min(p => p.X) - 1 / scale, maxX = points.Max(p => p.X) + 1 / scale;
                double minY = points.Min(p => p.Y) - 1 / scale, maxY = points.Max(p => p.Y) + 1 / scale;
                double minZ = points.Min(p => p.Z);
                vertices = [new(minX, minY, minZ), new(maxX, minY, minZ),
                    new(maxX, maxY, minZ), new(minX, maxY, minZ)];
            }
        }
        else
        {
            if (!outline.IsClosed || !outline.TryGetPolyline(out Polyline polyline))
                throw new ArgumentException("Outline must be a closed, non-curved polyline.");
            vertices = polyline.ToArray();
            if (vertices.Length > 1 && vertices[0].DistanceTo(vertices[^1]) < 1e-9)
                vertices = vertices[..^1];
        }
        if (vertices.Length is < 3 or > 256 || vertices.Any(point => !point.IsValid))
            throw new ArgumentException("Outline must contain 3–256 distinct, finite vertices.");
        double area2 = 0;
        for (int i = 0; i < vertices.Length; i++)
        {
            Point3d a = vertices[i], b = vertices[(i + 1) % vertices.Length];
            area2 += a.X * b.Y - b.X * a.Y;
        }
        if (Math.Abs(area2 * scale * scale) < 1e-9)
            throw new ArgumentException("The mesh outline has no XY area.");
        double baseLevel = Math.Min(vertices.Min(p => p.Z),
            lines.Length == 0 ? vertices.Min(p => p.Z) : lines.SelectMany(line => line).Min(p => p.Z)) * scale;
        if (preset) lines = [[new Point3d(2 / scale, 5 / scale, 0),
            new Point3d(5 / scale, 5 / scale, 2 / scale), new Point3d(8 / scale, 5 / scale, 0)]];
        var boundary = vertices.SelectMany(p => new[] { p.X * scale, p.Y * scale }).ToArray();
        var heights = vertices.Select(p => p.Z * scale - baseLevel).ToArray();
        var ridges = lines.SelectMany(line => line).SelectMany(p =>
            new[] { p.X * scale, p.Y * scale, p.Z * scale - baseLevel }).ToArray();
        return new Prepared(boundary, heights, baseLevel, ridges, lines.Select(line => line.Length).ToArray(), layer, project);
    }

    internal void Create()
    {
        Prepared? request = prepared;
        if (request is null || Document is null || State.Phase != Phase.Completed ||
            ArchicadProjectOptions.CurrentIdentity() != request.Project ||
            Interlocked.CompareExchange(ref busy, 1, 0) != 0) return;
        if (MessageBox.Show("Create a NEW Archicad terrain mesh? This action is not repeated by GH2 solves.",
                "Tapioca", MessageBoxButtons.YesNo) != DialogResult.Yes)
        {
            Interlocked.Exchange(ref busy, 0);
            return;
        }
        message = "Creating mesh in Archicad...";
        Expire();
        Document?.Solution.Start();
        _ = CreateAsync(request);
    }

    private async Task CreateAsync(Prepared request)
    {
        string outcome;
        try
        {
            AcElementRef result = await CommitAsync(request);
            outcome = $"Created Archicad mesh {result.ElementId:D}.";
        }
        catch (Exception error) { outcome = error.Message; }
        finally { Interlocked.Exchange(ref busy, 0); }
        Application? app = Application.Instance;
        if (app is null) return;
        try { app.AsyncInvoke((System.Action)(() =>
        {
            if (Document is null) return;
            message = outcome;
            Expire();
            Document.Solution.Start();
        })); }
        catch (ObjectDisposedException) { }
        catch (InvalidOperationException) { }
    }

    internal static async Task<AcElementRef> CommitAsync(Prepared request)
    {
        if (ArchicadProjectOptions.ParseIdentity(await Gh2ConnectionStatus.ReadProjectInfoAsync()) != request.Project)
            throw new InvalidOperationException("Archicad changed projects; no mesh was created.");
        string response = await Gh2ConnectionStatus.CreateTerrainMeshAsync(request.Outline, request.PolyZ,
            request.BaseLevel, request.RidgeCoords, request.RidgeCounts, request.Layer);
        JsonElement data = ArchicadSelectionInput.ValidateReply(response, "terrain mesh bake");
        Guid id = data.GetProperty("elementId").GetProperty("guid").GetGuid();
        if (id == Guid.Empty) throw new InvalidDataException("Archicad returned no mesh GUID.");
        if (ArchicadProjectOptions.ParseIdentity(await Gh2ConnectionStatus.ReadProjectInfoAsync()) != request.Project)
            throw new InvalidOperationException($"Mesh {id:D} may have been created, but Archicad changed projects. Inspect both projects before retrying.");
        Gh2ConnectionStatus.RequestRefresh();
        return new AcElementRef(request.Project, id);
    }
}
