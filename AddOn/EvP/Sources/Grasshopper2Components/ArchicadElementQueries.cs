using System.Text.Json;
using Eto.Forms;
using Grasshopper2.Components;
using Grasshopper2.Data;
using Grasshopper2.Parameters;
using Grasshopper2.UI;
using GrasshopperIO;

namespace TapiocaGH2;

// A shared, non-blocking query lifecycle for all five downstream components.
// Each input pear has one aligned result/status/diagnostic pear; never collapse
// missing elements and accidentally associate the next row with their GUID.
public abstract class ArchicadElementQuery<T> : Component where T : class
{
    private readonly object sync = new();
    private readonly Dictionary<string, QueryRow[]> cache = new();
    private readonly HashSet<string> pending = new();
    private readonly Dictionary<string, string> failures = new();
    private DateTimeOffset? snapshotTime;
    private long ticket;

    protected ArchicadElementQuery(Nomen nomen) : base(nomen) => ArchicadPresetBindings.Register(this);
    protected ArchicadElementQuery(IReader reader) : base(reader) => ArchicadPresetBindings.Register(this);
    protected abstract string Kind { get; }
    protected abstract string DefaultMode { get; }
    protected abstract string[] Modes { get; }
    protected abstract void AddResult(OutputAdder outputs);
    protected abstract T Convert(AcElementRef source, string mode, JsonElement[] items);
    protected virtual void AddExtraOutputs(OutputAdder outputs) { }
    protected virtual void SetExtraOutputs(IDataAccess access, T[] values, Pear<AcElementRef>[] pears) { }

    protected override void AddInputs(InputAdder inputs)
    {
        inputs.Add(new AcElementParameter("Elements", "E", "Project-bound Archicad references.", Access.Twig));
        inputs.AddText("Representation", "M", "Supported: " + string.Join(", ", Modes)).Set(DefaultMode);
        inputs.AddText("Selectors", "S", "Comma-separated property definition GUIDs or GDL internal names; empty discovers available values.").Set("");
        inputs.AddText("Search", "Q", "Filter property names or GDL names/descriptions/groups during discovery.").Set("");
    }

    protected override void AddOutputs(OutputAdder outputs)
    {
        AddResult(outputs);
        outputs.AddText("Status", "S", "Pending, Success, Empty, NotApplicable, Unavailable, Stale or Error, per input.", Access.Twig);
        outputs.AddText("Diagnostic", "D", "Reason when a query cannot produce data, per input.", Access.Twig);
        AddExtraOutputs(outputs);
    }

