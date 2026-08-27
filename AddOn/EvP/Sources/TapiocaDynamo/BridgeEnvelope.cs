using System.Text.Json;

namespace Tapioca;

internal static class BridgeEnvelope
{
    internal static JsonElement CallData(string command, object parameters)
    {
        string paramsJson = JsonSerializer.Serialize(parameters);
        string envelope = new NamedPipeBridge().Call(command, paramsJson);
        using JsonDocument document = JsonDocument.Parse(envelope);
        JsonElement root = document.RootElement;
        if (!root.GetProperty("ok").GetBoolean())
            throw new InvalidOperationException(ReadError(root, command));
        return root.GetProperty("data").Clone();
    }

    private static string ReadError(JsonElement root, string command)
    {
        if (!root.TryGetProperty("error", out JsonElement error))
            return $"{command} failed without an error payload.";
        if (error.ValueKind == JsonValueKind.String)
            return error.GetString() ?? $"{command} failed.";
        return error.TryGetProperty("message", out JsonElement message)
            ? message.GetString() ?? $"{command} failed."
            : $"{command} failed.";
    }
}
