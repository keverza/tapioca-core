using System.Text.Json;
using Eto.Forms;
using Grasshopper2.Components;
using Grasshopper2.Doc;
using Grasshopper2.Data;
using Grasshopper2.Data.Meta;
using Grasshopper2.Parameters;
using TextParameter = Grasshopper2.Parameters.Standard.TextParameter;
using Grasshopper2.UI;
using GrasshopperIO;
using Rhino;
using Rhino.Geometry;

namespace TapiocaGH2;

// A shared, non-blocking query lifecycle for all five downstream components.
// Each input pear has one aligned result/status/diagnostic pear; never collapse
// missing elements and accidentally associate the next row with their GUID.
internal interface IArchicadQueryPresets
{
    void RestoreModePresets();
    void ClearSelectorPresets();
}

public abstract class ArchicadElementQuery<T> : Component, IArchicadQueryPresets, IArchicadInputConsumer where T : class
{
    private sealed record Request(string Project, Guid[] Ids, string Mode, string Search, string[] Selectors);
    private sealed record Staged(QueryRow[]? Rows, string? Failure, long Ticket);
    private readonly object sync = new();
    // What Process reads. Only CommitInputs replaces it, between solutions.
    private readonly Dictionary<string, QueryRow[]> cache = new();
    private readonly Dictionary<string, Request> requests = new();
    // Rows read since, awaiting adoption. A refresh never empties the outputs.
    private readonly Dictionary<string, Staged> staged = new();
    private readonly HashSet<string> pending = new();
    private readonly HashSet<string> stalePending = new();
    private readonly Dictionary<string, string> failures = new();
    private string? stagedIssue, refreshIssue;
    private DateTimeOffset? snapshotTime;
    private long ticket;
    private volatile bool deliveryActive;
    bool IArchicadInputConsumer.IsActive => deliveryActive;
    object? IArchicadInputConsumer.InputGroup =>
        Document is { } document && document.State is not (Grasshopper2.Doc.DocumentState.Closing or Grasshopper2.Doc.DocumentState.Closed)
            ? document : null;

    private void ResetLocked()
    {
        ++ticket; cache.Clear(); requests.Clear(); staged.Clear(); pending.Clear(); stalePending.Clear();
        failures.Clear(); stagedIssue = refreshIssue = null;
    }

    // Re-read only the rows a change names, merged over the rows now shown.
    async Task<bool> IArchicadInputConsumer.StageInputsAsync(IReadOnlyCollection<Guid>? changed, bool lastAttempt,
        CancellationToken cancel)
    {
        HashSet<Guid>? set = changed is null ? null : changed as HashSet<Guid> ?? [.. changed];
        var work = new List<(string Key, Request Query, QueryRow[]? Shown)>();
        long started;
        lock (sync)
        {
            started = ticket;
            foreach (var (key, query) in requests)
            {
                if (set is not null && !query.Ids.Any(set.Contains)) continue;
                // A first read in flight may predate this change: it reads again.
                if (pending.Contains(key)) { stalePending.Add(key); continue; }
                work.Add((key, query, staged.TryGetValue(key, out Staged? newer) && newer.Ticket == started &&
                    newer.Rows is not null ? newer.Rows : cache.GetValueOrDefault(key)));
            }
        }
        if (work.Count == 0) return true;
        try
        {
            var done = new List<(string Key, QueryRow[] Rows)>();
            foreach (var (key, query, shown) in work)
            {
                cancel.ThrowIfCancellationRequested();
                // A relationship row depends on elements other than its own.
                bool whole = shown is null || set is null || Kind == "relationships";
                Guid[] ids = (whole ? query.Ids : query.Ids.Where(set!.Contains)).Distinct().ToArray();
                Dictionary<Guid, QueryRow>? rows = await FetchRowsAsync(ids, query, () =>
                {
                    if (cancel.IsCancellationRequested) return false;
                    lock (sync) return started == ticket;
                }).ConfigureAwait(false);
                cancel.ThrowIfCancellationRequested();
                if (rows is null) return true; // superseded by a refresh or reconnect; nothing to retry
                done.Add((key, query.Ids.Select((id, i) => rows.TryGetValue(id, out QueryRow? row) ? row : shown![i]).ToArray()));
            }
            string identity = ArchicadProjectOptions.ParseIdentity(
                await Gh2ConnectionStatus.ReadProjectInfoAsync().ConfigureAwait(false));
            lock (sync)
            {
                if (started != ticket) return true;
                foreach (var (key, rows) in done)
                    if (requests.TryGetValue(key, out Request? query) && query.Project == identity)
                        staged[key] = new(rows, null, started);
                stagedIssue = null;
            }
            return true;
        }
        catch (OperationCanceledException) { throw; }
        catch (Exception error)
        {
            // The rows shown stay; after the last attempt they are labelled stale.
            if (lastAttempt)
                lock (sync)
                    if (started == ticket) stagedIssue = error.Message;
            return false;
        }
    }