    protected override void Process(IDataAccess access)
    {
        if (!access.GetPears<AcElementRef>(0, out var pears) || pears.Length == 0) return;
        if (pears.Length > 512 || pears.Any(pear => pear is null || !AcElementData.IsValid(pear.Item)))
        {
            access.AddWarning("Invalid batch", "Provide at most 512 valid references per twig.");
            return;
        }
        access.GetItem(1, out string? mode);
        mode ??= DefaultMode;
        if (!Modes.Contains(mode, StringComparer.Ordinal))
        {
            access.AddWarning("Unsupported representation", "Choose one of: " + string.Join(", ", Modes));
            EmitNoData(access, pears, mode, AcQueryStatus.Unavailable, "Unsupported representation.");
            return;
        }
        access.GetItem(2, out string? selectorText);
        string[] selectors = (selectorText ?? "").Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);
        if (selectors.Length > 64 || selectors.Any(s => s.Length > 128))
        {
            access.AddWarning("Invalid selectors", "At most 64 selectors of 128 characters each are allowed.");
            return;
        }
        access.GetItem(3, out string? search);
        search ??= "";
        if (search.Length > 128) { access.AddWarning("Invalid search", "Search is limited to 128 characters."); return; }
        var status = ArchicadProjectOptions.ReadStatus();
        string? project = ArchicadProjectOptions.CurrentIdentity();
        if (project is null || pears.Any(pear => pear.Item.ProjectKey != project))
        {
            access.AddWarning("Project mismatch", "Attach to this Archicad project; references from another project cannot be queried.");
            EmitNoData(access, pears, mode, AcQueryStatus.Stale, "The element reference belongs to another or unknown project.");
            return;
        }
        Guid[] ids = pears.Select(pear => pear.Item.ElementId).ToArray();
        string key = JsonSerializer.Serialize(new { project, Kind, mode, search, selectors, ids });
        QueryRow[]? rows;
        string? failure;
        bool queue;
        long request;
        lock (sync)
        {
            if (snapshotTime != status.CheckedAt)
            {
                ++ticket; cache.Clear(); pending.Clear(); failures.Clear(); snapshotTime = status.CheckedAt;
            }
            cache.TryGetValue(key, out rows);
            failures.TryGetValue(key, out failure);
            queue = rows is null && failure is null && pending.Add(key);
            request = ticket;
        }
        if (queue) _ = Task.Run(() => ResolveAsync(key, project, ids, mode, search, selectors, request, status.CheckedAt));
        if (failure is not null)
        {
            access.AddWarning("Archicad query", failure);
            EmitNoData(access, pears, mode, AcQueryStatus.Error, failure);
            return;
        }
        if (rows is null)
        {
            EmitNoData(access, pears, mode, AcQueryStatus.Pending, "Reading from Archicad asynchronously.");
            return;
        }
        try
        {
            T[] values = pears.Select((pear, i) => Convert(pear.Item, mode, rows[i].Items)).ToArray();
            access.SetTwig(0, Garden.TwigFromPears(pears.Select((pear, i) =>
                Garden.Pear(values[i], pear.Meta ?? AcElementData.Provenance(pear.Item)))));
            access.SetTwig(1, Garden.TwigFromPears(pears.Select((pear, i) =>
                Garden.Pear(rows[i].Status.ToString(), pear.Meta ?? AcElementData.Provenance(pear.Item)))));
            access.SetTwig(2, Garden.TwigFromPears(pears.Select((pear, i) =>
                Garden.Pear(rows[i].Diagnostic, pear.Meta ?? AcElementData.Provenance(pear.Item)))));
            SetExtraOutputs(access, values, pears);
        }
        catch (Exception error) when (error is JsonException or FormatException or KeyNotFoundException or InvalidOperationException)
        { access.AddWarning("Invalid query data", error.Message); }
    }

    private void EmitNoData(IDataAccess access, Pear<AcElementRef>[] pears, string mode,
        AcQueryStatus status, string diagnostic)
    {
        T[] values = pears.Select(pear => Convert(pear.Item, mode, [])).ToArray();
        access.SetTwig(0, Garden.TwigFromPears(pears.Select((pear, i) =>
            Garden.Pear(values[i], pear.Meta ?? AcElementData.Provenance(pear.Item)))));
        access.SetTwig(1, Garden.TwigFromPears(pears.Select(pear =>
            Garden.Pear(status.ToString(), pear.Meta ?? AcElementData.Provenance(pear.Item)))));
        access.SetTwig(2, Garden.TwigFromPears(pears.Select(pear =>
            Garden.Pear(diagnostic, pear.Meta ?? AcElementData.Provenance(pear.Item)))));
        SetExtraOutputs(access, values, pears);
    }

    private async Task ResolveAsync(string key, string project, Guid[] ids, string mode, string search, string[] selectors,
        long request, DateTimeOffset? checkedAt)
    {
        QueryRow[]? result = null;
        string? failure = null;
        try
        {
            string first = await Gh2ConnectionStatus.ReadProjectInfoAsync().ConfigureAwait(false);
            if (ArchicadProjectOptions.ParseIdentity(first) != project)
                throw new InvalidDataException("Archicad changed projects before the query.");
            var collected = new List<QueryRow>(ids.Length);
            for (int offset = 0; offset < ids.Length; offset += 8)
            {
                lock (sync) if (request != ticket) return;
                Guid[] page = ids.Skip(offset).Take(8).ToArray();
                var pageRows = new QueryRow[page.Length];
                for (int position = 0; ; position += 128)
                {
                    lock (sync) if (request != ticket) return;
                    QueryRow[] slice = ParseRows(await Gh2ConnectionStatus.ReadElementQueryAsync(
                        page, Kind, mode, search, selectors, position).ConfigureAwait(false), page);
                    pageRows = MergePages(pageRows, slice);
                    if (!slice.Any(row => row.More)) break;
                    if (position >= 1920)
                    {
                        for (int i = 0; i < pageRows.Length; i++)
                            if (slice[i].More)
                                pageRows[i] = new QueryRow(page[i], AcQueryStatus.Unavailable,
                                    "More than 2,048 matching entries; narrow Search or Selectors.", false, []);
                        break;
                    }
                }
                collected.AddRange(pageRows);
            }
            string last = await Gh2ConnectionStatus.ReadProjectInfoAsync().ConfigureAwait(false);
            // modiStamp advances on selection and model-generation reads too.
            // It is a hint for the Connection Status catalog, not a transaction
            // token for unrelated Archicad element queries.
            if (ArchicadProjectOptions.ParseIdentity(last) != project)
                throw new InvalidDataException("Archicad changed projects during the query.");
            result = collected.ToArray();
        }
        catch (Exception error) { failure = error.Message; }
        Application? app = Application.Instance;
        if (app is null) return;
        try { app.AsyncInvoke((System.Action)(() =>
        {
            lock (sync)
            {
                if (request != ticket || snapshotTime != checkedAt ||
                    ArchicadProjectOptions.CurrentIdentity() != project) return;
                pending.Remove(key);
                if (result is null) failures[key] = failure ?? "Archicad did not return query results.";
                else { if (cache.Count >= 32) cache.Clear(); cache[key] = result; }
            }
            if (Document is null) return;
            Expire();
            Document.Solution.Start();
        })); }
        catch (ObjectDisposedException) { }
        catch (InvalidOperationException) { }
    }

    internal sealed record QueryRow(Guid Guid, AcQueryStatus Status, string Diagnostic, bool More, JsonElement[] Items);

    internal static QueryRow[] MergePages(QueryRow[] accumulated, QueryRow[] page)
    {
        if (accumulated.Length != page.Length)
            throw new InvalidDataException("Archicad returned a misaligned query page.");
        var merged = new QueryRow[page.Length];
        for (int i = 0; i < page.Length; i++)
        {
            QueryRow? previous = accumulated[i];
            QueryRow current = page[i];
            if (previous is not null && previous.Guid != current.Guid)
                throw new InvalidDataException("Archicad changed the GUID between query pages.");
            merged[i] = previous is null ? current
                : previous.Status is not (AcQueryStatus.Success or AcQueryStatus.Empty) ? previous
                : current.Status is AcQueryStatus.Success or AcQueryStatus.Empty
                    ? current with
                    {
                        Status = previous.Items.Length + current.Items.Length == 0
                            ? AcQueryStatus.Empty : AcQueryStatus.Success,
                        Items = [.. previous.Items, .. current.Items]
                    }
                    : current; // never publish a partial failed result
        }
        return merged;
    }

    internal static QueryRow[] ParseRows(string json, IReadOnlyList<Guid> requested)
    {
        JsonElement data = ArchicadSelectionInput.ValidateReply(json, "element-query");
        if (!data.TryGetProperty("elements", out var elements) || elements.ValueKind != JsonValueKind.Array ||
            elements.GetArrayLength() != requested.Count)
            throw new InvalidDataException("Archicad returned a truncated element-query batch.");
        var rows = new QueryRow[requested.Count];
        for (int i = 0; i < rows.Length; i++)
        {
            var row = elements[i];
            if (!Guid.TryParse(row.GetProperty("guid").GetString(), out Guid id) || id != requested[i] ||
                !Enum.TryParse(row.GetProperty("status").GetString(), false, out AcQueryStatus status))
                throw new InvalidDataException("Archicad returned misaligned or invalid query results.");
            var items = row.GetProperty("items");
            if (items.ValueKind != JsonValueKind.Array || items.GetArrayLength() > 256)
                throw new InvalidDataException("Archicad returned an invalid item count.");
            rows[i] = new QueryRow(id, status, row.GetProperty("diagnostic").GetString() ?? "",
                row.GetProperty("more").GetBoolean(),
                items.EnumerateArray().Select(item => item.Clone()).ToArray());
        }
        return rows;
    }
}

