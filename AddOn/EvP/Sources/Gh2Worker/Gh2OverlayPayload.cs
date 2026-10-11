using System.Text;
using System.Text.Json;

namespace Tapioca.Gh2Worker;

// Neutral model-metre data only. Native SetOverlayLayer validates the same
// geometry again; neither Rhino objects nor renderer/camera state cross the pipe.
internal static class Gh2OverlayPayload
{
    internal const int MaxBytes = 1024 * 1024;
    internal sealed record Line(double[] points, bool closed, string color, double widthPixels, string occlusion);
    internal sealed record PointSet(double[] points, string color, double sizePixels, double sizeMetres);
    internal sealed record Mesh(double[] points, int[] indices, string color, object style);

    internal static string Layer(uint generation, Guid producer)
    {
        if (generation == 0 || producer == Guid.Empty) throw new ArgumentOutOfRangeException(nameof(generation));
        return $"gh2.{generation}.{producer:N}";
    }

    internal static string Encode(uint generation, Guid producer, bool plan, Line[] lines, PointSet[] points, Mesh[] meshes)
    {
        string json = JsonSerializer.Serialize(new
        {
            layer = Layer(generation, producer), views = plan ? "2d" : "3d",
            occlusion = plan ? "always" : "hide", polylines = lines, points, meshes
        });
        if (Encoding.UTF8.GetByteCount(json) > MaxBytes)
            throw new ArgumentException("Overlay exceeds the 1 MiB transfer budget; simplify the geometry.");
        return json;
    }
}
