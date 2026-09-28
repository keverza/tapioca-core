using System.Text.Json;
using Eto.Forms;
using Grasshopper2.Components;
using Grasshopper2.Data;
using Grasshopper2.Data.Meta;
using Grasshopper2.Parameters;
using Grasshopper2.UI;
using GrasshopperIO;

namespace TapiocaGH2;

// Header fields are requested only by this downstream component. One solve
// schedules a bounded batch; later solutions consume copied immutable data.
[IoId("5b531b0f-7b19-4cb1-8b05-4bdc349be451")]
public sealed class ArchicadElementHeaders : Component
{
    internal sealed record Header(Guid Guid, bool Found, string Type,
        string ElementId, int Story, string Layer, int TypeId = 0, int VariationId = 0);

    private static readonly MetaName TypeName = new("Tapioca", "Source", "Type");
    private readonly object sync = new();
    private readonly Dictionary<string, Header[]> cache = new();
    private readonly HashSet<string> pending = new();
    private readonly Dictionary<string, string> errors = new();
    private DateTimeOffset? snapshotTime;
    private long ticket;

    public ArchicadElementHeaders() : base(new Nomen("AC Element Headers",
        "Request basic Archicad element fields in bounded batches; no geometry or properties are fetched.",
        "Tapioca", "Deconstruct")) { ArchicadPresetBindings.Register(this); }
    public ArchicadElementHeaders(IReader reader) : base(reader) { ArchicadPresetBindings.Register(this); }

    protected override void AddInputs(InputAdder inputs)
    {
        inputs.Add(new AcElementParameter("Elements", "E", "Project-bound Archicad elements.", Access.Twig));
    }

    protected override void AddOutputs(OutputAdder outputs)
    {
        outputs.AddText("Type", "T", "Archicad element type name.", Access.Twig);
        outputs.AddText("Element ID", "ID", "Value of Archicad's element ID field.", Access.Twig);
        outputs.AddInteger("Story", "S", "Archicad story index.", Access.Twig);
        outputs.AddText("Layer", "L", "Current Archicad layer name.", Access.Twig);
    }

    protected override void Process(IDataAccess access)
    {
        if (!access.GetPears<AcElementRef>(0, out var pears) || pears.Length == 0)
            return;
        if (pears.Length > 4096 || pears.Any(pear => pear is null || !AcElementData.IsValid(pear.Item)))
        {
            access.AddWarning("Invalid batch", "Provide at most 4096 valid element references per twig.");
            return;
        }
        string? project = ArchicadProjectOptions.CurrentIdentity();
        var status = ArchicadProjectOptions.ReadStatus();
        if (project is null || pears.Any(pear => pear.Item.ProjectKey != project))
        {
            access.AddWarning("Project mismatch", "Attach to the project that owns these element references.");
            return;
        }
        Guid[] ids = pears.Select(pear => pear.Item.ElementId).ToArray();
        string key = project + "|" + string.Join(",", ids.Select(id => id.ToString("D")));
        Header[]? headers;
        string? issue;
        bool queue;
        long request;
        lock (sync)
        {
            if (snapshotTime != status.CheckedAt)
            {
                ++ticket;
                cache.Clear();
                pending.Clear();
                errors.Clear();
                snapshotTime = status.CheckedAt;
            }
            cache.TryGetValue(key, out headers);
            errors.TryGetValue(key, out issue);
            queue = headers is null && issue is null && pending.Add(key);
            request = ticket;
        }
        if (queue)
            _ = Task.Run(() => ResolveAsync(key, project, ids, request, status.CheckedAt));
        if (issue is not null)
            access.AddWarning("Header query", issue);
        if (headers is null)
            return;
        int missing = headers.Count(header => !header.Found);
        if (missing != 0)
            access.AddWarning("Missing elements", $"{missing} Archicad element(s) could not be resolved; their output positions are null.");
        // Null pears retain the missing element's position in each immutable
        // twig; omitting them would shift later fields onto the wrong GUID.
        access.SetTwig(0, Garden.TwigFromPears(pears.Select((pear, i) => headers[i].Found
            ? Garden.Pear(headers[i].Type, (pear.Meta ?? MetaData.Empty)
                .Merge(new MetaData((TypeName, headers[i].Type)), MergeBehaviour.Overwrite)) : null)));
        access.SetTwig(1, Garden.TwigFromPears(pears.Select((pear, i) => headers[i].Found
            ? Garden.Pear(headers[i].ElementId, pear.Meta) : null)));
        access.SetTwig(2, Garden.TwigFromPears(pears.Select((pear, i) => headers[i].Found
            ? Garden.Pear(headers[i].Story, pear.Meta) : null)));
        access.SetTwig(3, Garden.TwigFromPears(pears.Select((pear, i) => headers[i].Found
            ? Garden.Pear(headers[i].Layer, pear.Meta) : null)));
    }

