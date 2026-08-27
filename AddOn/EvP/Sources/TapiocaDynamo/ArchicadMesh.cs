using Autodesk.DesignScript.Interfaces;

namespace Tapioca;

public sealed class ArchicadMesh : IGraphicItem
{
    private readonly double[] vertices;
    private readonly int[] triangles;

    internal ArchicadMesh(string elementId, int bodyIndex, string elementType, double[] vertices, int[] triangles)
    {
        ElementId = elementId;
        BodyIndex = bodyIndex;
        ElementType = elementType;
        this.vertices = (double[])vertices.Clone();
        this.triangles = (int[])triangles.Clone();
    }

    public string ElementId { get; }
    public int BodyIndex { get; }
    public string ElementType { get; }
    public IReadOnlyList<double> Vertices => vertices;
    public IReadOnlyList<int> Triangles => triangles;
    public int VertexCount => vertices.Length / 3;
    public int TriangleCount => triangles.Length / 3;

    internal ArchicadMesh Translate(double dx, double dy, double dz)
    {
        double[] moved = (double[])vertices.Clone();
        for (int index = 0; index < moved.Length; index += 3)
        {
            moved[index] += dx;
            moved[index + 1] += dy;
            moved[index + 2] += dz;
        }
        return new ArchicadMesh(ElementId, BodyIndex, ElementType, moved, triangles);
    }

    public void Tessellate(IRenderPackage package, TessellationParameters parameters)
    {
        _ = parameters;
        for (int index = 0; index < triangles.Length; index += 3)
        {
            int a = triangles[index] * 3;
            int b = triangles[index + 1] * 3;
            int c = triangles[index + 2] * 3;
            double abx = vertices[b] - vertices[a];
            double aby = vertices[b + 1] - vertices[a + 1];
            double abz = vertices[b + 2] - vertices[a + 2];
            double acx = vertices[c] - vertices[a];
            double acy = vertices[c + 1] - vertices[a + 1];
            double acz = vertices[c + 2] - vertices[a + 2];
            double nx = aby * acz - abz * acy;
            double ny = abz * acx - abx * acz;
            double nz = abx * acy - aby * acx;
            double length = Math.Sqrt(nx * nx + ny * ny + nz * nz);
            if (length > 0)
            {
                nx /= length;
                ny /= length;
                nz /= length;
            }

            AddVertex(package, a, nx, ny, nz);
            AddVertex(package, b, nx, ny, nz);
            AddVertex(package, c, nx, ny, nz);
        }
    }

    public override string ToString() =>
        $"ArchicadMesh({ElementType}, {ElementId}, body {BodyIndex}, {VertexCount} vertices, {TriangleCount} triangles)";

    private void AddVertex(IRenderPackage package, int coordinateIndex, double nx, double ny, double nz)
    {
        package.AddTriangleVertex(vertices[coordinateIndex], vertices[coordinateIndex + 1], vertices[coordinateIndex + 2]);
        package.AddTriangleVertexNormal(nx, ny, nz);
        package.AddTriangleVertexColor(80, 170, 220, 255);
    }
}