    bool IArchicadInputConsumer.CommitInputs()
    {
        if (Document is null) return false;
        DateTimeOffset? checkedAt = ArchicadProjectOptions.ReadStatus().CheckedAt;
        string? identity = ArchicadProjectOptions.CurrentIdentity();
        bool changed = false;
        var presets = new List<QueryRow[]>();
        lock (sync)
        {
            foreach (var (key, entry) in staged)
            {
                if (snapshotTime != checkedAt || entry.Ticket != ticket ||
                    !requests.TryGetValue(key, out Request? query) || query.Project != identity) continue;
                pending.Remove(key);
                if (entry.Rows is null)
                {
                    if (!cache.ContainsKey(key))
                    {
                        failures[key] = entry.Failure ?? "Archicad did not return query results.";
                        changed = true;
                    }
                    continue;
                }
                bool known = cache.TryGetValue(key, out QueryRow[]? shown);
                // A hint names an element, not a field. Identical rows mean the
                // change was unrelated to this query: no expiry, no solution.
                if (known && SameRows(shown!, entry.Rows)) { changed |= failures.Remove(key); continue; }
                if (!known && cache.Count >= 32)
                {
                    cache.Clear(); failures.Clear();
                    foreach (string stale in requests.Keys.Where(name => name != key && !pending.Contains(name) &&
                                 !staged.ContainsKey(name)).ToArray())
                        requests.Remove(stale);
                }
                cache[key] = entry.Rows; failures.Remove(key); changed = true;
                // A selected-value read is only a subset. Replacing the picker's
                // catalog with that subset would make its other choices disappear.
                if (query.Selectors.Length == 0 && ((Kind == "properties" && query.Mode == "Discover") || Kind == "gdl"))
                    presets.Add(entry.Rows);
            }
            staged.Clear();
            changed |= stagedIssue is not null && stagedIssue != refreshIssue;
            refreshIssue = stagedIssue;
        }
        foreach (QueryRow[] rows in presets) ApplySelectorPresets(rows);
        if (changed) Expire();
        return changed;
    }

    internal static bool SameRows(QueryRow[] left, QueryRow[] right)
    {
        if (left.Length != right.Length) return false;
        for (int i = 0; i < left.Length; i++)
        {
            QueryRow a = left[i], b = right[i];
            if (ReferenceEquals(a, b)) continue;
            if (a.Guid != b.Guid || a.Status != b.Status || a.Diagnostic != b.Diagnostic || a.More != b.More ||
                a.Items.Length != b.Items.Length) return false;
            for (int item = 0; item < a.Items.Length; item++)
                if (a.Items[item].GetRawText() != b.Items[item].GetRawText()) return false;
        }
        return true;
    }

    protected ArchicadElementQuery(Nomen nomen) : base(nomen) => InitializeDelivery();
    protected ArchicadElementQuery(IReader reader) : base(reader) => InitializeDelivery();
    private void InitializeDelivery()
    {
        ArchicadPresetBindings.Register(this);
        DocumentChanged += (_, _) => deliveryActive = false;
    }
    protected abstract string Kind { get; }
    protected abstract string DefaultMode { get; }
    protected abstract string[] Modes { get; }
    protected abstract void AddResult(OutputAdder outputs);
    protected abstract T Convert(AcElementRef source, string mode, JsonElement[] items);
    protected virtual void AddExtraOutputs(OutputAdder outputs) { }
    protected virtual void SetExtraOutputs(IDataAccess access, T[] values, Pear<AcElementRef>[] pears) { }
    protected static MetaData SourceMeta(Pear<AcElementRef> pear) =>
        (pear.Meta ?? MetaData.Empty).Merge(AcElementData.Provenance(pear.Item), MergeBehaviour.Overwrite);