    private async Task ResolveAsync(string key, string project, Guid[] ids, long request,
        DateTimeOffset? checkedAt)
    {
        Header[]? result = null;
        string? failure = null;
        try
        {
            string first = await Gh2ConnectionStatus.ReadProjectInfoAsync().ConfigureAwait(false);
            if (ArchicadProjectOptions.ParseIdentity(first) != project)
                throw new InvalidDataException("Archicad changed projects before the header read.");
            var collected = new List<Header>(ids.Length);
            for (int offset = 0; offset < ids.Length; offset += 64)
            {
                lock (sync)
                    if (request != ticket)
                        return; // a newer snapshot superseded this batch
                Guid[] page = ids.Skip(offset).Take(64).ToArray();
                collected.AddRange(ParseHeaders(
                    await Gh2ConnectionStatus.ReadElementHeadersAsync(page).ConfigureAwait(false), page));
            }
            result = collected.ToArray();
            string last = await Gh2ConnectionStatus.ReadProjectInfoAsync().ConfigureAwait(false);
            if (ArchicadProjectOptions.ParseIdentity(last) != project)
                throw new InvalidDataException("Archicad changed projects during the header read.");
        }
        catch (Exception error) { failure = error.Message; }
        Application? app = Application.Instance;
        if (app is null)
            return;
        try { app.AsyncInvoke((System.Action)(() =>
        {
            lock (sync)
            {
                if (request != ticket || snapshotTime != checkedAt ||
                    ArchicadProjectOptions.CurrentIdentity() != project)
                    return;
                pending.Remove(key);
                if (result is not null)
                {
                    if (cache.Count >= 64)
                        cache.Clear();
                    cache[key] = result;
                }
                else
                    errors[key] = failure ?? "Archicad did not return element headers.";
            }
            if (Document is null)
                return;
            Expire();
            Document.Solution.Start();
        })); }
        catch (ObjectDisposedException) { }
        catch (InvalidOperationException) { }
    }

    internal static Header[] ParseHeaders(string json, IReadOnlyList<Guid> requested)
    {
        JsonElement data = ArchicadSelectionInput.ValidateReply(json, "element-header");
        if (!data.TryGetProperty("elements", out var rows) ||
            rows.ValueKind != JsonValueKind.Array || rows.GetArrayLength() != requested.Count)
            throw new InvalidDataException("Archicad returned a truncated element-header batch.");
        var values = new Header[requested.Count];
        for (int i = 0; i < values.Length; i++)
        {
            JsonElement row = rows[i];
            if (!Guid.TryParse(row.GetProperty("guid").GetString(), out Guid id) || id != requested[i])
                throw new InvalidDataException("Archicad returned out-of-order element headers.");
            values[i] = new Header(id, row.GetProperty("found").GetBoolean(),
                row.GetProperty("type").GetString() ?? "",
                row.GetProperty("elementId").GetString() ?? "", row.GetProperty("story").GetInt32(),
                row.GetProperty("layer").GetString() ?? "",
                row.TryGetProperty("typeId", out var typeId) ? typeId.GetInt32() : 0,
                row.TryGetProperty("variationId", out var variationId) ? variationId.GetInt32() : 0);
        }
        return values;
    }
}
