using System.Text.Json;
using Tapioca.Gh2Worker;

namespace TapiocaGH2;

// A copied project snapshot. Light project-stamp checks may flag it as stale;
// only an explicit refresh reads the catalogs. Component solves never use IO.
internal static class ArchicadProjectOptions
{
    internal readonly record struct Story(int Index, string Name, double Level);
    internal sealed record Snapshot(string[] Layers, string[] LineTypes, Story[] Stories);
    [Flags]
    internal enum Changed { None = 0, Layers = 1, LineTypes = 2, Stories = 4, All = 7 }
    internal readonly record struct Status(bool Stale, bool Checking, long Revision, DateTimeOffset? CheckedAt,
        Changed LastChanged, string Issue, bool Monitoring);

    private static readonly object Sync = new();
    private static Snapshot current = new([], [], []);
    private static string identity = "";
    private static bool stale = true;
    private static bool invalid = true;
    private static bool checking;
    private static long? modelStamp;
    private static long? selectionStamp;
    private static long revision;
    private static long epoch;
    private static DateTimeOffset? checkedAt;
    private static Changed lastChanged;
    private static string issue = "Not connected";
    internal static event Action<Changed, Status>? Updated;

    internal static Snapshot Read()
    {
        lock (Sync)
            return invalid ? new([], [], []) : current;
    }

    internal static string? CurrentIdentity()
    {
        lock (Sync)
            return invalid ? null : identity;
    }

    internal static Status ReadStatus()
    {
        lock (Sync)
            return new(stale, checking, revision, checkedAt, lastChanged, issue,
                modelStamp.HasValue && selectionStamp.HasValue);
    }

    internal static void Clear()
    {
        lock (Sync)
        {
            ++epoch;
            ++revision;
            current = new([], [], []);
            identity = "";
            stale = true;
            invalid = true;
            checking = false;
            modelStamp = null;
            selectionStamp = null;
            checkedAt = null;
            lastChanged = Changed.All;
            issue = "Not connected";
        }
        Updated?.Invoke(Changed.All, ReadStatus());
    }

    internal static void Set(Snapshot snapshot, string projectIdentity = "", long? stamp = null,
        long? selected = null)
    {
        lock (Sync)
        {
            current = snapshot;
            identity = projectIdentity;
            stale = false;
            invalid = false;
            checking = false;
            modelStamp = stamp;
            selectionStamp = selected;
            checkedAt = DateTimeOffset.UtcNow;
            lastChanged = Changed.All;
            issue = "";
            ++revision;
        }
        Updated?.Invoke(Changed.All, ReadStatus());
    }

    internal static string ParseIdentity(string json)
    {
        using JsonDocument document = JsonDocument.Parse(json);
        JsonElement data = Data(document.RootElement);
        if (!data.TryGetProperty("projectPath", out JsonElement path) || path.ValueKind != JsonValueKind.String ||
            !data.TryGetProperty("projectName", out JsonElement name) || name.ValueKind != JsonValueKind.String ||
            !data.TryGetProperty("untitled", out JsonElement untitled) ||
            (untitled.ValueKind != JsonValueKind.True && untitled.ValueKind != JsonValueKind.False))
            throw new InvalidDataException("Archicad project identity fields are missing.");
        // A comparison key, NOT a project GUID. Identical untitled projects
        // cannot be distinguished until Tapioca has a project-bound identity.
        return JsonSerializer.Serialize(new { Path = path.GetString(), Name = name.GetString(),
            Untitled = untitled.GetBoolean() });
    }

    internal static long? ParseStamp(string json) => ParseStampField(json, "modelStamp");

    internal static long? ParseSelectionStamp(string json) => ParseStampField(json, "selectionStamp");

    private static long? ParseStampField(string json, string field)
    {
        using JsonDocument document = JsonDocument.Parse(json);
        JsonElement data = Data(document.RootElement);
        if (!data.TryGetProperty(field, out JsonElement stamp))
            return null; // older native builds may not provide this hint
        if (stamp.ValueKind != JsonValueKind.Number || !stamp.TryGetInt64(out long value))
            throw new InvalidDataException($"Archicad returned an invalid {field}.");
        return value;
    }

