using Grasshopper2.Data;
using Grasshopper2.Data.Meta;
using Grasshopper2.Parameters;
using Grasshopper2.Types.Assistant;
using Grasshopper2.UI;
using GrasshopperIO;

namespace TapiocaGH2;

// ProjectKey is the bridge's comparison key, NOT an Archicad project GUID.
// No per-element revision or stable subelement addressing is available yet.
public sealed record AcElementRef(string ProjectKey, Guid ElementId)
{
    public override string ToString() => $"Archicad element {ElementId:D}";
}

internal static class AcElementData
{
    private static readonly MetaName SourceGuid = new("Tapioca", "Source", "Guid");
    private static readonly MetaName SourceProject = new("Tapioca", "Source", "ProjectKey");

    internal static MetaData Provenance(AcElementRef element) => new(
        (SourceGuid, element.ElementId), (SourceProject, element.ProjectKey));

    internal static bool IsValid(AcElementRef? element) => element is not null &&
        element.ElementId != Guid.Empty && !string.IsNullOrWhiteSpace(element.ProjectKey) &&
        element.ProjectKey.Length <= 8192;
}

// GH2 harvests assistants from the .rhp; writing a single versioned payload
// lets persistent parameters and copied pears restore the actual reference.
public sealed class AcElementRefAssistant : TypeAssistant<AcElementRef>
{
    public AcElementRefAssistant() : base("Archicad element reference") { }

    public override AcElementRef Copy(AcElementRef instance) => instance;
    public override bool Same(AcElementRef a, AcElementRef b) => a == b;
    public override int Sort(AcElementRef a, AcElementRef b)
    {
        int project = string.CompareOrdinal(a.ProjectKey, b.ProjectKey);
        return project != 0 ? project : a.ElementId.CompareTo(b.ElementId);
    }
    public override string DescribePrimary(Pear<AcElementRef> pear) => pear.Item.ToString();
    public override bool Test(AcElementRef instance, out string invalidReason)
    {
        invalidReason = AcElementData.IsValid(instance) ? "" : "Missing project key or element GUID.";
        return invalidReason.Length == 0;
    }
    public override bool Write(IWriter writer, Name location, AcElementRef instance)
    {
        if (!AcElementData.IsValid(instance))
            return false;
        writer.String(location, System.Text.Json.JsonSerializer.Serialize(new
        {
            version = 1, projectKey = instance.ProjectKey, elementId = instance.ElementId
        }));
        return true;
    }
    public override bool Read(IReader reader, Name location, out AcElementRef instance)
    {
        instance = null!;
        if (!reader.HasItem(location))
            return false;
        try
        {
            using var document = System.Text.Json.JsonDocument.Parse(reader.String(location));
            var root = document.RootElement;
            if (root.GetProperty("version").GetInt32() != 1 ||
                !Guid.TryParse(root.GetProperty("elementId").GetString(), out Guid id))
                return false;
            var candidate = new AcElementRef(root.GetProperty("projectKey").GetString() ?? "", id);
            if (!AcElementData.IsValid(candidate))
                return false;
            instance = candidate;
            return true;
        }
        catch (Exception error) when (error is System.Text.Json.JsonException or InvalidOperationException or
            KeyNotFoundException or FormatException or ArgumentException)
        { return false; }
    }
}

[IoId("fb101c48-204a-49da-85d0-22cb6e655331")]
public sealed class AcElementParameter : Parameter<AcElementRef>
{
    public AcElementParameter() : base(new Nomen("Archicad Element", "Project-bound Archicad element reference.",
        "Tapioca", "Parameters", rank: Rank.Hidden)) { }
    public AcElementParameter(string name, string code, string info, Access access = Access.Tree)
        : base(name, code, info, access) { }
    public AcElementParameter(IReader reader) : base(reader) { }
}
