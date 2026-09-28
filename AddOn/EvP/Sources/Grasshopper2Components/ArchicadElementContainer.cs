using System.Text.Json;
using Eto.Forms;
using Grasshopper2.Components;
using Grasshopper2.Data;
using Grasshopper2.Data.Meta;
using Grasshopper2.Parameters;
using Grasshopper2.UI;
using Grasshopper2.Undo;
using GrasshopperIO;

namespace TapiocaGH2;

// One output per resolved SDK type, never one output per element or a mesh
// fetched merely to discover a type. Port keys survive save/reopen and reorder.
[IoId("b44115d6-96aa-4692-94f7-8e9304bf4503")]
public sealed class ArchicadElementContainer : Component
{
    internal sealed record TypePort(int TypeId, int VariationId, string Name);
    private static readonly MetaName SourceType = new("Tapioca", "Source", "Type");
    private static readonly MetaName SourceTypeId = new("Tapioca", "Source", "TypeId");
    private static readonly MetaName SourceVariationId = new("Tapioca", "Source", "VariationId");
    private readonly object sync = new();
    private TypePort[] ports = [];
    private ArchicadElementHeaders.Header[]? headers;
    private string? cacheKey;
    private string? issue;
    private long generation;

    public ArchicadElementContainer() : base(new Nomen("Archicad Element Container",
        "Split project-bound element references into typed Archicad element families.",
        "Tapioca", "Input")) { ArchicadPresetBindings.Register(this); }

    public ArchicadElementContainer(IReader reader) : base(reader)
    {
        if (reader.HasItem((Name)"TypePorts"))
        {
            try
            {
                var restored = JsonSerializer.Deserialize<TypePort[]>(reader.String((Name)"TypePorts"));
                if (restored is { Length: <= 64 } && restored.Length == Parameters.OutputCount &&
                    restored.All(port => port.TypeId > 0 && !string.IsNullOrWhiteSpace(port.Name)) &&
                    restored.Select(port => (port.TypeId, port.VariationId)).Distinct().Count() == restored.Length)
                    ports = restored;
            }
            catch (JsonException) { } // Invalid state cannot assign a different type to a saved wire.
        }
        ArchicadPresetBindings.Register(this);
    }

    public override void Store(IWriter writer)
    {
        base.Store(writer);
        writer.String((Name)"TypePorts", JsonSerializer.Serialize(ports));
    }

    protected override void AddInputs(InputAdder inputs) => inputs.Add(new AcElementParameter(
        "Elements", "E", "Typed references from Archicad Selection or AC Element.", Access.Tree));

    protected override void AddOutputs(OutputAdder outputs) { } // Resolved types define ports later.

    protected override void Process(IDataAccess access)
    {
        if (!access.GetTree<AcElementRef>(0, out var tree)) return;
        var sourceTwigs = Enumerable.Range(0, tree.PathCount).Select(index => tree.Twigs[index]).ToArray();
        if (tree.ItemCount > 4096 || sourceTwigs.Any(twig => twig.Pears.Any(pear =>
                pear is null || !AcElementData.IsValid(pear.Item))))
        {
            access.AddWarning("Invalid elements", "Provide at most 4096 valid element references.");
            return;
        }
        string? project = ArchicadProjectOptions.CurrentIdentity();
        if (project is null || sourceTwigs.SelectMany(twig => twig.Pears).Any(pear => pear.Item.ProjectKey != project))
        {
            access.AddWarning("Project mismatch", "Attach to the project owning these references.");
            return;
        }
        Guid[] ids = sourceTwigs.SelectMany(twig => twig.Pears).Select(pear => pear.Item.ElementId)
            .Distinct().ToArray();
        string key = JsonSerializer.Serialize(new { project, ids,
            checkedAt = ArchicadProjectOptions.ReadStatus().CheckedAt });
        ArchicadElementHeaders.Header[]? rows;
        string? error;
        long request;
        bool fetch = false;
        lock (sync)
        {
            if (cacheKey != key)
            {
                cacheKey = key;
                headers = null;
                issue = null;
                ++generation;
                fetch = ids.Length != 0;
            }
            rows = headers;
            error = issue;
            request = generation;
        }
        if (fetch) _ = Task.Run(() => ResolveAsync(project, ids, request));
        if (error is not null) access.AddWarning("Element types", error);
        if (rows is null)
        {
            if (ids.Length > 0 && error is null)
                access.AddRemark("Element types pending", "Resolving headers outside the GH2 solve.");
            return;
        }
        var byId = rows.ToDictionary(row => row.Guid);
        if (rows.Any(row => !row.Found))
            access.AddWarning("Missing elements", "Some references could not be resolved; they were not assigned a type.");
        for (int output = 0; output < ports.Length; output++)
        {
            TypePort port = ports[output];
            var twigs = sourceTwigs.Select(twig => Garden.TwigFromPears(twig.Pears.Where(pear =>
            {
                var row = byId[pear.Item.ElementId];
                return row.Found && row.TypeId == port.TypeId && row.VariationId == port.VariationId;
            }).Select(pear => Garden.Pear(pear.Item,
                (pear.Meta ?? MetaData.Empty).Merge(AcElementData.Provenance(pear.Item), MergeBehaviour.Overwrite)
                    .Merge(new MetaData((SourceType, port.Name), (SourceTypeId, port.TypeId),
                        (SourceVariationId, port.VariationId)), MergeBehaviour.Overwrite)))));
            access.SetTree(output, Garden.TreeFromTwigs(tree.Paths, twigs));
        }
    }

