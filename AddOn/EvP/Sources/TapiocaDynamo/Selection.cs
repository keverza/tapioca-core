using Autodesk.DesignScript.Runtime;
using Dynamo.Graph.Nodes;

namespace Tapioca;

public static class Selection
{
    [NodeCategory("Query")]
    [CanUpdatePeriodically(true)]
    public static IReadOnlyList<string> Current(bool refresh = false)
    {
        _ = refresh;
        var data = BridgeEnvelope.CallData("Tapioca.GetSelection", new { });

        var guids = new List<string>();
        foreach (var element in data.GetProperty("elements").EnumerateArray())
        {
            string? guid = element.GetProperty("elementId").GetProperty("guid").GetString();
            if (!string.IsNullOrEmpty(guid))
                guids.Add(guid);
        }
        return guids;
    }

    /// <summary>
    /// Selects the given elements in Archicad, replacing whatever was selected.
    /// Returns how many of them Archicad could actually resolve.
    /// </summary>
    /// <remarks>
    /// ⚠️ THIS WRITES THE USER'S SELECTION, which is why it is a separate node rather
    /// than something Current could do. A graph that reached out and changed the
    /// selection while merely evaluating is a defect — the same rule
    /// NodeGraphSelectionCommands.cpp states for the node-graph editor, where
    /// `reselect` is a button the user pressed and never an evaluation side effect.
    /// Keep it downstream of something deliberate.
    /// </remarks>
    [NodeCategory("Actions")]
    public static int Select(IEnumerable<string> elements)
    {
        var payload = elements
            .Where(guid => !string.IsNullOrWhiteSpace(guid))
            .Select(guid => new { elementId = new { guid = guid.Trim() } })
            .ToArray();

        var data = BridgeEnvelope.CallData("Tapioca.SetSelection", new { elements = payload });
        return data.TryGetProperty("selected", out var selected) && selected.TryGetInt32(out int count)
            ? count
            : payload.Length;
    }
}
