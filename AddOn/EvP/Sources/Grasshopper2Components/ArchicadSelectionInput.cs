using System.Text.Json;
using Eto.Forms;
using Grasshopper2.Components;
using Grasshopper2.Doc.Attributes;
using Grasshopper2.Parameters;
using Grasshopper2.UI;
using Grasshopper2.Undo;
using Grasshopper2.Undo.Actions;
using GrasshopperIO;
using Rhino;

namespace TapiocaGH2;

// The saved set belongs to this GH2 component. Capturing the current Archicad
// selection never changes Archicad; only the explicit Reselect button does.
[IoId("b31976a0-3e53-47f6-8c4e-696696c48972")]
public sealed class ArchicadSelectionInput : ArchicadInputBase
{
    private Guid[] elements = [];
    private string projectKey = "";
    private string issue = "";
    private int busy;
    private long actionId;
    private sealed record SavedSelection(string ProjectKey, Guid[] Elements);

    // GH2's undo action snapshots a simple property, so a captured set can be
    // undone like other authored component state (not just saved to the file).
    public string SavedSelectionState
    {
        get => JsonSerializer.Serialize(new SavedSelection(projectKey, elements));
        set
        {
            SavedSelection? saved = JsonSerializer.Deserialize<SavedSelection>(value);
            if (saved?.Elements is null || saved.Elements.Length > 4096 ||
                saved.Elements.Any(id => id == Guid.Empty))
                return;
            elements = saved.Elements.Distinct().ToArray();
            projectKey = saved.ProjectKey ?? "";
            Expire();
        }
    }

    private void RecordSelectionChange() => AddUndoRecord(new VerbNoun("Edit", "Archicad Selection"),
        new PropertyAction(this, nameof(SavedSelectionState), autoExtend: false));

    public ArchicadSelectionInput() : base(new Nomen("Archicad Selection",
        "Save Archicad's current element selection in this definition.", "Tapioca", "Input")) { }

    public ArchicadSelectionInput(IReader reader) : base(reader)
    {
        projectKey = reader.HasItem((Name)"ProjectKey") ? reader.String((Name)"ProjectKey") : "";
        if (!reader.HasItem((Name)"ElementGuids"))
            return;
        try
        {
            string[] values = JsonSerializer.Deserialize<string[]>(reader.String((Name)"ElementGuids")) ?? [];
            if (values.Length <= 4096 && values.All(value => Guid.TryParse(value, out Guid id) && id != Guid.Empty))
                elements = values.Select(Guid.Parse).Distinct().ToArray();
        }
        catch (JsonException) { } // corrupted saved data cannot become a live selection
    }

    public override void Store(IWriter writer)
    {
        base.Store(writer);
        writer.String((Name)"ElementGuids", JsonSerializer.Serialize(elements.Select(id => id.ToString("D"))));
        writer.String((Name)"ProjectKey", projectKey);
    }

    internal int Count => elements.Length;

    protected override Grasshopper2.Doc.IAttributes CreateAttributes() => new ArchicadSelectionAttributes(this);

    protected override void AddInputs(InputAdder inputs) { }

    protected override void AddOutputs(OutputAdder outputs) =>
        outputs.AddText("Element GUIDs", "IDs", "Saved Archicad element identifiers; geometry is not yet available.", Access.Twig);

    protected override void Process(IDataAccess access)
    {
        access.SetTwig(0, elements.Select(id => id.ToString("D")).ToArray(), null, null);
        if (!string.IsNullOrEmpty(issue))
            access.AddWarning("Selection action", issue);
    }

    internal void Activate(int button)
    {
        if (button is < 0 or > 4)
            return;
        if (button == 3) // Clear does not read or modify Archicad.
        {
            Interlocked.Increment(ref actionId);
            if (elements.Length != 0)
                RecordSelectionChange();
            elements = [];
            projectKey = "";
            issue = "";
            Expire();
            Document?.Solution.Start();
            return;
        }
        if (button == 4 && elements.Length == 0)
        {
            issue = "No saved elements to reselect.";
            Expire();
            Document?.Solution.Start();
            return;
        }
        if (Interlocked.CompareExchange(ref busy, 1, 0) != 0)
            return;
        string? active = ArchicadProjectOptions.CurrentIdentity();
        if (active is null || (button != 2 && elements.Length != 0 && projectKey != active))
        {
            issue = active is null ? "Attach to a known Archicad project first." :
                "These GUIDs belong to another project. Update or Clear the saved selection.";
            Interlocked.Exchange(ref busy, 0);
            Expire();
            Document?.Solution.Start();
            return;
        }
        _ = RunActionAsync(button, active, Interlocked.Increment(ref actionId));
    }

