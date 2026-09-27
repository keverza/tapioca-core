using System.Text.Json;
using Eto.Forms;
using Grasshopper2.Components;
using Grasshopper2.Data;
using Grasshopper2.Doc.Attributes;
using Grasshopper2.Parameters;
using Grasshopper2.UI;
using GrasshopperIO;
using Rhino;
using Rhino.Geometry;

namespace TapiocaGH2;

// The solver only prepares a bounded immutable request. Archicad is changed
// solely by a deliberate canvas click, never by document load or a GH2 solve.
[IoId("ee94cef3-7073-4a37-a338-24e3a5e02d80")]
public sealed class ArchicadBakeTerrainMesh : Component
{
    internal sealed record Prepared(double[] Outline, double[] PolyZ, double BaseLevel,
        double[] RidgeCoords, int[] RidgeCounts, string Layer, string Project);

    private Prepared? prepared;
    private AcElementRef? created;
    private string message = "Connect to Archicad, then press Create Mesh.";
    private int busy;

    public ArchicadBakeTerrainMesh() : base(new Nomen("Create Archicad Terrain Mesh",
        "Create a new Archicad terrain mesh only when the canvas button is pressed; never on solve.",
        "Tapioca", "Archicad")) { ArchicadPresetBindings.Register(this); }
    public ArchicadBakeTerrainMesh(IReader reader) : base(reader) { ArchicadPresetBindings.Register(this); }

    protected override Grasshopper2.Doc.IAttributes CreateAttributes() => new ArchicadBakeTerrainMeshAttributes(this);

    protected override void AddInputs(InputAdder inputs)
    {
        inputs.AddCurve("Outline", "C", "Closed Rhino polyline; XY boundary and vertex elevations. Unwired: outline around points or a 10×10 m test square.");
        inputs.AddPoint("Terrain Points", "P", "Optional XYZ terrain points; each becomes a single Archicad mesh level point.", Access.Twig);
        inputs.AddText("Layer", "L", "Optional Archicad layer; blank uses Mesh tool defaults.").Set("");
    }

    protected override void AddOutputs(OutputAdder outputs)
    {
        outputs.Add(new AcElementParameter("Created Element", "E", "Reference to the last successfully created mesh.", Access.Item));
        outputs.AddText("Action", "A", "Preparation, success or error; a solve never creates an element.");
        outputs.AddCurve("Prepared Outline", "C", "Preview of the boundary that will be created in Archicad.");
    }

    protected override void Process(IDataAccess access)
    {
        access.GetItem(0, out Curve? outline);
        access.GetPears<Point3d>(1, out var pointPears);
        access.GetItem(2, out string? layer);
        string? project = ArchicadProjectOptions.CurrentIdentity();
        try
        {
            prepared = project is null ? null : Prepare(outline,
                pointPears?.Select(pear => pear.Item).ToArray() ?? [], layer ?? "", project,
                RhinoMath.UnitScale(RhinoDoc.ActiveDoc?.ModelUnitSystem ?? UnitSystem.Meters, UnitSystem.Meters));
            if (prepared is not null)
            {
                var stories = ArchicadProjectOptions.Read().Stories;
                if (!stories.Any(story => story.Index == 0))
                    throw new InvalidOperationException("Archicad story 0 is unavailable; refresh project choices before creating a terrain mesh.");
                prepared = prepared with { BaseLevel = prepared.BaseLevel -
                    stories.First(story => story.Index == 0).Level };
            }
            if (prepared is not null && Volatile.Read(ref busy) == 0 && created is null)
                message = $"Ready: {prepared.Outline.Length / 2} boundary vertices, {prepared.RidgeCounts.Length} terrain points. Press Create Mesh.";
        }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException)
        {
            prepared = null;
            access.AddWarning("Mesh input", error.Message);
            message = error.Message;
        }
        if (created is not null && created.ProjectKey == project)
            access.SetItem(0, created);
        access.SetItem(1, message);
        if (prepared is not null)
        {
            double units = RhinoMath.UnitScale(UnitSystem.Meters,
                RhinoDoc.ActiveDoc?.ModelUnitSystem ?? UnitSystem.Meters);
            double storyZero = ArchicadProjectOptions.Read().Stories.First(story => story.Index == 0).Level;
            var vertices = Enumerable.Range(0, prepared.PolyZ.Length).Select(index => new Point3d(
                prepared.Outline[2 * index] * units, prepared.Outline[2 * index + 1] * units,
                (prepared.BaseLevel + storyZero + prepared.PolyZ[index]) * units)).ToList();
            vertices.Add(vertices[0]);
            access.SetItem(2, new PolylineCurve(vertices));
        }
    }

    internal static Prepared Prepare(Curve? outline, Point3d[] points, string layer, string project, double scale)
    {
        if (layer.Length > 128 || !double.IsFinite(scale) || scale <= 0 || points.Length > 512 ||
            points.Any(point => !point.IsValid))
            throw new ArgumentException("Layer, document units or terrain points are invalid (limit 512 points).");
        Point3d[] vertices;
        bool preset = outline is null && points.Length == 0;
        if (outline is null)
        {
            if (preset)
                vertices = [new(0, 0, 0), new(10 / scale, 0, 0),
                    new(10 / scale, 10 / scale, 0), new(0, 10 / scale, 0)];
            else
            {
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
            points.Length == 0 ? vertices.Min(p => p.Z) : points.Min(p => p.Z)) * scale;
        if (preset) points = [new Point3d(5 / scale, 5 / scale, 2 / scale)];
        var boundary = vertices.SelectMany(p => new[] { p.X * scale, p.Y * scale }).ToArray();
        var heights = vertices.Select(p => p.Z * scale - baseLevel).ToArray();
        var ridges = points.SelectMany(p => new[] { p.X * scale, p.Y * scale, p.Z * scale - baseLevel }).ToArray();
        return new Prepared(boundary, heights, baseLevel, ridges, Enumerable.Repeat(1, points.Length).ToArray(), layer, project);
    }

    internal void Create()
    {
        Prepared? request = prepared;
        if (request is null || Interlocked.CompareExchange(ref busy, 1, 0) != 0) return;
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
        AcElementRef? result = null;
        string outcome;
        try
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
            result = new AcElementRef(request.Project, id);
            outcome = $"Created Archicad mesh {id:D}.";
            Gh2ConnectionStatus.RequestRefresh();
        }
        catch (Exception error) { outcome = error.Message; }
        finally { Interlocked.Exchange(ref busy, 0); }
        Application? app = Application.Instance;
        if (app is null) return;
        try { app.AsyncInvoke((System.Action)(() =>
        {
            if (Document is null) return;
            message = outcome;
            created = result;
            Expire();
            Document.Solution.Start();
        })); }
        catch (ObjectDisposedException) { }
        catch (InvalidOperationException) { }
    }
}