[IoId("839335bd-cf50-4598-9765-f158c4b68c59")]
public sealed class ArchicadGetContours : ArchicadElementQuery<AcContourSet>
{
    public ArchicadGetContours() : base(new Nomen("Get Contours", "Read native polygon rings and holes on demand.", "Tapioca", "Archicad")) { }
    public ArchicadGetContours(IReader reader) : base(reader) { }
    protected override string Kind => "contours";
    protected override string DefaultMode => "Definition";
    protected override string[] Modes => ["Definition", "Boundary", "Visible 2D", "Section"];
    protected override void AddResult(OutputAdder outputs) => outputs.Add(new AcContourParameter(
        "Contours", "C", "Native polygon rings and holes, in project coordinates."));
    protected override void AddExtraOutputs(OutputAdder outputs) => outputs.AddText("Rings", "R",
        "Ring role, closure and project-coordinate vertices, one line per ring.", Access.Twig);
    protected override void SetExtraOutputs(IDataAccess access, AcContourSet[] values, Pear<AcElementRef>[] pears) =>
        access.SetTwig(3, Garden.TwigFromPears(values.Select((value, i) => Garden.Pear(
            string.Join("\n", value.Rings.Select(ring =>
                $"{ring.Role} ({(ring.Closed ? "closed" : "open")}): " +
                string.Join("; ", Enumerable.Range(0, ring.Xy.Length / 2)
                    .Select(index => $"({ring.Xy[2 * index]:G6}, {ring.Xy[2 * index + 1]:G6}) arc {ring.ArcAngles[index]:G6}")))),
            pears[i].Meta ?? AcElementData.Provenance(pears[i].Item)))));
    protected override AcContourSet Convert(AcElementRef source, string mode, JsonElement[] items) => new(source,
        items.Select(item => new AcContourRing(item.GetProperty("role").GetString() ?? "",
            item.GetProperty("closed").GetBoolean(),
            item.GetProperty("xy").EnumerateArray().Select(value => value.GetDouble()).ToArray(),
            item.GetProperty("arcs").EnumerateArray().Select(value => value.GetDouble()).ToArray())).ToArray());
}