    private async Task RunActionAsync(int button, string project, long ticket)
    {
        try
        {
            string before = ArchicadProjectOptions.ParseIdentity(
                await Gh2ConnectionStatus.ReadProjectInfoAsync().ConfigureAwait(false));
            if (before != project)
                throw new InvalidOperationException("Archicad changed projects; refresh before changing the saved selection.");
            if (button == 4) // Reselect: an explicit Archicad UI change.
            {
                string response = await Gh2ConnectionStatus.ReplaceSelectionAsync(elements).ConfigureAwait(false);
                JsonElement data = ValidateReply(response);
                if (!data.TryGetProperty("missing", out JsonElement missing) || missing.ValueKind != JsonValueKind.Array)
                    throw new InvalidDataException("Archicad did not report unresolved selection IDs.");
                string message = missing.GetArrayLength() == 0 ? "" :
                    $"Archicad could not reselect {missing.GetArrayLength()} saved element(s).";
                PostResult(null, message, button, project, ticket);
                return;
            }
            Guid[] selected = ParseSelection(await Gh2ConnectionStatus.ReadSelectionAsync().ConfigureAwait(false));
            string after = ArchicadProjectOptions.ParseIdentity(
                await Gh2ConnectionStatus.ReadProjectInfoAsync().ConfigureAwait(false));
            if (after != project)
                throw new InvalidOperationException("Archicad changed projects while capturing; retry.");
            PostResult(selected, "", button, project, ticket);
        }
        catch (Exception error)
        {
            PostResult(null, error.Message, button, project, ticket);
        }
        finally { Interlocked.Exchange(ref busy, 0); }
    }

    private void PostResult(Guid[]? selected, string failure, int button, string project, long ticket)
    {
        Application? app = Application.Instance;
        if (app is null)
            return;
        try { app.AsyncInvoke((System.Action)(() =>
        {
            if (Document is null || ticket != Interlocked.Read(ref actionId))
                return;
            if (selected is not null && ArchicadProjectOptions.CurrentIdentity() != project)
                failure = "Archicad changed projects while capturing this selection. Retry.";
            issue = failure;
            if (selected is not null && failure.Length == 0)
            {
                Guid[] next = button switch
                {
                    0 => elements.Concat(selected).Distinct().ToArray(),
                    1 => elements.Except(selected).ToArray(),
                    2 => selected,
                    _ => elements
                };
                if (next.Length > 4096)
                    issue = "The saved selection exceeds the 4096-element limit.";
                else
                {
                    if (!elements.SequenceEqual(next) || projectKey != project)
                        RecordSelectionChange();
                    elements = next;
                    projectKey = project;
                }
            }
            Expire();
            Document?.Solution.Start();
        })); }
        catch (ObjectDisposedException) { }
        catch (InvalidOperationException) { }
    }

    private static JsonElement ValidateReply(string json)
    {
        using JsonDocument response = JsonDocument.Parse(json);
        JsonElement root = response.RootElement;
        if (root.ValueKind != JsonValueKind.Object ||
            !root.TryGetProperty("ok", out JsonElement ok) ||
            (ok.ValueKind != JsonValueKind.True && ok.ValueKind != JsonValueKind.False))
            throw new InvalidDataException("Archicad returned an invalid selection reply.");
        if (ok.ValueKind == JsonValueKind.False)
        {
            string reason = "No reason was provided.";
            if (root.TryGetProperty("error", out JsonElement error))
            {
                if (error.ValueKind == JsonValueKind.String)
                    reason = error.GetString() ?? reason;
                else if (error.ValueKind == JsonValueKind.Object)
                {
                    string code = error.TryGetProperty("code", out JsonElement codeValue) &&
                        codeValue.ValueKind == JsonValueKind.String ? codeValue.GetString() ?? "" : "";
                    string message = error.TryGetProperty("message", out JsonElement messageValue) &&
                        messageValue.ValueKind == JsonValueKind.String ? messageValue.GetString() ?? "" : "";
                    reason = string.IsNullOrEmpty(code) ? message : $"[{code}] {message}";
                }
            }
            throw new InvalidDataException("Archicad selection request failed: " + reason[..Math.Min(reason.Length, 512)]);
        }
        if (!root.TryGetProperty("data", out JsonElement data) || data.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException("Archicad selection reply has no data.");
        return data.Clone();
    }

    internal static Guid[] ParseSelection(string json)
    {
        JsonElement data = ValidateReply(json);
        if (!data.TryGetProperty("elements", out JsonElement items) || items.ValueKind != JsonValueKind.Array ||
            items.GetArrayLength() > 4096)
            throw new InvalidDataException("Archicad returned an invalid selection size.");
        var result = new List<Guid>(items.GetArrayLength());
        foreach (JsonElement item in items.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.Object ||
                !item.TryGetProperty("elementId", out JsonElement elementId) ||
                elementId.ValueKind != JsonValueKind.Object ||
                !elementId.TryGetProperty("guid", out JsonElement text) ||
                text.ValueKind != JsonValueKind.String ||
                !Guid.TryParse(text.GetString(), out Guid id) || id == Guid.Empty)
                throw new InvalidDataException("Archicad returned an invalid selected element ID.");
            result.Add(id);
        }
        return result.Distinct().ToArray();
    }
}
