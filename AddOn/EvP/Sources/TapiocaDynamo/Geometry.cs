using Autodesk.DesignScript.Runtime;
using Dynamo.Graph.Nodes;

namespace Tapioca;

public static class Geometry
{
    [NodeCategory("Query")]
    [MultiReturn("geometry", "elementIds", "bodyIndices", "elementTypes")]
    public static Dictionary<string, object> CurrentSelection(bool refresh = false)
    {
        return ByElementIds(Selection.Current(refresh), refresh);
    }

    [NodeCategory("Query")]
    [MultiReturn("geometry", "elementIds", "bodyIndices", "elementTypes")]
    public static Dictionary<string, object> ByElementIds(IEnumerable<string> elementIds, bool refresh = false)
    {
        _ = refresh;
        string[] guids = elementIds
            .Where(guid => !string.IsNullOrWhiteSpace(guid))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();

        var meshes = new List<ArchicadMesh>();
        var meshElementIds = new List<string>();
        var bodyIndices = new List<int>();
        var elementTypes = new List<string>();
        if (guids.Length == 0)
            return Result(meshes, meshElementIds, bodyIndices, elementTypes);

        var modelData = BridgeEnvelope.CallData(
            "Tapioca.GetModelElements",
            new
            {
                elements = guids.Select(ElementRef).ToArray(),
                skipEmpty = true,
                coordinateSystem = "world",
                include = Array.Empty<string>()
            });

        var matchedGuids = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var element in modelData.GetProperty("elements").EnumerateArray())
        {
            string guid = element.GetProperty("elementId").GetProperty("guid").GetString()
                ?? throw new InvalidDataException("Tapioca returned geometry without an element GUID.");
            matchedGuids.Add(guid);
            if (element.TryGetProperty("invalid", out var invalid) && invalid.GetBoolean())
                continue;

            string elementType = element.GetProperty("typeName").GetString() ?? "unknown";
            int bodyCount = element.GetProperty("tessellatedBodyCount").GetInt32();
            for (int bodyIndex = 1; bodyIndex <= bodyCount; bodyIndex++)
            {
                ArchicadMesh mesh = ReadBody(guid, bodyIndex, elementType);
                meshes.Add(mesh);
                meshElementIds.Add(guid);
                bodyIndices.Add(bodyIndex);
                elementTypes.Add(elementType);
            }
        }

        string[] missing = guids.Where(guid => !matchedGuids.Contains(guid)).ToArray();
        if (missing.Length > 0)
            throw new InvalidOperationException(
                $"No directly addressable 3D body was found for: {string.Join(", ", missing)}. " +
                "Composite Archicad elements currently expose model sub-part GUIDs; owner-to-sub-part geometry resolution is an MVP gap.");

