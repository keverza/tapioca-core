using System.Text.Json;
using Dynamo.Graph.Nodes;

namespace Tapioca;

public static class Selection
{
    [NodeCategory("Query")]
    public static IReadOnlyList<string> Current()
    {
        string envelope = new NamedPipeBridge().Call("Tapioca.GetSelection", "{}");
        using JsonDocument document = JsonDocument.Parse(envelope);
        JsonElement root = document.RootElement;
        if (!root.GetProperty("ok").GetBoolean())
            throw new InvalidOperationException(ReadError(root));

        var guids = new List<string>();
        foreach (JsonElement element in root.GetProperty("data").GetProperty("elements").EnumerateArray())
        {
            string? guid = element.GetProperty("elementId").GetProperty("guid").GetString();
            if (!string.IsNullOrEmpty(guid))
                guids.Add(guid);
        }
        return guids;
    }

    private static string ReadError(JsonElement root)
    {
        if (!root.TryGetProperty("error", out JsonElement error))
            return "Tapioca.GetSelection failed without an error payload.";
        return error.TryGetProperty("message", out JsonElement message)
            ? message.GetString() ?? "Tapioca.GetSelection failed."
            : "Tapioca.GetSelection failed.";
    }
}
