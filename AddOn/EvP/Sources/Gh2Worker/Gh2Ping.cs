using System.Text.Json;

namespace Tapioca.Gh2Worker;

// Shared by the headless worker and the Rhino-owned peer. This fixed read is
// outside GH2 evaluation; components never receive the bridge.
internal static class Gh2Ping
{
    internal static string ConfirmStartup(Gh2PipeClient bridge, uint generation, string greeting)
    {
        using JsonDocument response = JsonDocument.Parse(bridge.ReadProjectInfo());
        JsonElement root = response.RootElement;
        if (root.ValueKind != JsonValueKind.Object ||
            !root.TryGetProperty("ok", out JsonElement ok) ||
            (ok.ValueKind != JsonValueKind.True && ok.ValueKind != JsonValueKind.False))
            throw new InvalidDataException("Archicad project-info Ping has no valid 'ok' flag.");
        if (ok.ValueKind == JsonValueKind.False)
        {
            if (root.TryGetProperty("error", out JsonElement nativeError) &&
                nativeError.ValueKind == JsonValueKind.Object)
            {
                string? code = ErrorField(nativeError, "code");
                string? message = ErrorField(nativeError, "message");
                string? detail = ErrorField(nativeError, "detail");
                throw new InvalidDataException("Archicad project-info Ping failed" +
                    (code is null ? "" : $" [{code}]") +
                    (message is null ? "." : $": {message}") +
                    (detail is null ? "" : $" (detail: {detail})"));
            }
            string? reason = nativeError.ValueKind == JsonValueKind.String ? nativeError.GetString() : null;
            throw new InvalidDataException("Archicad project-info Ping failed: " +
                (string.IsNullOrWhiteSpace(reason) ? "native error unavailable." : reason));
        }
        if (!root.TryGetProperty("data", out JsonElement data) || data.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException("Archicad project-info Ping: data must be an object.");
        if (!data.TryGetProperty("archicadVersion", out JsonElement versionValue))
            throw new InvalidDataException("Archicad project-info Ping: archicadVersion is missing.");
        if (versionValue.ValueKind != JsonValueKind.Number || !versionValue.TryGetInt32(out int version) || version <= 0)
        {
            string status = versionValue.ValueKind != JsonValueKind.Number ? versionValue.ValueKind.ToString() :
                !versionValue.TryGetInt32(out int value) ? "not an Int32 integer" :
                $"non-positive ({value})";
            throw new InvalidDataException($"Archicad project-info Ping: archicadVersion is invalid ({status}).");
        }
        if (!data.TryGetProperty("archicadBuild", out JsonElement buildValue) ||
            buildValue.ValueKind != JsonValueKind.Number || !buildValue.TryGetInt32(out int build) || build < 0)
            throw new InvalidDataException("Archicad project-info Ping: archicadBuild must be a non-negative Int32 integer.");
        if (!data.TryGetProperty("projectName", out JsonElement nameValue) || nameValue.ValueKind != JsonValueKind.String)
            throw new InvalidDataException("Archicad project-info Ping: projectName must be a string.");
        string name = nameValue.GetString()!;
        if (string.IsNullOrWhiteSpace(name))
            name = data.TryGetProperty("untitled", out JsonElement untitled) && untitled.ValueKind == JsonValueKind.True
                ? "untitled" : "project name unavailable";
        bridge.Startup(true, $"{greeting} (generation {generation}).");
        string summary = $"Archicad {version} build {(build == 0 ? "unavailable" : build.ToString())}, project '{name}'";
        bridge.Log("GH2 Ping: " + summary + ".");
        return summary;
    }

    private static string? ErrorField(JsonElement error, string field) =>
        error.TryGetProperty(field, out JsonElement value) && value.ValueKind == JsonValueKind.String &&
        !string.IsNullOrWhiteSpace(value.GetString()) ? value.GetString() : null;

    internal static void AcknowledgeStartupFailure(Gh2PipeClient bridge, Exception error)
    {
        if (bridge.StartupAcknowledged)
            return;
        try { bridge.Startup(false, error.Message); }
        catch (IOException) { }
        catch (ObjectDisposedException) { }
    }
}
