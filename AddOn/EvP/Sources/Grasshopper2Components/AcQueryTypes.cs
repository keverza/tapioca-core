using System.Text.Json;
using Grasshopper2.Data;
using Grasshopper2.Parameters;
using Grasshopper2.Types.Assistant;
using Grasshopper2.UI;
using GrasshopperIO;

namespace TapiocaGH2;

public enum AcQueryStatus { Pending, Success, Empty, NotApplicable, Unavailable, Stale, Error }

// One result per input pear, even for an empty or unsupported query. These
// records are snapshots, not live Archicad objects; polygon XY and 3D bounds
// are project/world metres and the native polygon arc angles are radians.
public sealed record AcContourRing(string Role, bool Closed, double[] Xy, double[] ArcAngles);
public sealed record AcContourSet(AcElementRef Source, AcContourRing[] Rings);
public sealed record AcRelation(AcElementRef Source, AcElementRef Target, string RelationType, string Provenance);
public sealed record AcRelationSet(AcElementRef Source, AcRelation[] Edges);
public sealed record AcPropertyValue(Guid DefinitionId, string Group, string Name,
    int DataType, int DefinitionType, bool UserDefined, int CollectionType,
    string ValueStatus, string Text, double? Number, bool? Boolean);
public sealed record AcPropertySet(AcElementRef Source, AcPropertyValue[] Values);
public sealed record AcBounds(double MinX, double MinY, double MinZ, double MaxX, double MaxY, double MaxZ);
public sealed record AcGeometrySet(AcElementRef Source, string Representation, AcBounds[] Bounds);
public sealed record AcGdlParameter(string LibraryPart, string InternalName, string DisplayName, string Group, int DataType,
    bool Hidden, bool Disabled, bool IsArray, int Rows, int Columns,
    string ValueStatus, string Text, double Number);
public sealed record AcGdlSet(AcElementRef Source, AcGdlParameter[] Parameters);

// GH2 needs a type assistant to round-trip custom data in persistent parameters
// and saved .ghz files. The format is versioned independently of the wire JSON.
public abstract class AcQueryAssistant<T>(string name) : TypeAssistant<T>(name) where T : class
{
    public override T Copy(T instance) => instance;
    public override bool Same(T a, T b) => JsonSerializer.Serialize(a) == JsonSerializer.Serialize(b);
    public override int Sort(T a, T b) => string.CompareOrdinal(JsonSerializer.Serialize(a), JsonSerializer.Serialize(b));
    public override string DescribePrimary(Pear<T> pear) => pear.Item switch
    {
        AcContourSet value => $"{value.Rings.Length} Archicad contour(s) of {value.Source.ElementId:D}",
        AcRelationSet value => $"{value.Edges.Length} Archicad relation(s) of {value.Source.ElementId:D}",
        AcPropertySet value => $"{value.Values.Length} Archicad propert(ies) of {value.Source.ElementId:D}",
        AcGeometrySet value => $"{value.Bounds.Length} Archicad {value.Representation} result(s) of {value.Source.ElementId:D}",
        AcGdlSet value => $"{value.Parameters.Length} GDL parameter(s) of {value.Source.ElementId:D}",
        _ => Name
    };
    public override bool Test(T instance, out string invalidReason)
    {
        invalidReason = instance is null ? "Null Archicad query result." : "";
        return instance is not null;
    }
    public override bool Write(IWriter writer, Name location, T instance)
    {
        if (instance is null) return false;
        writer.String(location, JsonSerializer.Serialize(new { version = 1, value = instance }));
        return true;
    }
    public override bool Read(IReader reader, Name location, out T instance)
    {
        instance = null!;
        if (!reader.HasItem(location)) return false;
        try
        {
            using var json = JsonDocument.Parse(reader.String(location));
            if (json.RootElement.GetProperty("version").GetInt32() != 1) return false;
            instance = json.RootElement.GetProperty("value").Deserialize<T>()!;
            return instance is not null;
        }
        catch (Exception error) when (error is JsonException or InvalidOperationException or KeyNotFoundException)
        { return false; }
    }
}

public sealed class AcContourAssistant() : AcQueryAssistant<AcContourSet>("Archicad contours");
public sealed class AcRelationAssistant() : AcQueryAssistant<AcRelationSet>("Archicad relationships");
public sealed class AcPropertyAssistant() : AcQueryAssistant<AcPropertySet>("Archicad properties");
public sealed class AcGeometryAssistant() : AcQueryAssistant<AcGeometrySet>("Archicad geometry bounds");
public sealed class AcGdlAssistant() : AcQueryAssistant<AcGdlSet>("Archicad GDL parameters");

public abstract class AcQueryParameter<T> : Parameter<T> where T : class
{
    protected AcQueryParameter(string name, string code, string description) :
        base(name, code, description, Access.Twig) { }
    protected AcQueryParameter(Nomen nomen) : base(nomen) { }
    protected AcQueryParameter(IReader reader) : base(reader) { }
}

[IoId("897e6ae6-e6cb-4c41-a332-376947d3ae15")]
public sealed class AcContourParameter : AcQueryParameter<AcContourSet>
{
    public AcContourParameter() : base(new Nomen("Contours", "Native polygon rings with arc angles and hole roles.",
        "Tapioca", "Parameters", rank: Rank.Hidden)) { }
    public AcContourParameter(string name, string code, string description) : base(name, code, description) { }
    public AcContourParameter(IReader reader) : base(reader) { }
}
[IoId("178749be-840c-487a-a422-58002ed3bb66")]
public sealed class AcRelationParameter : AcQueryParameter<AcRelationSet>
{
    public AcRelationParameter() : base(new Nomen("Relationships", "Typed directional native database edges.",
        "Tapioca", "Parameters", rank: Rank.Hidden)) { }
    public AcRelationParameter(string name, string code, string description) : base(name, code, description) { }
    public AcRelationParameter(IReader reader) : base(reader) { }
}
[IoId("465d383d-bc16-4806-8c07-84a64326eaaa")]
public sealed class AcPropertyParameter : AcQueryParameter<AcPropertySet>
{
    public AcPropertyParameter() : base(new Nomen("Properties", "Selected property definitions and evaluation states.",
        "Tapioca", "Parameters", rank: Rank.Hidden)) { }
    public AcPropertyParameter(string name, string code, string description) : base(name, code, description) { }
    public AcPropertyParameter(IReader reader) : base(reader) { }
}
[IoId("f0dcdeaf-d719-48cb-b483-65acc79bddf2")]
public sealed class AcGeometryParameter : AcQueryParameter<AcGeometrySet>
{
    public AcGeometryParameter() : base(new Nomen("Geometry", "3D world-coordinate model bounds (metres).",
        "Tapioca", "Parameters", rank: Rank.Hidden)) { }
    public AcGeometryParameter(string name, string code, string description) : base(name, code, description) { }
    public AcGeometryParameter(IReader reader) : base(reader) { }
}
[IoId("7cb556b7-4afa-4e1f-a94c-555302ed56d1")]
public sealed class AcGdlParameterOutput : AcQueryParameter<AcGdlSet>
{
    public AcGdlParameterOutput() : base(new Nomen("GDL Parameters", "Placed library-part parameters, distinct from Archicad properties.",
        "Tapioca", "Parameters", rank: Rank.Hidden)) { }
    public AcGdlParameterOutput(string name, string code, string description) : base(name, code, description) { }
    public AcGdlParameterOutput(IReader reader) : base(reader) { }
}