    internal static async Task CheckForChangesAsync(Gh2PipeClient bridge)
    {
        long token;
        lock (Sync)
        {
            if (stale || checking)
                return; // one warning per snapshot until an explicit refresh
            token = epoch;
        }
        try
        {
            string response = await bridge.ReadProjectInfoAsync();
            string project = ParseIdentity(response);
            long? stamp = ParseStamp(response);
            long? selected = ParseSelectionStamp(response);
            Changed changed = Changed.None;
            lock (Sync)
            {
                if (stale || checking || epoch != token)
                    return;
                if (identity == project)
                {
                    bool selectionChanged = selected.HasValue && selectionStamp.HasValue && selected != selectionStamp;
                    if (selectionChanged)
                        selectionStamp = selected;
                    if (!stamp.HasValue || !modelStamp.HasValue || !selected.HasValue ||
                        !selectionStamp.HasValue || stamp == modelStamp || selectionChanged)
                    {
                        // ACAPI's project stamp also advances on selection-only
                        // UI edits. Keep the snapshot and accept that stamp as
                        // the new baseline; the next real edit still alerts.
                        if (stamp.HasValue)
                            modelStamp = stamp;
                        return;
                    }
                }
                stale = true;
                if (identity != project)
                {
                    invalid = true;
                    current = new([], [], []);
                    changed = Changed.All;
                    issue = "Archicad changed projects; refresh the snapshot";
                }
                else
                    issue = "Archicad model changed; refresh to check project choices";
            }
            Updated?.Invoke(changed, ReadStatus());
        }
        catch (Exception error)
        {
            lock (Sync)
            {
                if (stale || checking || epoch != token)
                    return;
                stale = true;
                invalid = true; // project identity could not be checked
                issue = "Archicad model check failed: " + error.Message;
            }
            Updated?.Invoke(Changed.All, ReadStatus());
        }
    }

    internal static long BeginCheck()
    {
        long token;
        lock (Sync)
        {
            checking = true;
            issue = "Checking Archicad project options";
            token = ++epoch;
        }
        Updated?.Invoke(Changed.None, ReadStatus());
        return token;
    }

    private static void InvalidateChangedProject(long token, string nextIdentity)
    {
        lock (Sync)
        {
            if (epoch != token || identity == nextIdentity)
                return;
            // Values from the previous project must not be selectable while
            // the new project's lists are being read.
            current = new([], [], []);
            stale = true;
            invalid = true;
            issue = "Archicad changed projects; loading its choices";
        }
        Updated?.Invoke(Changed.All, ReadStatus());
    }

    internal static async Task RefreshAsync(Gh2PipeClient bridge, long token)
    {
        try
        {
            string first = await bridge.ReadProjectInfoAsync();
            string before = ParseIdentity(first);
            long? beforeStamp = ParseStamp(first);
            InvalidateChangedProject(token, before);
            string[] layers = ParseAttributes(await bridge.ReadAttributesAsync("layer"), "layer");
            string[] lineTypes = ParseAttributes(await bridge.ReadAttributesAsync("lineType"), "lineType");
            Story[] stories = ParseStories(await bridge.ReadStoriesAsync());
            string last = await bridge.ReadProjectInfoAsync();
            string after = ParseIdentity(last);
            long? afterStamp = ParseStamp(last);
            long? afterSelection = ParseSelectionStamp(last);
            // Selection may account for a stamp change, but a simultaneous
            // catalog edit is indistinguishable here. Never publish lists read
            // across a changed stamp; ask for another explicit refresh.
            if (before != after || (beforeStamp.HasValue && afterStamp.HasValue && beforeStamp != afterStamp))
                throw new InvalidDataException("Archicad changed while refreshing GH2 choices; retry.");

            Changed changes;
            lock (Sync)
            {
                if (epoch != token)
                    return; // detached or superseded; never republish an old project
                bool projectChanged = identity != after;
                changes = Changed.None;
                if (projectChanged || !current.Layers.SequenceEqual(layers, StringComparer.Ordinal))
                {
                    current = current with { Layers = layers };
                    changes |= Changed.Layers;
                }
                if (projectChanged || !current.LineTypes.SequenceEqual(lineTypes, StringComparer.Ordinal))
                {
                    current = current with { LineTypes = lineTypes };
                    changes |= Changed.LineTypes;
                }
                if (projectChanged || !current.Stories.SequenceEqual(stories))
                {
                    current = current with { Stories = stories };
                    changes |= Changed.Stories;
                }
                identity = after;
                stale = false;
                invalid = false;
                checking = false;
                modelStamp = afterStamp;
                selectionStamp = afterSelection;
                issue = "";
                checkedAt = DateTimeOffset.UtcNow;
                lastChanged = changes;
                if (changes != Changed.None)
                    ++revision;
            }
            Updated?.Invoke(changes, ReadStatus());
        }
        catch (Exception error)
        {
            lock (Sync)
            {
                if (epoch != token)
                    return;
                stale = true;
                invalid = true;
                checking = false;
                issue = error.Message;
            }
            Updated?.Invoke(Changed.All, ReadStatus());
        }
    }