        return Result(meshes, meshElementIds, bodyIndices, elementTypes);
    }

    [NodeCategory("Transform")]
    public static ArchicadMesh Translate(ArchicadMesh mesh, double dx, double dy, double dz)
    {
        ArgumentNullException.ThrowIfNull(mesh);
        if (!double.IsFinite(dx) || !double.IsFinite(dy) || !double.IsFinite(dz))
            throw new ArgumentOutOfRangeException(nameof(dx), "Translation components must be finite metres.");
        return mesh.Translate(dx, dy, dz);
    }

    [NodeCategory("Transform")]
    [MultiReturn("dx", "dy", "dz")]
    public static Dictionary<string, object> Translation(
        ArchicadMesh original,
        ArchicadMesh modified,
        double tolerance = 0.000001)
    {
        var translation = TranslationVector(original, modified, tolerance);
        return new Dictionary<string, object>
        {
            ["dx"] = translation.dx,
            ["dy"] = translation.dy,
            ["dz"] = translation.dz
        };
    }

    internal static (double dx, double dy, double dz) TranslationVector(
        ArchicadMesh original,
        ArchicadMesh modified,
        double tolerance = 0.000001)
    {
        ArgumentNullException.ThrowIfNull(original);
        ArgumentNullException.ThrowIfNull(modified);
        if (!double.IsFinite(tolerance) || tolerance <= 0)
            throw new ArgumentOutOfRangeException(nameof(tolerance), "Tolerance must be a positive finite distance in metres.");
        if (original.ElementId != modified.ElementId || original.BodyIndex != modified.BodyIndex ||
            original.Vertices.Count == 0 || original.Vertices.Count != modified.Vertices.Count ||
            !original.Triangles.SequenceEqual(modified.Triangles))
            throw new InvalidOperationException(
                "The modified mesh changed identity or topology. This MVP can push rigid translations only; vertex, face, scale and rotation edits are not yet writable.");

        double dx = modified.Vertices[0] - original.Vertices[0];
        double dy = modified.Vertices[1] - original.Vertices[1];
        double dz = modified.Vertices[2] - original.Vertices[2];
        for (int index = 0; index < original.Vertices.Count; index += 3)
        {
            if (Math.Abs((modified.Vertices[index] - original.Vertices[index]) - dx) > tolerance ||
                Math.Abs((modified.Vertices[index + 1] - original.Vertices[index + 1]) - dy) > tolerance ||
                Math.Abs((modified.Vertices[index + 2] - original.Vertices[index + 2]) - dz) > tolerance)
                throw new InvalidOperationException(
                    "The modified mesh is not a rigid translation. This MVP cannot replace Archicad element topology or deform its body.");
        }
        return (dx, dy, dz);
    }

    internal static ArchicadMesh ReadBody(string guid, int bodyIndex, string elementType)
    {
        var bodyData = BridgeEnvelope.CallData(
            "Tapioca.GetBodyGeometry",
            new
            {
                elementId = new { guid },
                body = bodyIndex,
                source = "tessellated",
                coordinateSystem = "world",
                include = new[] { "vertices", "polygons", "convex" },
                maxVertices = 100000,
                maxPolygons = 50000,
                maxEdges = 100000
            });

        if (IsTrue(bodyData, "verticesTruncated"))
            throw new InvalidDataException($"Body {bodyIndex} of {guid} exceeded the 100000-vertex Dynamo preview limit.");
        var polygons = bodyData.GetProperty("polygons");
        if (IsTrue(polygons, "truncated"))
            throw new InvalidDataException($"Body {bodyIndex} of {guid} exceeded the 50000-polygon Dynamo preview limit.");
        if (polygons.GetProperty("skipped").GetInt32() > 0)
            throw new InvalidDataException($"Body {bodyIndex} of {guid} contains degenerate polygons and cannot be previewed completely.");

        double[] coordinates = bodyData.GetProperty("vertices").EnumerateArray().Select(value => value.GetDouble()).ToArray();
        if (coordinates.Length == 0 || coordinates.Length % 3 != 0)
            throw new InvalidDataException($"Body {bodyIndex} of {guid} returned an invalid vertex array.");

        var convex = bodyData.GetProperty("convex");
        int[] counts = convex.GetProperty("vertexCounts").EnumerateArray().Select(value => value.GetInt32()).ToArray();
        int[] sourceIndices = convex.GetProperty("vertexIndices").EnumerateArray().Select(value => value.GetInt32() - 1).ToArray();
        var triangles = new List<int>();
        int cursor = 0;
        foreach (int count in counts)
        {
            if (count < 3 || cursor + count > sourceIndices.Length)
                throw new InvalidDataException($"Body {bodyIndex} of {guid} returned an invalid convex decomposition.");
            int first = sourceIndices[cursor];
            ValidateIndex(first, coordinates.Length / 3, guid, bodyIndex);
            for (int corner = 1; corner + 1 < count; corner++)
            {
                int second = sourceIndices[cursor + corner];
                int third = sourceIndices[cursor + corner + 1];
                ValidateIndex(second, coordinates.Length / 3, guid, bodyIndex);
                ValidateIndex(third, coordinates.Length / 3, guid, bodyIndex);
                triangles.Add(first);
                triangles.Add(second);
                triangles.Add(third);
            }
            cursor += count;
        }
        if (cursor != sourceIndices.Length || triangles.Count == 0)
            throw new InvalidDataException($"Body {bodyIndex} of {guid} returned no preview triangles.");
        return new ArchicadMesh(guid, bodyIndex, elementType, coordinates, triangles.ToArray());
    }

    private static void ValidateIndex(int index, int vertexCount, string guid, int bodyIndex)
    {
        if (index < 0 || index >= vertexCount)
            throw new InvalidDataException($"Body {bodyIndex} of {guid} returned an out-of-range vertex index.");
    }

    private static object ElementRef(string guid) => new { elementId = new { guid } };

    private static bool IsTrue(System.Text.Json.JsonElement element, string property) =>
        element.TryGetProperty(property, out var value) && value.ValueKind == System.Text.Json.JsonValueKind.True;

    private static Dictionary<string, object> Result(
        List<ArchicadMesh> meshes,
        List<string> elementIds,
        List<int> bodyIndices,
        List<string> elementTypes) =>
        new()
        {
            ["geometry"] = meshes,
            ["elementIds"] = elementIds,
            ["bodyIndices"] = bodyIndices,
            ["elementTypes"] = elementTypes
        };
}