[IoId("1aca73bb-0231-4a9c-921e-37aba8546155")]
public sealed class ArchicadGetRelationships : ArchicadElementQuery<AcRelationSet>
{
    public ArchicadGetRelationships() : base(new Nomen("Get Relationships", "Read typed, native wall-hosted opening edges.", "Tapioca", "Archicad")) { }
    public ArchicadGetRelationships(IReader reader) : base(reader) { }
    protected override string Kind => "relationships";
    protected override string DefaultMode => "Hosted";
    protected override string[] Modes => ["Hosted", "Connected", "Inferred"];
    protected override void AddResult(OutputAdder outputs) => outputs.Add(new AcRelationParameter(
        "Relationships", "R", "Typed directional relations with source and target references."));
    protected override void AddExtraOutputs(OutputAdder outputs)
    {
        outputs.Add(new AcElementParameter("Related Elements", "E", "Typed targets for downstream Archicad queries.", Access.Twig));
        outputs.AddText("Edges", "X", "Native relationship type and target GUID, one line per edge.", Access.Twig);
    }
    protected override void SetExtraOutputs(IDataAccess access, AcRelationSet[] values, Pear<AcElementRef>[] pears)
    {
        access.SetTwig(3, Garden.TwigFromPears(values.SelectMany(value => value.Edges.Select(edge =>
            Garden.Pear(edge.Target, AcElementData.Provenance(edge.Target))))));
        access.SetTwig(4, Garden.TwigFromPears(values.Select((value, i) => Garden.Pear(
            string.Join("\n", value.Edges.Select(edge => $"{edge.RelationType}: {edge.Target.ElementId:D}")),
            pears[i].Meta ?? AcElementData.Provenance(pears[i].Item)))));
    }
    protected override AcRelationSet Convert(AcElementRef source, string mode, JsonElement[] items) => new(source,
        items.Select(item => new AcRelation(source,
            new AcElementRef(source.ProjectKey, item.GetProperty("target").GetGuid()),
            item.GetProperty("relation").GetString() ?? "", item.GetProperty("provenance").GetString() ?? "")).ToArray());
}

