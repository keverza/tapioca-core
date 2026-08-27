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
}