    protected override void AddInputs(InputAdder inputs)
    {
        inputs.Add(new AcElementParameter("Elements", "E", "Project-bound Archicad references.", Access.Twig));
        TextParameter representation = inputs.AddText("Representation", "M",
            "Connect a GH2 Preset Picker or choose: " + string.Join(", ", Modes));
        representation.Set(DefaultMode);
        foreach (string mode in Modes)
            representation.Presets.Add(mode, "Archicad query representation", mode);
        inputs.AddText("Selectors", "S", "Comma-separated property definition GUIDs or GDL internal names; empty discovers available values.").Set("");
        inputs.AddText("Search", "Q", "Filter property names or GDL names/descriptions/groups during discovery.").Set("");
    }

    void IArchicadQueryPresets.RestoreModePresets()
    {
        if (Parameters?.Input(1) is not TextParameter input) return;
        input.Presets.Clear();
        foreach (string mode in Modes)
            input.Presets.Add(mode, "Archicad query representation", mode);
        ArchicadPresetBindings.RefreshConnectedPickers(this, 1);
    }

    void IArchicadQueryPresets.ClearSelectorPresets()
    {
        if (Parameters?.Input(2) is not TextParameter input) return;
        input.Presets.Clear();
        ArchicadPresetBindings.RefreshConnectedPickers(this, 2);
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
        deliveryActive = true;
        QueryRow[]? rows;
        string? failure, issue;
        bool queue;
        long request;
        lock (sync)
        {
            if (snapshotTime != status.CheckedAt) { ResetLocked(); snapshotTime = status.CheckedAt; }
            cache.TryGetValue(key, out rows);
            failures.TryGetValue(key, out failure);
            queue = rows is null && failure is null && pending.Add(key);
            if (queue) requests[key] = new(project, ids, mode, search, selectors);
            request = ticket;
            issue = refreshIssue;
        }
        Task baseline = ArchicadChangeDelivery.Subscribe(this, key, ids,
            Kind switch { "properties" => 8, "gdl" => 4, "relationships" => 16, _ => 3 }, beforeFirstRead: queue);
        access.AddRemark("Archicad change source", ArchicadChangeDelivery.Status);
        if (queue)
        {
            object? fetch = ArchicadInputs.Shared.BeginFetch(this);
            _ = Task.Run(() => ResolveAsync(key, request, fetch, baseline));
        }
        if (issue is not null && rows is not null)
            access.AddWarning("Archicad refresh", "Showing the last successful read; the latest refresh failed. " + issue);
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
        QueryRow? problem = rows.FirstOrDefault(row => row.Status is AcQueryStatus.Error or
            AcQueryStatus.Unavailable or AcQueryStatus.Stale);
        if (problem is not null)
            access.AddWarning("Archicad query", $"{rows.Count(row => row.Status == problem.Status)} " +
                $"element(s): {problem.Status}. {problem.Diagnostic}");
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
        catch (Exception error) when (error is JsonException or FormatException or KeyNotFoundException or
            InvalidOperationException or InvalidDataException or ArgumentException)
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

    // The first read of a new request. Its rows are staged like any refresh,
    // so sibling components adopt together and the definition solves once.
    private async Task ResolveAsync(string key, long request, object? fetch, Task baseline)
    {
        QueryRow[]? result = null;
        string? failure = null;
        bool stagedAny = false;
        try
        {
            await baseline.ConfigureAwait(false);
            Request? query;
            lock (sync)
                if (request != ticket || !requests.TryGetValue(key, out query)) return;
            try
            {
                for (int attempt = 0; result is null; attempt++)
                {
                    string first = await Gh2ConnectionStatus.ReadProjectInfoAsync().ConfigureAwait(false);
                    if (ArchicadProjectOptions.ParseIdentity(first) != query.Project)
                        throw new InvalidDataException("Archicad changed projects before the query.");
                    Dictionary<Guid, QueryRow>? rows = await FetchRowsAsync(query.Ids.Distinct().ToArray(), query,
                        () => { lock (sync) return request == ticket; }).ConfigureAwait(false);
                    if (rows is null) return;
                    string last = await Gh2ConnectionStatus.ReadProjectInfoAsync().ConfigureAwait(false);
                    // modiStamp advances on selection and model-generation reads too.
                    // It is a hint for the Connection Status catalog, not a transaction
                    // token for unrelated Archicad element queries.
                    if (ArchicadProjectOptions.ParseIdentity(last) != query.Project)
                        throw new InvalidDataException("Archicad changed projects during the query.");
                    lock (sync)
                    {
                        if (request != ticket) return;
                        // A change was reported while this read ran; read again, a bounded number of times.
                        if (stalePending.Remove(key) && attempt < 2) continue;
                    }
                    result = query.Ids.Select(id => rows[id]).ToArray();
                }
            }
            catch (Exception error) { failure = error.Message; }
            lock (sync)
            {
                if (request != ticket || !pending.Contains(key)) return;
                staged[key] = new(result, result is null ? failure ?? "Archicad did not return query results." : null, request);
                stagedAny = true;
            }
        }
        finally { ArchicadInputs.Shared.EndFetch(fetch, this, stagedAny); }
    }

    // Reads the given distinct elements in bounded pages; null when superseded.
    private async Task<Dictionary<Guid, QueryRow>?> FetchRowsAsync(Guid[] ids, Request query, Func<bool> current)
    {
        var collected = new Dictionary<Guid, QueryRow>(ids.Length);
        int batchSize = Kind == "geometry" && query.Mode == "Surface mesh" ? 1 : 8;
        for (int offset = 0; offset < ids.Length; offset += batchSize)
        {
            if (!current()) return null;
            Guid[] page = ids.Skip(offset).Take(batchSize).ToArray();
            var pageRows = new QueryRow[page.Length];
            for (int position = 0; ; position += 128)
            {
                if (!current()) return null;
                QueryRow[] slice = ParseRows(await Gh2ConnectionStatus.ReadElementQueryAsync(
                    page, Kind, query.Mode, query.Search, query.Selectors, position).ConfigureAwait(false), page);
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
            for (int i = 0; i < page.Length; i++) collected[page[i]] = pageRows[i];
        }
        return collected;
    }

    private void ApplySelectorPresets(QueryRow[] rows)
    {
        if (Parameters?.Input(2) is not TextParameter input) return;
        input.Presets.Clear();
        var unique = new HashSet<string>(StringComparer.Ordinal);
        foreach (JsonElement item in rows.SelectMany(row => row.Items))
        {
            if (!item.TryGetProperty("key", out JsonElement id) || id.ValueKind != JsonValueKind.String ||
                !item.TryGetProperty("name", out JsonElement name) &&
                !item.TryGetProperty("label", out name)) continue;
            string key = id.GetString() ?? "";
            if (key.Length == 0 || !unique.Add(key)) continue;
            string group = item.TryGetProperty("group", out JsonElement groupValue)
                ? groupValue.GetString() ?? "" : "";
            input.Presets.Add($"{group} / {name.GetString()} [{key}]", "Archicad selector key", key);
        }
        ArchicadPresetBindings.RefreshConnectedPickers(this, 2);
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
    public ArchicadGetContours() : base(new Nomen("Get Contours", "Read native polygon rings and holes on demand.", "Tapioca", "Deconstruct")) { }
    public ArchicadGetContours(IReader reader) : base(reader) { }
    protected override string Kind => "contours";
    protected override string DefaultMode => "Definition";
    protected override string[] Modes => ["Definition", "Boundary"];
    protected override void AddResult(OutputAdder outputs) => outputs.Add(new AcContourParameter(
        "Contours", "C", "Native polygon rings and holes, in project coordinates."));
    protected override void AddExtraOutputs(OutputAdder outputs)
    {
        outputs.AddText("Rings", "R", "Ring role, closure and project-coordinate vertices.", Access.Twig);
        outputs.AddCurve("Plan Curves", "Crv", "Rhino curves at Z=0, converted from Archicad metres to document units; arcs retained.", Access.Twig);
    }
    protected override void SetExtraOutputs(IDataAccess access, AcContourSet[] values, Pear<AcElementRef>[] pears)
    {
        access.SetTwig(3, Garden.TwigFromPears(values.Select((value, i) => Garden.Pear(
            string.Join("\n", value.Rings.Select(ring =>
                $"{ring.Role} ({(ring.Closed ? "closed" : "open")}): " +
                string.Join("; ", Enumerable.Range(0, ring.Xy.Length / 2)
                    .Select(index => $"({ring.Xy[2 * index]:G6}, {ring.Xy[2 * index + 1]:G6}) arc {ring.ArcAngles[index]:G6}")))),
            pears[i].Meta ?? AcElementData.Provenance(pears[i].Item)))));
        double scale = RhinoMath.UnitScale(UnitSystem.Meters,
            RhinoDoc.ActiveDoc?.ModelUnitSystem ?? UnitSystem.Meters);
        access.SetTwig(4, Garden.TwigFromPears(values.SelectMany((value, i) => value.Rings.Select(ring =>
            Garden.Pear<Curve>(BuildPlanCurve(ring, scale),
                SourceMeta(pears[i]))))));
    }

    private static Curve BuildPlanCurve(AcContourRing ring, double scale)
    {
        int count = ring.Xy.Length / 2;
        if (count < 2 || ring.ArcAngles.Length != count)
            throw new InvalidDataException("Archicad returned an invalid contour ring.");
        var points = Enumerable.Range(0, count).Select(i => new Point3d(
            ring.Xy[2 * i] * scale, ring.Xy[2 * i + 1] * scale, 0)).ToArray();
        int distinct = ring.Closed && points[0].DistanceTo(points[^1]) < 1e-9 ? count - 1 : count;
        var curve = new PolyCurve();
        int segments = ring.Closed ? distinct : distinct - 1;
        for (int i = 0; i < segments; i++)
        {
            Point3d start = points[i], end = points[(i + 1) % distinct];
            if (start.DistanceTo(end) < 1e-9) continue;
            double angle = ring.ArcAngles[i];
            if (Math.Abs(angle) < 1e-10)
                curve.Append(new LineCurve(start, end));
            else
            {
                double bulge = Math.Tan(angle / 4);
                var middle = new Point3d((start.X + end.X) / 2 - (end.Y - start.Y) * bulge / 2,
                    (start.Y + end.Y) / 2 + (end.X - start.X) * bulge / 2, 0);
                curve.Append(new ArcCurve(new Arc(start, middle, end)));
            }
        }
        return curve;
    }
    protected override AcContourSet Convert(AcElementRef source, string mode, JsonElement[] items) => new(source,
        items.Select(item => new AcContourRing(item.GetProperty("role").GetString() ?? "",
            item.GetProperty("closed").GetBoolean(),
            item.GetProperty("xy").EnumerateArray().Select(value => value.GetDouble()).ToArray(),
            item.GetProperty("arcs").EnumerateArray().Select(value => value.GetDouble()).ToArray())).ToArray());
}

[IoId("1aca73bb-0231-4a9c-921e-37aba8546155")]
public sealed class ArchicadGetRelationships : ArchicadElementQuery<AcRelationSet>
{
    public ArchicadGetRelationships() : base(new Nomen("Get Relationships", "Read typed, native wall-hosted opening edges.", "Tapioca", "Deconstruct")) { }
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
    public ArchicadGetProperties() : base(new Nomen("Get Properties", "Discover available definitions or extract selected Archicad properties.", "Tapioca", "Deconstruct")) { }
    public ArchicadGetProperties(IReader reader) : base(reader) { }
    protected override string Kind => "properties";
    protected override string DefaultMode => "Element settings";
    protected override string[] Modes => ["Element settings", "Discover", "All", "User", "Built-in"];
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
                property.DefinitionId == Guid.Empty ? $"{property.Group} / {property.Name}" :
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
    public ArchicadGetGeometry() : base(new Nomen("Get Geometry", "Read 3D bounds/mesh or evaluated 2D drawing on demand.", "Tapioca", "Deconstruct")) { }
    public ArchicadGetGeometry(IReader reader) : base(reader) { }
    protected override string Kind => "geometry";
    protected override string DefaultMode => "Bounding box";
    protected override string[] Modes => ["Bounding box", "Surface mesh", "2D drawing"];
    protected override void AddResult(OutputAdder outputs) => outputs.Add(new AcGeometryParameter(
        "Geometry", "G", "Available 3D model bounds in project coordinates."));
    protected override void AddExtraOutputs(OutputAdder outputs)
    {
        outputs.AddText("Bounds", "B", "Minimum and maximum project-coordinate metres.", Access.Twig);
        outputs.AddMesh("Rhino Mesh", "M", "3D tessellation or a bounding-box display mesh, in Rhino units.", Access.Twig);
        outputs.AddCurve("2D Paths", "2D", "Evaluated Archicad floor-plan primitives at Z=0, in Rhino units.", Access.Twig);
    }
    protected override void SetExtraOutputs(IDataAccess access, AcGeometrySet[] values, Pear<AcElementRef>[] pears)
    {
        access.SetTwig(3, Garden.TwigFromPears(values.Select((value, i) => Garden.Pear(
            string.Join("\n", value.Bounds.Select(box =>
                $"({box.MinX:G6}, {box.MinY:G6}, {box.MinZ:G6}) – ({box.MaxX:G6}, {box.MaxY:G6}, {box.MaxZ:G6}) m")),
            pears[i].Meta ?? AcElementData.Provenance(pears[i].Item)))));
        double scale = RhinoMath.UnitScale(UnitSystem.Meters,
            RhinoDoc.ActiveDoc?.ModelUnitSystem ?? UnitSystem.Meters);
        access.SetTwig(4, Garden.TwigFromPears(values.SelectMany((value, i) =>
            value.Meshes.Select(source => Garden.Pear(ToRhinoMesh(source, scale), SourceMeta(pears[i])))
                .Concat(value.Bounds.Select(box => Garden.Pear(
                    Mesh.CreateFromBox(new BoundingBox(new Point3d(box.MinX * scale, box.MinY * scale, box.MinZ * scale),
                        new Point3d(box.MaxX * scale, box.MaxY * scale, box.MaxZ * scale)), 1, 1, 1),
                    SourceMeta(pears[i])))))));
        access.SetTwig(5, Garden.TwigFromPears(values.SelectMany((value, i) => value.Paths.Select(path =>
            Garden.Pear<Curve>(new PolylineCurve(Enumerable.Range(0, path.Xy.Length / 2).Select(index =>
                new Point3d(path.Xy[2 * index] * scale, path.Xy[2 * index + 1] * scale, 0))
                    .Concat(path.Closed ? [new Point3d(path.Xy[0] * scale, path.Xy[1] * scale, 0)] : [])),
                SourceMeta(pears[i]))))));
    }
    private static Mesh ToRhinoMesh(AcSurfaceMesh source, double scale)
    {
        if (source.Vertices.Length % 3 != 0 || source.Triangles.Length % 3 != 0 ||
            source.Vertices.Length > 12288 || source.Triangles.Length > 24576 ||
            source.Triangles.Any(index => index < 0 || index >= source.Vertices.Length / 3))
            throw new InvalidDataException("Archicad returned an invalid tessellated mesh.");
        var mesh = new Mesh();
        for (int i = 0; i < source.Vertices.Length; i += 3)
            mesh.Vertices.Add(source.Vertices[i] * scale, source.Vertices[i + 1] * scale,
                source.Vertices[i + 2] * scale);
        for (int i = 0; i < source.Triangles.Length; i += 3)
            mesh.Faces.AddFace(source.Triangles[i], source.Triangles[i + 1], source.Triangles[i + 2]);
        mesh.Normals.ComputeNormals();
        return mesh;
    }
    protected override AcGeometrySet Convert(AcElementRef source, string mode, JsonElement[] items) => new(source, mode,
        mode == "Bounding box" ? items.Select(item => new AcBounds(item.GetProperty("minX").GetDouble(),
            item.GetProperty("minY").GetDouble(), item.GetProperty("minZ").GetDouble(),
            item.GetProperty("maxX").GetDouble(), item.GetProperty("maxY").GetDouble(),
            item.GetProperty("maxZ").GetDouble())).ToArray() : [],
        mode == "Surface mesh" ? items.Select(item => new AcSurfaceMesh(
            item.GetProperty("vertices").EnumerateArray().Select(value => value.GetDouble()).ToArray(),
            item.GetProperty("triangles").EnumerateArray().Select(value => value.GetInt32()).ToArray())).ToArray() : [],
        mode == "2D drawing" ? items.Select(item => new AcDrawingPath(
            item.GetProperty("xy").EnumerateArray().Select(value => value.GetDouble()).ToArray(),
            item.GetProperty("closed").GetBoolean())).ToArray() : []);
}

[IoId("859af4c8-e7dd-484e-a03c-338d7574b50f")]
public sealed class ArchicadGdlParameters : ArchicadElementQuery<AcGdlSet>
{
    public ArchicadGdlParameters() : base(new Nomen("GDL Parameters & Settings", "Read placed GDL parameters, descriptions, groups and display flags; no edits.", "Tapioca", "Deconstruct")) { }
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