[IoId("72d1643e-0e64-4e37-95c4-5ffbc73cd7a7")]
public sealed class ArchicadGetProperties : ArchicadElementQuery<AcPropertySet>
{
    public ArchicadGetProperties() : base(new Nomen("Get Properties", "Discover available definitions or extract selected Archicad properties.", "Tapioca", "Archicad")) { }
    public ArchicadGetProperties(IReader reader) : base(reader) { }
    protected override string Kind => "properties";
    protected override string DefaultMode => "Discover";
    protected override string[] Modes => ["Discover", "All", "User", "Built-in"];
    protected override void AddResult(OutputAdder outputs) => outputs.Add(new AcPropertyParameter(
        "Properties", "P", "Available definitions or selected evaluated property values."));
    protected override void AddExtraOutputs(OutputAdder outputs)
    {
        outputs.AddText("Available", "A", "Property group/name [definition GUID] list, one item per source.", Access.Twig);
        outputs.AddText("Values", "V", "Formatted value and evaluation status for each selected definition.", Access.Twig);
    }
    protected override void SetExtraOutputs(IDataAccess access, AcPropertySet[] values, Pear<AcElementRef>[] pears)
    {
        access.SetTwig(3, Garden.TwigFromPears(values.Select((value, i) => Garden.Pear(
            string.Join("\n", value.Values.Select(property =>
                $"{property.Group} / {property.Name} [{property.DefinitionId:D}]")),
            pears[i].Meta ?? AcElementData.Provenance(pears[i].Item)))));
        access.SetTwig(4, Garden.TwigFromPears(values.Select((value, i) => Garden.Pear(
            string.Join("\n", value.Values.Select(property =>
                $"{property.Group} / {property.Name}: {property.Text} ({property.ValueStatus})")),
            pears[i].Meta ?? AcElementData.Provenance(pears[i].Item)))));
    }
    protected override AcPropertySet Convert(AcElementRef source, string mode, JsonElement[] items) => new(source,
        items.Select(item => new AcPropertyValue(item.GetProperty("key").GetGuid(),
            item.GetProperty("group").GetString() ?? "", item.GetProperty("name").GetString() ?? "",
            item.GetProperty("dataType").GetInt32(), item.GetProperty("definitionType").GetInt32(),
            item.GetProperty("userDefined").GetBoolean(), item.GetProperty("collectionType").GetInt32(),
            item.GetProperty("valueStatus").GetString() ?? "", item.GetProperty("value").GetString() ?? "",
            item.GetProperty("hasNumber").GetBoolean() ? item.GetProperty("number").GetDouble() : null,
            item.GetProperty("hasBoolean").GetBoolean() ? item.GetProperty("boolean").GetBoolean() : null)).ToArray());
}

[IoId("da78e1ea-f789-4246-ae25-1a42166ef404")]
public sealed class ArchicadGetGeometry : ArchicadElementQuery<AcGeometrySet>
{
    public ArchicadGetGeometry() : base(new Nomen("Get Geometry", "Request 3D model bounds, without inventing 3D geometry for annotations.", "Tapioca", "Archicad")) { }
    public ArchicadGetGeometry(IReader reader) : base(reader) { }
    protected override string Kind => "geometry";
    protected override string DefaultMode => "Bounding box";
    protected override string[] Modes => ["Bounding box", "Surface mesh", "Native definition", "2D drawing", "Derived Brep"];
    protected override void AddResult(OutputAdder outputs) => outputs.Add(new AcGeometryParameter(
        "Geometry", "G", "Available 3D model bounds in project coordinates."));
    protected override void AddExtraOutputs(OutputAdder outputs) => outputs.AddText("Bounds", "B",
        "Minimum and maximum project-coordinate metres, one item per source.", Access.Twig);
    protected override void SetExtraOutputs(IDataAccess access, AcGeometrySet[] values, Pear<AcElementRef>[] pears) =>
        access.SetTwig(3, Garden.TwigFromPears(values.Select((value, i) => Garden.Pear(
            string.Join("\n", value.Bounds.Select(box =>
                $"({box.MinX:G6}, {box.MinY:G6}, {box.MinZ:G6}) – ({box.MaxX:G6}, {box.MaxY:G6}, {box.MaxZ:G6}) m")),
            pears[i].Meta ?? AcElementData.Provenance(pears[i].Item)))));
    protected override AcGeometrySet Convert(AcElementRef source, string mode, JsonElement[] items) => new(source, mode,
        items.Select(item => new AcBounds(item.GetProperty("minX").GetDouble(), item.GetProperty("minY").GetDouble(),
            item.GetProperty("minZ").GetDouble(), item.GetProperty("maxX").GetDouble(),
            item.GetProperty("maxY").GetDouble(), item.GetProperty("maxZ").GetDouble())).ToArray());
}

