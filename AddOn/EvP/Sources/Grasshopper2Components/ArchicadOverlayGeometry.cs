using Rhino.Geometry;
using Tapioca.Gh2Worker;

namespace TapiocaGH2;

// BOUND BY OVERLAY-INVARIANTS.md: geometry conversion only. Plan deliberately
// projects supplied geometry onto XY, never cuts a 3D body or borrows its camera.
internal static class ArchicadOverlayGeometry
{
    internal sealed record Prepared(Gh2OverlayPayload.Line[] Lines, Gh2OverlayPayload.PointSet[] Points,
        Gh2OverlayPayload.Mesh[] Meshes)
    {
        internal bool Empty => Lines.Length + Points.Length + Meshes.Length == 0;
    }

    internal static Prepared PrepareMixed(object[] items, bool plan, double scale, string tint, double width)
    {
        var curves = new List<Curve>();
        var meshes = new List<Mesh>();
        var points = new List<Point3d>();
        var owned = new List<IDisposable>();
        try
        {
            if (items.Length > 20000) throw new ArgumentException("Overlay input exceeds 20,000 items.");
            void AddBrep(Brep brep)
            {
                Mesh[] parts = Mesh.CreateFromBrep(brep, MeshingParameters.FastRenderMesh) ?? [];
                if (parts.Length == 0) throw new ArgumentException("Could not tessellate overlay surface.");
                owned.AddRange(parts); meshes.AddRange(parts);
            }
            foreach (object item in items)
            {
                switch (item)
                {
                    case Point3d point: points.Add(point); break;
                    case Point point: points.Add(point.Location); break;
                    case Curve curve: curves.Add(curve); break;
                    case Mesh mesh: meshes.Add(mesh); break;
                    case Brep brep: AddBrep(brep); break;
                    case Sphere sphere:
                        using (Brep brep = sphere.ToBrep()) AddBrep(brep);
                        break;
                    case Box box:
                        using (Brep brep = box.ToBrep()) AddBrep(brep);
                        break;
                    case BoundingBox box:
                        using (Brep brep = Brep.CreateFromBox(box)) AddBrep(brep);
                        break;
                    case Polyline polyline:
                        var polylineCurve = new PolylineCurve(polyline); owned.Add(polylineCurve); curves.Add(polylineCurve); break;
                    case Rectangle3d rectangle:
                        var rectangleCurve = rectangle.ToNurbsCurve(); owned.Add(rectangleCurve); curves.Add(rectangleCurve); break;
                    case Surface surface:
                        using (Brep brep = surface.ToBrep()) AddBrep(brep);
                        break;
                    case SubD subd:
                        using (Brep brep = subd.ToBrep()) AddBrep(brep);
                        break;
                    case Line line:
                        var lineCurve = new LineCurve(line); owned.Add(lineCurve); curves.Add(lineCurve); break;
                    case Arc arc:
                        var arcCurve = new ArcCurve(arc); owned.Add(arcCurve); curves.Add(arcCurve); break;
                    case Circle circle:
                        var circleCurve = new ArcCurve(circle); owned.Add(circleCurve); curves.Add(circleCurve); break;
                    default: throw new ArgumentException($"Unsupported overlay geometry: {item?.GetType().Name ?? "null"}.");
                }
            }
            return Prepare(curves.ToArray(), meshes.ToArray(), points.ToArray(), plan, scale, tint, width);
        }
        finally { foreach (IDisposable item in owned) item.Dispose(); }
    }

    internal static Prepared Prepare(Curve[] curves, Mesh[] meshes, Point3d[] points,
        bool plan, double scale, string tint, double width)
    {
        if (!double.IsFinite(scale) || scale <= 0 || !double.IsFinite(width) || width is <= 0 or > 64)
            throw new ArgumentException("Document units or line width are invalid (width: 0–64 pixels, excluding zero).");
        if (curves.Length > 256 || meshes.Length > 64 || points.Length > 20000)
            throw new ArgumentException("Overlay limit: 256 curves, 64 meshes and 20,000 points.");
        double[] Pack(IEnumerable<Point3d> vertices) => vertices.SelectMany(point =>
        {
            if (!point.IsValid) throw new ArgumentException("Overlay contains an invalid point.");
            double[] xyz = [point.X * scale, point.Y * scale, plan ? 0 : point.Z * scale];
            if (!xyz.All(double.IsFinite)) throw new ArgumentException("Scaled overlay coordinates must be finite.");
            return xyz;
        }).ToArray();

        var lines = new List<Gh2OverlayPayload.Line>();
        int linePoints = 0;
        foreach (Curve curve in curves)
        {
            if (curve is null || !curve.IsValid) throw new ArgumentException("Overlay contains an invalid curve.");
            Point3d[] vertices;
            if (curve.TryGetPolyline(out Polyline polyline)) vertices = polyline.ToArray();
            else
            {
                // Fixed preview-only sampling bounds conversion and IPC work.
                if (curve.DivideByCount(128, true, out vertices) is null)
                    throw new ArgumentException("Could not sample the overlay curve.");
            }
            if (curve.IsClosed && vertices.Length > 2 && vertices[0].DistanceTo(vertices[^1]) < 1e-9 / scale)
                vertices = vertices[..^1];
            linePoints += vertices.Length;
            if (vertices.Length < 2 || linePoints > 20000)
                throw new ArgumentException("Overlay curves exceed 20,000 polyline vertices or contain an empty curve.");
            lines.Add(new(Pack(vertices), curve.IsClosed, tint, width, plan ? "always" : "hide"));
        }

        var surfaces = new List<Gh2OverlayPayload.Mesh>();
        int meshVertices = 0, triangles = 0;
        foreach (Mesh mesh in meshes)
        {
            if (mesh is null || !mesh.IsValid || mesh.Faces.Count == 0)
                throw new ArgumentException("Overlay contains an invalid or empty mesh.");
            meshVertices += mesh.Vertices.Count;
            triangles += mesh.Faces.Sum(face => face.IsQuad ? 2 : 1);
            if (meshVertices > 30000 || triangles > 20000)
                throw new ArgumentException("Overlay meshes exceed 30,000 vertices or 20,000 triangles.");
            var indices = new List<int>();
            foreach (MeshFace face in mesh.Faces)
            {
                indices.AddRange([face.A, face.B, face.C]);
                if (face.IsQuad) indices.AddRange([face.A, face.C, face.D]);
            }
            surfaces.Add(new(Pack(mesh.Vertices.Select(vertex => new Point3d(vertex))), indices.ToArray(), tint,
                new { shading = plan ? "flat" : "lit", opacity = 1.0, occlusion = plan ? "always" : "hide", cull = "none" }));
        }
        Gh2OverlayPayload.PointSet[] markers = points.Length == 0 ? [] : [new(Pack(points), tint, 6.0, 0.1)];
        return new(lines.ToArray(), markers, surfaces.ToArray());
    }
}