    internal static Snapshot Fetch(Gh2PipeClient bridge) => new(
        ParseAttributes(bridge.ReadAttributes("layer"), "layer"),
        ParseAttributes(bridge.ReadAttributes("lineType"), "lineType"),
        ParseStories(bridge.ReadStories()));

    internal static string[] ParseAttributes(string json, string kind)
    {
        using JsonDocument document = JsonDocument.Parse(json);
        JsonElement data = Data(document.RootElement);
        if (!data.TryGetProperty("kind", out JsonElement type) || type.ValueKind != JsonValueKind.String ||
            type.GetString() != kind ||
            !data.TryGetProperty("attributes", out JsonElement attributes) ||
            attributes.ValueKind != JsonValueKind.Array || attributes.GetArrayLength() > 4096)
            throw new InvalidDataException($"Archicad {kind} choices have an invalid kind or count.");
        return attributes.EnumerateArray().Select(row =>
        {
            if (row.ValueKind != JsonValueKind.Object ||
                !row.TryGetProperty("name", out JsonElement name) || name.ValueKind != JsonValueKind.String ||
                string.IsNullOrWhiteSpace(name.GetString()))
                throw new InvalidDataException($"Archicad {kind} choices contain an unnamed attribute.");
            return name.GetString()!;
        }).ToArray();
    }

    internal static Story[] ParseStories(string json)
    {
        using JsonDocument document = JsonDocument.Parse(json);
        JsonElement data = Data(document.RootElement);
        if (!data.TryGetProperty("indices", out JsonElement indices) || indices.ValueKind != JsonValueKind.Array ||
            !data.TryGetProperty("names", out JsonElement names) || names.ValueKind != JsonValueKind.Array ||
            !data.TryGetProperty("levels", out JsonElement levels) || levels.ValueKind != JsonValueKind.Array ||
            indices.GetArrayLength() != names.GetArrayLength() ||
            indices.GetArrayLength() != levels.GetArrayLength() || indices.GetArrayLength() > 4096)
            throw new InvalidDataException("Archicad stories have mismatched or excessive arrays.");
        var result = new Story[indices.GetArrayLength()];
        for (int i = 0; i < result.Length; i++)
        {
            if (indices[i].ValueKind != JsonValueKind.Number || !indices[i].TryGetInt32(out int index) ||
                names[i].ValueKind != JsonValueKind.String || levels[i].ValueKind != JsonValueKind.Number ||
                !levels[i].TryGetDouble(out double level) || !double.IsFinite(level))
                throw new InvalidDataException($"Archicad story {i} has an invalid index, name or level.");
            result[i] = new Story(index, names[i].GetString()!, level);
        }
        return result;
    }

    private static JsonElement Data(JsonElement root)
    {
        if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("ok", out JsonElement ok) ||
            ok.ValueKind != JsonValueKind.True || !root.TryGetProperty("data", out JsonElement data) ||
            data.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException("Archicad refused the GH2 project-options snapshot.");
        return data;
    }
}