[IoId("859af4c8-e7dd-484e-a03c-338d7574b50f")]
public sealed class ArchicadGdlParameters : ArchicadElementQuery<AcGdlSet>
{
    public ArchicadGdlParameters() : base(new Nomen("GDL Parameters & Settings", "Read placed GDL parameters, descriptions, groups and display flags; no edits.", "Tapioca", "Archicad")) { }
    public ArchicadGdlParameters(IReader reader) : base(reader) { }
    protected override string Kind => "gdl";
    protected override string DefaultMode => "Instance";
    protected override string[] Modes => ["Instance", "All parameters"];
    protected override void AddResult(OutputAdder outputs) => outputs.Add(new AcGdlParameterOutput(
        "GDL Parameters", "P", "Placed GDL instance parameters and settings descriptors."));
    protected override void AddExtraOutputs(OutputAdder outputs)
    {
        outputs.AddText("Available", "A", "GDL group/description [internal name], one item per source.", Access.Twig);
        outputs.AddText("Values", "V", "Placed scalar values and per-parameter availability.", Access.Twig);
    }
    protected override void SetExtraOutputs(IDataAccess access, AcGdlSet[] values, Pear<AcElementRef>[] pears)
    {
        access.SetTwig(3, Garden.TwigFromPears(values.Select((value, i) => Garden.Pear(
            string.Join("\n", value.Parameters.Select(parameter =>
                $"{parameter.Group} / {parameter.DisplayName} [{parameter.InternalName}]")),
            pears[i].Meta ?? AcElementData.Provenance(pears[i].Item)))));
        access.SetTwig(4, Garden.TwigFromPears(values.Select((value, i) => Garden.Pear(
            string.Join("\n", value.Parameters.Select(parameter =>
                $"{parameter.Group} / {parameter.DisplayName} [{parameter.InternalName}]: " +
                $"{(parameter.IsArray ? $"array {parameter.Rows}×{parameter.Columns}" : parameter.ValueStatus != "HasValue" ? "—" : parameter.Text.Length > 0 ? parameter.Text : parameter.Number.ToString("G6"))} ({parameter.ValueStatus})")),
            pears[i].Meta ?? AcElementData.Provenance(pears[i].Item)))));
    }
    protected override AcGdlSet Convert(AcElementRef source, string mode, JsonElement[] items) => new(source,
        items.Select(item => new AcGdlParameter(item.GetProperty("libraryPart").GetString() ?? "",
            item.GetProperty("key").GetString() ?? "",
            item.GetProperty("label").GetString() ?? "", item.GetProperty("group").GetString() ?? "",
            item.GetProperty("type").GetInt32(), item.GetProperty("hidden").GetBoolean(),
            item.GetProperty("disabled").GetBoolean(), item.GetProperty("array").GetBoolean(),
            item.GetProperty("dim1").GetInt32(), item.GetProperty("dim2").GetInt32(),
            item.GetProperty("valueStatus").GetString() ?? "", item.GetProperty("text").GetString() ?? "",
            item.GetProperty("number").GetDouble())).ToArray());
}
