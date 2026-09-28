using System.Diagnostics;
using System.Globalization;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Tapioca.Gh2Worker;

// A bounded channel shared by worker and attached peer. The peer owns
// project reads and explicit UI selection actions; solves never use this pipe.
internal sealed class Gh2PipeClient : IDisposable
{
    // GS::ObjectState's Array<double> parser treats a JSON integer token as 0.
    // Serialize every mesh coordinate as a real token, including 0.0 and 10.0.
    private sealed class RealTokenConverter : JsonConverter<double>
    {
        public override double Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options) =>
            reader.GetDouble();
        public override void Write(Utf8JsonWriter writer, double value, JsonSerializerOptions options)
        {
            string text = value.ToString("R", CultureInfo.InvariantCulture);
            if (!text.Contains('.') && !text.Contains('E') && !text.Contains('e')) text += ".0";
            writer.WriteRawValue(text);
        }
    }
    private static readonly JsonSerializerOptions MeshJsonOptions = new()
    {
        Converters = { new RealTokenConverter() }
    };
    private readonly NamedPipeClientStream pipe;
    private readonly object writes = new();
    private readonly Timer heartbeat;
    private readonly Timer monitorTimer;
    private readonly SemaphoreSlim projectReads = new(1, 1);
    private readonly object requests = new();
    private TaskCompletionSource<string>? pendingReply;
    private uint pendingId;
    private Func<Task>? refresh;
    private Func<Task>? monitor;
    private int refreshing;
    private int monitoring;
    private uint nextRequestId;
    private long lastHeartbeatStamp;
    private int heartbeatFailed;
    internal bool StartupAcknowledged { get; private set; }

    private Gh2PipeClient(NamedPipeClientStream pipe)
    {
        this.pipe = pipe;
        lastHeartbeatStamp = Stopwatch.GetTimestamp();
        heartbeat = new Timer(_ =>
        {
            try
            {
                Send(Gh2Wire.Message.Heartbeat, 0, 0, []);
                Interlocked.Exchange(ref lastHeartbeatStamp, Stopwatch.GetTimestamp());
            }
            catch (IOException) { Interlocked.Exchange(ref heartbeatFailed, 1); }
            catch (ObjectDisposedException) { Interlocked.Exchange(ref heartbeatFailed, 1); }
        }, null, Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
        monitorTimer = new Timer(_ => RequestMonitor(), null, Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
    }

    // One-way heartbeat writes only prove that the local pipe accepted data;
    // they are not an Archicad-side health acknowledgement.
    internal string HeartbeatHealth
    {
        get
        {
            if (Volatile.Read(ref heartbeatFailed) != 0)
                return "Degraded: local pipe heartbeat write failed";
            try
            {
                if (!pipe.IsConnected)
                    return "Disconnected: local pipe closed";
            }
            catch (ObjectDisposedException) { return "Disconnected: local pipe closed"; }
            double seconds = Stopwatch.GetElapsedTime(Interlocked.Read(ref lastHeartbeatStamp)).TotalSeconds;
            return seconds <= 10.0 ? $"Healthy: local pipe write {seconds:0}s ago"
                                   : $"Stale: no successful local pipe write for {seconds:0}s";
        }
    }

    internal static Gh2PipeClient Connect(string pipeName)
    {
        var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
        try
        {
            pipe.Connect(30_000);
            var client = new Gh2PipeClient(pipe);
            try
            {
                client.Send(Gh2Wire.Message.Hello, 0, 0, Gh2Wire.Hello(Environment.ProcessId));
                var (header, body) = client.Receive();
                if (header.Type != Gh2Wire.Message.HelloAck || header.RequestId != 0)
                    throw new InvalidDataException("The bridge did not answer GH2's hello.");
                Gh2Wire.VerifyHelloAck(body);
                // Read replies are flushed by the bridge IO thread on the next
                // message. 250 ms bounds each catalog read without an API pipe
                // write on Archicad's main thread.
                client.heartbeat.Change(TimeSpan.FromMilliseconds(250), TimeSpan.FromMilliseconds(250));
                return client;
            }
            catch
            {
                client.Dispose();
                throw;
            }
        }
        catch
        {
            pipe.Dispose();
            throw;
        }
    }

    internal void Startup(bool ready, string message)
    {
        if (StartupAcknowledged)
            throw new InvalidOperationException("The GH2 startup has already been acknowledged.");
        // Even a partially failed pipe write must never be retried as a second
        // startup Ack on a stream whose framing is no longer known.
        StartupAcknowledged = true;
        Send(Gh2Wire.Message.Ack, 0, 0, Gh2Wire.Ack(ready, message));
    }

    internal void Log(string text) =>
        Send(Gh2Wire.Message.Log, 0, 0, Encoding.UTF8.GetBytes(text));

    internal string? LastProjectInfoJson { get; private set; }
    internal string ReadProjectInfo()
    {
        string response = ReadProject("Tapioca.GetGhConnectionInfo", "{}");
        LastProjectInfoJson = response;
        return response;
    }

    internal string ReadStories() => ReadProject("Tapioca.GetStories", "{}");

    internal string ReadAttributes(string kind) => kind switch
    {
        "layer" => ReadProject("Tapioca.ListAttributes", "{\"kind\":\"layer\"}"),
        "lineType" => ReadProject("Tapioca.ListAttributes", "{\"kind\":\"lineType\"}"),
        _ => throw new ArgumentOutOfRangeException(nameof(kind))
    };

    private string ReadProject(string command, string parameters)
    {
        uint id = ++nextRequestId;
        Send(Gh2Wire.Message.ApiRequest, id, 0, Gh2Wire.ApiRequest(command, parameters));
        var (header, body) = Receive(command == "Tapioca.GetGhConnectionInfo"
            ? Gh2Wire.MaxPingResponse : Gh2Wire.MaxProjectOptionsResponse);
        if (header.Type == Gh2Wire.Message.Shutdown && body.Length == 0)
            throw new EndOfStreamException("Archicad detached during a project-options read.");
        if (header.Type != Gh2Wire.Message.ApiResponse || header.CorrelationId != id)
            throw new InvalidDataException("The project read was addressed to another request.");
        return Encoding.UTF8.GetString(body);
    }

    internal void ConfigureRefresh(Func<Task> callback) => refresh = callback;

    internal void ConfigureMonitor(Func<Task> callback)
    {
        monitor = callback;
        monitorTimer.Change(TimeSpan.FromSeconds(5), TimeSpan.FromSeconds(5));
    }

    private void RequestMonitor()
    {
        if (monitor is null || Volatile.Read(ref refreshing) != 0 ||
            Interlocked.CompareExchange(ref monitoring, 1, 0) != 0)
            return;
        _ = Task.Run(async () =>
        {
            try { await monitor(); }
            catch (Exception error) { LogRefreshFailure(error.Message); }
            finally { Interlocked.Exchange(ref monitoring, 0); }
        });
    }

    // Process() only signals this bounded task. All pipe reads remain on the
    // single WaitForShutdown reader; no GH solver iteration blocks on ACAPI.
    internal bool RequestRefresh()
    {
        if (refresh is null || Interlocked.CompareExchange(ref refreshing, 1, 0) != 0)
            return false;
        _ = Task.Run(async () =>
        {
            try { await refresh(); }
            catch (Exception error) { LogRefreshFailure(error.Message); }
            finally { Interlocked.Exchange(ref refreshing, 0); }
        });
        return true;
    }

    private void LogRefreshFailure(string message)
    {
        try { Log("GH2 project options refresh failed: " + message); }
        catch (IOException) { }
        catch (ObjectDisposedException) { }
    }

    internal Task<string> ReadProjectInfoAsync() => ReadProjectAsync("Tapioca.GetGhConnectionInfo", "{}");
    internal Task<string> ReadStoriesAsync() => ReadProjectAsync("Tapioca.GetStories", "{}");
    internal Task<string> ReadSelectionAsync() => ReadProjectAsync("Tapioca.GetSelection", "{}");
    internal Task<string> ReadElementHeadersAsync(IReadOnlyList<Guid> ids)
    {
        if (ids.Count > 64 || ids.Any(id => id == Guid.Empty))
            throw new ArgumentOutOfRangeException(nameof(ids));
        return ReadProjectAsync("Tapioca.GetGhElementHeaders",
            JsonSerializer.Serialize(new { guids = ids.Select(id => id.ToString("D")) }));
    }
    internal Task<string> ReadElementQueryAsync(IReadOnlyList<Guid> ids, string kind, string mode,
        string search, IReadOnlyList<string> selectors, int offset = 0)
    {
        if (ids.Count > 8 || ids.Any(id => id == Guid.Empty) || selectors.Count > 64 ||
            selectors.Any(value => string.IsNullOrWhiteSpace(value) || value.Length > 128) ||
            !new[] { "contours", "relationships", "properties", "geometry", "gdl" }.Contains(kind) ||
            mode.Length > 64 || search.Length > 128 || offset < 0 || offset > 2048 || offset % 128 != 0)
            throw new ArgumentOutOfRangeException(nameof(ids), "Invalid bounded element query.");
        return ReadProjectAsync("Tapioca.GetGhElementQuery", JsonSerializer.Serialize(new
        {
            guids = ids.Select(id => id.ToString("D")), kind, mode, search, selectors, offset
        }));
    }
    internal Task<string> ReplaceSelectionAsync(IReadOnlyList<Guid> ids)
    {
        if (ids.Count > 4096)
            throw new ArgumentOutOfRangeException(nameof(ids));
        string request = JsonSerializer.Serialize(new
        {
            elements = ids.Select(id => new { elementId = new { guid = id.ToString("D") } }),
            add = false
        });
        return ReadProjectAsync("Tapioca.SetSelection", request);
    }
    internal Task<string> CreateTerrainMeshAsync(double[] outline, double[] polyZ,
        double baseLevel, double[] ridgeCoords, int[] ridgeCounts, string layer)
    {
        if (outline.Length is < 6 or > 512 || outline.Length % 2 != 0 ||
            polyZ.Length != outline.Length / 2 || ridgeCoords.Length > 1536 ||
            ridgeCoords.Length % 3 != 0 || ridgeCounts.Length > 512 ||
            ridgeCounts.Any(count => count is < 2 or > 256) || ridgeCounts.Sum() != ridgeCoords.Length / 3 ||
            layer.Length > 128 ||
            !outline.Concat(polyZ).Concat(ridgeCoords).Append(baseLevel).All(double.IsFinite))
            throw new ArgumentOutOfRangeException(nameof(outline), "Terrain mesh exceeds the bounded bake contract.");
        return ReadProjectAsync("Tapioca.CreateMesh", JsonSerializer.Serialize(new
        {
            outline, polyZ, baseLevel, ridgeCoords, ridgeCounts, floorInd = 0,
            skirt = "SurfaceOnlyWithoutSkirt", skirtLevel = 0.0, onFloorPlan = true, layer
        }, MeshJsonOptions));
    }
    internal Task<string> ReadAttributesAsync(string kind) => kind switch
    {
        "layer" => ReadProjectAsync("Tapioca.ListAttributes", "{\"kind\":\"layer\"}"),
        "lineType" => ReadProjectAsync("Tapioca.ListAttributes", "{\"kind\":\"lineType\"}"),
        _ => throw new ArgumentOutOfRangeException(nameof(kind))
    };

    private async Task<string> ReadProjectAsync(string command, string parameters)
    {
        await projectReads.WaitAsync();
        try { return await RequestProjectAsync(command, parameters).WaitAsync(
            TimeSpan.FromSeconds(command == "Tapioca.CreateMesh" ? 120 : 30)); }
        catch (TimeoutException)
        {
            // A late reply cannot safely be mistaken for the next request.
            // Tear down this pipe rather than leave its reader and refresh latch stuck.
            Dispose();
            if (command == "Tapioca.CreateMesh")
                throw new TimeoutException("Mesh creation outcome is unknown. Inspect Archicad before retrying; the write may have committed.");
            throw;
        }
        finally { projectReads.Release(); }
    }

    private Task<string> RequestProjectAsync(string command, string parameters)
    {
        TaskCompletionSource<string> completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
        uint id;
        lock (requests)
        {
            if (pendingReply is not null)
                throw new InvalidOperationException("Another GH2 project read is in flight.");
            id = ++nextRequestId;
            pendingId = id;
            pendingReply = completion;
        }
        try { Send(Gh2Wire.Message.ApiRequest, id, 0, Gh2Wire.ApiRequest(command, parameters)); }
        catch
        {
            CompletePending(new IOException("Could not send the GH2 project read."));
            throw;
        }
        return completion.Task;
    }

    private void CompletePending(Exception error)
    {
        TaskCompletionSource<string>? completion;
        lock (requests)
        {
            completion = pendingReply;
            pendingReply = null;
        }
        completion?.TrySetException(error);
    }

    internal void WaitForShutdown(bool acknowledgeShutdown = true)
    {
        try
        {
            while (true)
            {
                var (header, body) = Receive(Gh2Wire.MaxProjectOptionsResponse);
                if (header.Type == Gh2Wire.Message.ApiResponse)
                {
                    TaskCompletionSource<string>? completion;
                    lock (requests)
                    {
                        if (pendingReply is null || header.CorrelationId != pendingId)
                            throw new InvalidDataException("Unsolicited or misaddressed GH2 project reply.");
                        completion = pendingReply;
                        pendingReply = null;
                    }
                    completion.TrySetResult(Encoding.UTF8.GetString(body));
                    continue;
                }
                if (header.Type == Gh2Wire.Message.Shutdown && body.Length == 0)
                {
                    if (acknowledgeShutdown)
                        Send(Gh2Wire.Message.Ack, 0, 0, Gh2Wire.Ack(true, "GH2 worker shutting down."));
                    return;
                }
                throw new InvalidDataException($"GH2 does not yet support message {header.Type}.");
            }
        }
        finally { CompletePending(new EndOfStreamException("The GH2 bridge closed during a project refresh.")); }
    }

    private void Send(Gh2Wire.Message type, uint requestId, uint correlationId, byte[] body)
    {
        byte[] header = Gh2Wire.EncodeHeader(type, requestId, correlationId, body.Length);
        lock (writes)
        {
            pipe.Write(header);
            if (body.Length != 0)
                pipe.Write(body);
            pipe.Flush();
        }
    }

    private (Gh2Wire.Header, byte[]) Receive(int limit = Gh2Wire.MaxPingResponse)
    {
        byte[] headerBytes = new byte[Gh2Wire.HeaderSize];
        pipe.ReadExactly(headerBytes);
        Gh2Wire.Header header = Gh2Wire.DecodeHeader(headerBytes);
        if (header.PayloadBytes > limit)
            throw new InvalidDataException("The GH2 bridge refused an oversized response.");
        byte[] body = new byte[header.PayloadBytes];
        if (body.Length != 0)
            pipe.ReadExactly(body);
        return (header, body);
    }

    public void Dispose()
    {
        CompletePending(new ObjectDisposedException(nameof(Gh2PipeClient)));
        heartbeat.Dispose();
        monitorTimer.Dispose();
        pipe.Dispose();
    }
}