    private async Task ResolveAsync(string project, Guid[] ids, long request)
    {
        ArchicadElementHeaders.Header[]? rows = null;
        string? failure = null;
        try
        {
            if (ArchicadProjectOptions.ParseIdentity(await Gh2ConnectionStatus.ReadProjectInfoAsync()) != project)
                throw new InvalidDataException("Archicad changed projects before the type read.");
            var found = new List<ArchicadElementHeaders.Header>(ids.Length);
            for (int offset = 0; offset < ids.Length; offset += 64)
            {
                lock (sync) if (generation != request) return;
                Guid[] batch = ids.Skip(offset).Take(64).ToArray();
                found.AddRange(ArchicadElementHeaders.ParseHeaders(
                    await Gh2ConnectionStatus.ReadElementHeadersAsync(batch), batch));
            }
            if (ArchicadProjectOptions.ParseIdentity(await Gh2ConnectionStatus.ReadProjectInfoAsync()) != project)
                throw new InvalidDataException("Archicad changed projects during the type read.");
            if (found.Any(row => row.Found && row.TypeId <= 0))
                throw new InvalidDataException("The connected Archicad add-on did not return stable element type IDs. Update the matched add-on.");
            rows = found.ToArray();
        }
        catch (Exception error) { failure = error.Message; }
        Application? app = Application.Instance;
        if (app is null) return;
        try { app.AsyncInvoke((System.Action)(() =>
        {
            if (Document is null || ArchicadProjectOptions.CurrentIdentity() != project) return;
            lock (sync)
            {
                if (generation != request) return;
                headers = rows;
                issue = failure;
            }
            if (rows is not null)
                SyncPorts(rows.Where(row => row.Found).Select(row => new TypePort(
                    row.TypeId, row.VariationId, row.Type)).DistinctBy(port => (port.TypeId, port.VariationId))
                    .OrderBy(port => port.TypeId).ThenBy(port => port.VariationId).ToArray());
            Expire();
            Document.Solution.Start();
        })); }
        catch (ObjectDisposedException) { }
        catch (InvalidOperationException) { }
    }

    private void SyncPorts(TypePort[] next)
    {
        if (next.Length > 64)
        {
            lock (sync) { headers = null; issue = "More than 64 element types; narrow the selection."; }
            return;
        }
        int wiredMissing = Enumerable.Range(0, ports.Length).Count(index =>
            !next.Any(port => port.TypeId == ports[index].TypeId &&
                port.VariationId == ports[index].VariationId) && Parameters.Output(index).Outputs.Count > 0);
        if (wiredMissing != 0 && MessageBox.Show(
                $"{wiredMissing} Archicad type output(s) have connected wires but no matching input elements. " +
                "Remove those ports and their wires? No keeps the old port layout until the next refresh.",
                "Tapioca", MessageBoxButtons.YesNo) != DialogResult.Yes)
        {
            lock (sync) issue = "Type ports differ from the current selection; confirm removal on a subsequent refresh.";
            return;
        }
        // Keep existing parameter objects (and wires) for matching type keys.
        var undo = new ActionList(Array.Empty<Grasshopper2.Undo.Action>());
        for (int i = ports.Length - 1; i >= 0; i--)
            if (!next.Any(port => port.TypeId == ports[i].TypeId && port.VariationId == ports[i].VariationId))
                Parameters.RemoveOutput(i, undo);
        foreach (TypePort port in next)
            if (!ports.Any(old => old.TypeId == port.TypeId && old.VariationId == port.VariationId))
                Parameters.AddOutput(new AcElementParameter(port.Name, "E",
                    "Project-bound Archicad " + port.Name + " references.", Access.Tree), undo);
        // Parameter order follows survivor order, then new arrivals. Do not
        // reorder existing ports merely because the incoming GUIDs moved.
        ports = ports.Where(old => next.Any(port => port.TypeId == old.TypeId && port.VariationId == old.VariationId))
            .Concat(next.Where(port => !ports.Any(old => old.TypeId == port.TypeId && old.VariationId == port.VariationId)))
            .ToArray();
        if (undo.Count > 0)
            Document?.Undo.Do(new VerbNoun("Update", "Archicad type ports"), undo);
    }
}
