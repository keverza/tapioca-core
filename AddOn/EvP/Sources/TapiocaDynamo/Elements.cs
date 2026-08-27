using System.Collections.Concurrent;
using Autodesk.DesignScript.Runtime;
using Dynamo.Graph.Nodes;

namespace Tapioca;

public static class Elements
{
    private static readonly ConcurrentDictionary<string, bool> ApplyStates = new(StringComparer.OrdinalIgnoreCase);

    [NodeCategory("Modify")]
    [IsLacingDisabled]
    [MultiReturn("applied", "backend", "elementId", "message")]
    public static Dictionary<string, object> ApplyTranslation(
        ArchicadMesh original,
        ArchicadMesh modified,
        string backend = "Tapioca",
        bool apply = false)
    {
        var desired = Geometry.TranslationVector(original, modified);
        string elementId = original.ElementId;

        string selectedBackend = backend.Trim();
        if (!selectedBackend.Equals("Tapioca", StringComparison.OrdinalIgnoreCase) &&
            !selectedBackend.Equals("Tapir", StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("Backend must be either 'Tapioca' or 'Tapir'.", nameof(backend));

        string applyKey = $"{selectedBackend}:{elementId}";
        if (!apply)
        {
            ApplyStates[applyKey] = false;
            return Result(false, selectedBackend, elementId,
                $"Staged target translation ({desired.dx}, {desired.dy}, {desired.dz}) m. Set apply to true to write Archicad.");
        }
        if (!ApplyStates.TryGetValue(applyKey, out bool previousState))
        {
            ApplyStates[applyKey] = true;
            return Result(false, selectedBackend, elementId, "Apply opened as true and was not executed. Set apply false once to arm this write, then toggle it true.");
        }
        if (previousState || !ApplyStates.TryUpdate(applyKey, true, false))
            return Result(false, selectedBackend, elementId, "This apply pulse was already consumed. Toggle apply false, then true, to write again.");

        ArchicadMesh current = Geometry.ReadBody(elementId, original.BodyIndex, original.ElementType);
        var residual = Geometry.TranslationVector(current, modified);
        if (Math.Abs(residual.dx) <= 0.000001 && Math.Abs(residual.dy) <= 0.000001 && Math.Abs(residual.dz) <= 0.000001)
            return Result(false, selectedBackend, elementId, "Archicad is already at the staged target; no write or undo entry was created.");

        if (selectedBackend.Equals("Tapioca", StringComparison.OrdinalIgnoreCase))
            return ApplyWithTapioca(elementId, residual.dx, residual.dy, residual.dz);
        return ApplyWithTapir(elementId, residual.dx, residual.dy, residual.dz);
    }

    private static Dictionary<string, object> ApplyWithTapioca(string guid, double dx, double dy, double dz)
    {
        const double tolerance = 0.000001;
        if (Math.Abs(dx) > tolerance || Math.Abs(dy) > tolerance)
            throw new InvalidOperationException(
                "The Tapioca MVP can push vertical translations only. Use backend 'Tapir' for XYZ movement, or keep dx and dy at zero.");

        var detailsData = BridgeEnvelope.CallData(
            "Tapioca.GetElementDetails",
            new { elements = new[] { new { elementId = new { guid } } } });
        var record = detailsData.GetProperty("detailsOfElements")[0];
        if (!record.GetProperty("found").GetBoolean())
            throw new InvalidOperationException($"Archicad element {guid} was not found or its type has no writable details.");
        var details = record.GetProperty("details");
        if (!details.TryGetProperty("level", out var levelValue))
            throw new InvalidOperationException(
                $"Tapioca cannot move element kind '{record.GetProperty("kind").GetString()}' vertically because it has no writable level.");

        double newLevel = levelValue.GetDouble() + dz;
        var writeData = BridgeEnvelope.CallData(
            "Tapioca.SetElementDetails",
            new { edits = new[] { new { elementId = new { guid }, details = new { level = newLevel } } } });
        var result = writeData.GetProperty("results")[0];
        if (!result.GetProperty("succeeded").GetBoolean())
            throw new InvalidOperationException(result.TryGetProperty("error", out var error)
                ? error.GetString() ?? "Tapioca.SetElementDetails refused the translation."
                : "Tapioca.SetElementDetails refused the translation.");
        return Result(true, "Tapioca", guid, $"Applied vertical translation {dz} m; new level is {newLevel} m.");
    }

    private static Dictionary<string, object> ApplyWithTapir(string guid, double dx, double dy, double dz)
    {
        var data = BridgeEnvelope.CallData(
            "Tapir.MoveElements",
            new
            {
                elementsWithMoveVectors = new[]
                {
                    new { elementId = new { guid }, moveVector = new { x = dx, y = dy, z = dz }, copy = false }
                }
            });
        if (!data.TryGetProperty("executionResults", out var results) || results.GetArrayLength() != 1 ||
            !results[0].TryGetProperty("success", out var success) || !success.GetBoolean())
            throw new InvalidOperationException(
                "Tapir.MoveElements did not confirm one successful translation. Verify that Tapir is installed and the element is editable.");
        return Result(true, "Tapir", guid, $"Applied XYZ translation ({dx}, {dy}, {dz}) m through Tapir.");
    }

    private static Dictionary<string, object> Result(bool applied, string backend, string elementId, string message) =>
        new()
        {
            ["applied"] = applied,
            ["backend"] = backend,
            ["elementId"] = elementId,
            ["message"] = message
        };
}
