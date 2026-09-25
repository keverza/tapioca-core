using System.Diagnostics;
using System.IO.Pipes;
using System.Text;

namespace Tapioca.Gh2Worker;

// A bounded, read-only bootstrap channel shared by worker and attached peer.
// GH2 components never hold it; the fixed pre-solve Ping is its only API request.
internal sealed class Gh2PipeClient : IDisposable
{
    private readonly NamedPipeClientStream pipe;
    private readonly object writes = new();
    private readonly Timer heartbeat;
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
                client.heartbeat.Change(TimeSpan.FromSeconds(3), TimeSpan.FromSeconds(3));
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

    internal string ReadProjectInfo()
    {
        uint id = ++nextRequestId;
        Send(Gh2Wire.Message.ApiRequest, id, 0, Gh2Wire.ApiRequest("Tapioca.GetGhConnectionInfo", "{}"));
        var (header, body) = Receive();
        if (header.Type != Gh2Wire.Message.ApiResponse || header.CorrelationId != id ||
            body.Length > Gh2Wire.MaxPingResponse)
            throw new InvalidDataException("The project-info reply is oversized or addressed to another request.");
        return Encoding.UTF8.GetString(body);
    }

    internal void WaitForShutdown(bool acknowledgeShutdown = true)
    {
        while (true)
        {
            var (header, body) = Receive();
            if (header.Type == Gh2Wire.Message.Shutdown && body.Length == 0)
            {
                if (acknowledgeShutdown)
                    Send(Gh2Wire.Message.Ack, 0, 0, Gh2Wire.Ack(true, "GH2 worker shutting down."));
                return;
            }
            // A GH1 session request must never be interpreted as a GH2 input
            // snapshot. This early transport slice accepts shutdown only.
            throw new InvalidDataException($"GH2 does not yet support message {header.Type}.");
        }
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

    private (Gh2Wire.Header, byte[]) Receive()
    {
        byte[] headerBytes = new byte[Gh2Wire.HeaderSize];
        pipe.ReadExactly(headerBytes);
        Gh2Wire.Header header = Gh2Wire.DecodeHeader(headerBytes);
        if (header.PayloadBytes > Gh2Wire.MaxPingResponse)
            throw new InvalidDataException("The GH2 bootstrap channel refused an oversized response.");
        byte[] body = new byte[header.PayloadBytes];
        if (body.Length != 0)
            pipe.ReadExactly(body);
        return (header, body);
    }

    public void Dispose()
    {
        heartbeat.Dispose();
        pipe.Dispose();
    }
}
