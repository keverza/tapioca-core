using System.Buffers.Binary;
using System.IO.Pipes;
using System.Text;
using TapiocaGH2;

namespace Tapioca.Gh2Worker;

// Exercises the real named-pipe stream with an independent little-endian host.
// No Rhino, Archicad, GH1 assembly or UI is needed for this framing check.
internal static class Gh2WireCheck
{
    internal static int Run()
    {
        RoundTrip();
        AcceptedProjectInfo("{\"ok\":true,\"data\":{\"archicadVersion\":29,\"archicadBuild\":0,\"projectName\":\"\",\"untitled\":false,\"projectPath\":\"private/project/path\"}}",
            "Archicad 29 build unavailable, project 'project name unavailable'.");
        AcceptedProjectInfo("{\"ok\":true,\"data\":{\"archicadVersion\":29,\"archicadBuild\":0,\"projectName\":\"  \",\"untitled\":true}}",
            "Archicad 29 build unavailable, project 'untitled'.");
        AcceptedProjectInfo("{\"ok\":true,\"data\":{\"archicadVersion\":29,\"archicadBuild\":3000,\"projectName\":\"Teamwork test\",\"untitled\":false}}",
            "Archicad 29 build 3000, project 'Teamwork test'.");
        RejectedProjectInfo("{\"ok\":false,\"error\":\"No project is open. Open a project and retry.\"}",
            "No project is open. Open a project and retry.");
        RejectedProjectInfo("{\"ok\":false,\"error\":{\"code\":\"AcapiFailed\",\"message\":\"Native project read failed.\",\"detail\":\"Tapioca.GetGhConnectionInfo\"}}",
            "[AcapiFailed]: Native project read failed. (detail: Tapioca.GetGhConnectionInfo)");
        RejectedProjectInfo("{\"ok\":true,\"data\":{\"archicadVersion\":0,\"archicadBuild\":0,\"projectName\":\"\"}}",
            "archicadVersion is invalid (non-positive (0))");
        RejectedProjectInfo("{\"ok\":true,\"data\":{\"archicadVersion\":\"private/project/path\",\"archicadBuild\":0,\"projectName\":\"\"}}",
            "archicadVersion is invalid (String)");
        RejectedProjectInfo("{\"ok\":true,\"data\":{\"archicadVersion\":29,\"archicadBuild\":\"3000\",\"projectName\":\"\"}}",
            "archicadBuild must be a non-negative Int32 integer");
        RejectedProjectInfo("{\"ok\":true,\"data\":{\"archicadVersion\":29,\"archicadBuild\":0,\"projectName\":null}}",
            "projectName must be a string");
        RefusedGh1Bridge();
        OversizedAnswer();
        PeerAttachAndDetach();
        PeerRefusedPing();
        PeerLostConnection();
        Console.WriteLine("GH2 named-pipe startup ordering, project refusal and shutdown checks passed.");
        return 0;
    }

    private static void AcceptedProjectInfo(string reply, string expectedLog)
    {
        string name = "Tapioca.Gh2.ProjectAccepted." + Guid.NewGuid().ToString("N");
        using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);
        Task host = Host(server, () =>
        {
            server.WaitForConnection();
            var (hello, payload) = Receive(server);
            Require(hello.type == 1 && payload.Length == 8 && Read(payload, 4) == Gh2Wire.CapabilityGh2,
                "No GH2 hello arrived for the accepted project check.");
            Send(server, 2, 0, 0, Word(Gh2Wire.CapabilityGh2));
            uint requestId = ExpectPing(server);
            RequireNoStartupBeforeReply(server);
            Send(server, 5, 0, requestId, Encoding.UTF8.GetBytes(reply));
            var (startup, body) = ReceiveControl(server);
            Require(startup.type == 9 && body.Length >= 4 && Read(body, 0) == 0,
                "The worker did not acknowledge valid project info.");
            var (log, logPayload) = ReceiveControl(server);
            string text = Encoding.UTF8.GetString(logPayload);
            Require(log.type == 10 && text.Contains(expectedLog, StringComparison.Ordinal) &&
                    !text.Contains("private/project/path", StringComparison.Ordinal),
                "The worker did not report the validated project without its path.");
            Send(server, 8, 0, 0, []);
            var (shutdown, answer) = ReceiveControl(server);
            Require(shutdown.type == 9 && answer.Length >= 4 && Read(answer, 0) == 0,
                "The worker did not acknowledge shutdown.");
            Require(server.ReadByte() == -1, "The worker did not disconnect after shutdown.");
        });
        using (var client = Gh2PipeClient.Connect(name))
        {
            Gh2Ping.ConfirmStartup(client, 17, "Rhino 9 / GH2 ready");
            client.WaitForShutdown();
        }
        host.GetAwaiter().GetResult();
    }

    private static void RoundTrip()
    {
        string name = "Tapioca.Gh2.Check." + Guid.NewGuid().ToString("N");
        using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);
        Task host = Host(server, () =>
        {
            server.WaitForConnection();
            var (hello, payload) = Receive(server);
            Require(hello.type == 1 && payload.Length == 8, "No GH2 hello arrived.");
            Require(Read(payload, 0) != 0 && Read(payload, 4) == Gh2Wire.CapabilityGh2,
                "GH2 did not offer exactly its engine capability.");
            Send(server, 2, 0, 0, Word(Gh2Wire.CapabilityGh2));

            uint requestId = ExpectPing(server);
            RequireNoStartupBeforeReply(server);
            Send(server, 5, 0, requestId,
                Encoding.UTF8.GetBytes("{\"ok\":true,\"data\":{\"archicadVersion\":29,\"archicadBuild\":3000,\"projectName\":\"GH2 test\"}}"));
            var (startup, startupPayload) = ReceiveControl(server);
            Require(startup.type == 9 && startup.requestId == 0 && startupPayload.Length >= 4 &&
                    Read(startupPayload, 0) == 0, "The worker did not acknowledge valid startup.");
            var (log, logPayload) = ReceiveControl(server);
            Require(log.type == 10 && Encoding.UTF8.GetString(logPayload).Contains("GH2 test", StringComparison.Ordinal),
                "The validated project was not logged.");
            Send(server, 8, 0, 0, []);
            var (shutdown, answer) = ReceiveControl(server);
            Require(shutdown.type == 9 && answer.Length >= 4 && Read(answer, 0) == 0 &&
                    Encoding.UTF8.GetString(answer, 4, answer.Length - 4).Contains("shutting down", StringComparison.Ordinal),
                "Worker did not acknowledge shutdown.");
            Require(server.ReadByte() == -1, "Worker did not disconnect after shutdown.");
        });

        using (var client = Gh2PipeClient.Connect(name))
        {
            Gh2Ping.ConfirmStartup(client, 17, "Rhino 9 / GH2 ready");
            client.WaitForShutdown();
            Gh2Ping.AcknowledgeStartupFailure(client, new InvalidDataException("Shutdown already acknowledged."));
        }
        host.GetAwaiter().GetResult();
    }

    private static void RejectedProjectInfo(string reply, string expectedReason)
    {
        string name = "Tapioca.Gh2.ProjectRefusal." + Guid.NewGuid().ToString("N");
        using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);
        Task host = Host(server, () =>
        {
            server.WaitForConnection();
            var (hello, payload) = Receive(server);
            Require(hello.type == 1 && payload.Length == 8 && Read(payload, 4) == Gh2Wire.CapabilityGh2,
                "No GH2 hello arrived for the project refusal check.");
            Send(server, 2, 0, 0, Word(Gh2Wire.CapabilityGh2));
            uint requestId = ExpectPing(server);
            RequireNoStartupBeforeReply(server);
            Send(server, 5, 0, requestId, Encoding.UTF8.GetBytes(reply));
            var (failure, body) = ReceiveControl(server);
            Require(failure.type == 9 && body.Length >= 4 && Read(body, 0) == 1 &&
                    Encoding.UTF8.GetString(body, 4, body.Length - 4).Contains(expectedReason, StringComparison.Ordinal),
                "The worker did not send one actionable failed startup ack.");
            Require(server.ReadByte() == -1, "The worker sent another startup ack after failure.");
        });
        using (var client = Gh2PipeClient.Connect(name))
        {
            try
            {
                Gh2Ping.ConfirmStartup(client, 17, "Rhino 9 / GH2 ready");
                throw new InvalidOperationException("An invalid project-info reply was accepted.");
            }
            catch (InvalidDataException error)
            {
                Require(error.Message.Contains(expectedReason, StringComparison.Ordinal),
                    "The native refusal or invalid project-info reason was lost.");
                Require(!error.Message.Contains("private/project/path", StringComparison.Ordinal),
                    "An invalid version leaked its raw value.");
                Gh2Ping.AcknowledgeStartupFailure(client, error);
            }
        }
        host.GetAwaiter().GetResult();
    }

    private static uint ExpectPing(Stream stream)
    {
        var (request, bytes) = ReceiveControl(stream);
        Require(request.type == 4 && request.requestId != 0 && request.correlationId == 0,
            "A startup ack arrived before the project-info API request.");
        Require(bytes.Length >= 8, "The project-info request is short.");
        int commandBytes = checked((int)Read(bytes, 0));
        int paramsBytes = checked((int)Read(bytes, 4));
        Require(8L + commandBytes + paramsBytes == bytes.Length, "API request lengths do not match.");
        Require(Encoding.UTF8.GetString(bytes, 8, commandBytes) == "Tapioca.GetGhConnectionInfo" &&
                Encoding.UTF8.GetString(bytes, 8 + commandBytes, paramsBytes) == "{}",
            "Ping asked for an unexpected command or payload.");
        return request.requestId;
    }

    private static void RequireNoStartupBeforeReply(NamedPipeServerStream server)
    {
        using var deadline = new CancellationTokenSource(TimeSpan.FromMilliseconds(150));
        try
        {
            server.ReadAsync(new byte[20], deadline.Token).AsTask().GetAwaiter().GetResult();
            throw new InvalidOperationException("The worker sent a frame before the API reply.");
        }
        catch (OperationCanceledException) when (deadline.IsCancellationRequested) { }
    }

    private static ((uint type, uint requestId, uint correlationId, int size) header, byte[] body)
        ReceiveControl(Stream stream)
    {
        while (true)
        {
            var message = Receive(stream);
            if (message.header.type != 3)
                return message;
            Require(message.body.Length == 0, "Heartbeat carried data.");
        }
    }

    private static Task Host(NamedPipeServerStream server, Action action) => Task.Run(() =>
    {
        try { action(); }
        catch
        {
            server.Dispose();
            throw;
        }
    });

    private static void RefusedGh1Bridge()
    {
        string name = "Tapioca.Gh2.Refusal." + Guid.NewGuid().ToString("N");
        using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);
        Task host = Task.Run(() =>
        {
            server.WaitForConnection();
            Receive(server);
            // An ordinary GH1 bridge grants no GH2 bit. Never continue to
            // project requests merely because the frame version matches.
            Send(server, 2, 0, 0, Word(0));
        });
        try
        {
            using var _ = Gh2PipeClient.Connect(name);
            throw new InvalidOperationException("GH2 accepted a GH1 bridge.");
        }
        catch (InvalidDataException error) when (error.Message.Contains("GH2", StringComparison.Ordinal)) { }
        host.GetAwaiter().GetResult();
    }

    private static void OversizedAnswer()
    {
        string name = "Tapioca.Gh2.Size." + Guid.NewGuid().ToString("N");
        using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);
        Task host = Task.Run(() =>
        {
            server.WaitForConnection();
            Receive(server);
            // Declare 64 KiB + 1 byte but transmit no payload: the client
            // must refuse from the header instead of allocating or blocking.
            byte[] header = new byte[20];
            Write(header, 0, 5);
            Write(header, 4, 2);
            Write(header, 16, Gh2Wire.MaxPingResponse + 1);
            server.Write(header);
        });
        try
        {
            using var _ = Gh2PipeClient.Connect(name);
            throw new InvalidOperationException("GH2 accepted an oversized handshake.");
        }
        catch (InvalidDataException error) when (error.Message.Contains("oversized", StringComparison.Ordinal)) { }
        host.GetAwaiter().GetResult();
    }

    private static void PeerAttachAndDetach()
    {
        var first = new Gh2Peer.Candidate("Tapioca.Gh2.v5.123.1", 1);
        var second = new Gh2Peer.Candidate("Tapioca.Gh2.v5.456.2", 2);
        Require(!Gh2Peer.TrySelect([], out _, out string none) && none.Contains("No Archicad", StringComparison.Ordinal),
            "Peer selected an absent bridge.");
        Require(!Gh2Peer.TrySelect([first, second], out _, out string many) &&
                many.Contains(first.Name, StringComparison.Ordinal) && many.Contains(second.Name, StringComparison.Ordinal),
            "Peer silently chose among multiple bridges.");
        Require(Gh2Peer.TrySelect([first], out var only, out _) && only == first,
            "Peer did not select the sole bridge.");

        string name = $"Tapioca.Gh2.v5.{Environment.ProcessId}.{Random.Shared.Next(100000, int.MaxValue)}";
        uint generation = uint.Parse(name.Split('.')[^1]);
        using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);
        Require(Gh2Peer.Discover().Any(candidate => candidate.Name == name && candidate.Generation == generation),
            "Peer discovery did not find the waiting v5 pipe.");
        Task host = Host(server, () =>
        {
            server.WaitForConnection();
            var (hello, body) = Receive(server);
            Require(hello.type == 1 && body.Length == 8 && Read(body, 4) == Gh2Wire.CapabilityGh2,
                "Peer did not offer GH2 hello.");
            Send(server, 2, 0, 0, Word(Gh2Wire.CapabilityGh2));
            uint id = ExpectPing(server);
            RequireNoStartupBeforeReply(server);
            Send(server, 5, 0, id,
                Encoding.UTF8.GetBytes("{\"ok\":true,\"data\":{\"archicadVersion\":29,\"archicadBuild\":3000,\"projectName\":\"Peer test\"}}"));
            var (ack, startup) = ReceiveControl(server);
            Require(ack.type == 9 && Read(startup, 0) == 0 &&
                    Encoding.UTF8.GetString(startup, 4, startup.Length - 4).Contains("Attached Rhino", StringComparison.Ordinal),
                "Peer did not acknowledge the validated Ping exactly once.");
            var (log, text) = ReceiveControl(server);
            Require(log.type == 10 && Encoding.UTF8.GetString(text).Contains("Peer test", StringComparison.Ordinal),
                "Peer did not report the validated project.");
            Send(server, 8, 0, 0, []);
            // A peer sends no second Ack when Archicad detaches.
            Require(server.ReadByte() == -1, "Peer did not release the pipe after Shutdown.");
        });
        var messages = new List<string>();
        Gh2Peer.AttachSelected(new Gh2Peer.Candidate(name, generation), message =>
        {
            messages.Add(message);
            if (!message.Contains("GH2 attached to", StringComparison.Ordinal))
                return;
            Gh2ConnectionStatus.Snapshot state = Gh2ConnectionStatus.Read();
            Require(state.Phase == "Connected" && state.Endpoint == name &&
                    state.Archicad.Contains("Peer test", StringComparison.Ordinal) &&
                    state.Health.StartsWith("Healthy: local pipe write", StringComparison.Ordinal) &&
                    state.LocalAddresses.Contains("127.0.0.1", StringComparison.Ordinal),
                "Status component did not receive a cached, healthy local-pipe snapshot.");
        });
        host.GetAwaiter().GetResult();
        Require(messages.Any(message => message.Contains("Rhino remains open", StringComparison.Ordinal)),
            "Peer did not report a non-destructive detach.");
        Require(Gh2ConnectionStatus.Read().Phase == "Disconnected" &&
                Gh2ConnectionStatus.Read().Archicad == "(not connected)",
            "Peer detach left stale project data in GH2 connection status.");
    }

    private static void PeerRefusedPing()
    {
        string name = $"Tapioca.Gh2.v5.{Environment.ProcessId}.{Random.Shared.Next(100000, int.MaxValue)}";
        using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);
        Task host = Host(server, () =>
        {
            server.WaitForConnection();
            Receive(server);
            Send(server, 2, 0, 0, Word(Gh2Wire.CapabilityGh2));
            uint id = ExpectPing(server);
            Send(server, 5, 0, id, Encoding.UTF8.GetBytes("{\"ok\":false,\"error\":\"Open a project.\"}"));
            var (ack, payload) = ReceiveControl(server);
            Require(ack.type == 9 && Read(payload, 0) == 1 &&
                    Encoding.UTF8.GetString(payload, 4, payload.Length - 4).Contains("Open a project.", StringComparison.Ordinal),
                "Peer did not send the native rejection as a failed startup Ack.");
            Require(server.ReadByte() == -1, "Peer sent more than one startup Ack on Ping refusal.");
        });
        try
        {
            Gh2Peer.AttachSelected(new Gh2Peer.Candidate(name, uint.Parse(name.Split('.')[^1])), _ => { });
            throw new InvalidOperationException("Peer accepted a rejected Ping.");
        }
        catch (InvalidDataException error) when (error.Message.Contains("Open a project.", StringComparison.Ordinal)) { }
        host.GetAwaiter().GetResult();
    }

    private static void PeerLostConnection()
    {
        string name = $"Tapioca.Gh2.v5.{Environment.ProcessId}.{Random.Shared.Next(100000, int.MaxValue)}";
        using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);
        Task host = Host(server, () =>
        {
            server.WaitForConnection();
            Receive(server);
            Send(server, 2, 0, 0, Word(Gh2Wire.CapabilityGh2));
            uint id = ExpectPing(server);
            Send(server, 5, 0, id,
                Encoding.UTF8.GetBytes("{\"ok\":true,\"data\":{\"archicadVersion\":29,\"archicadBuild\":3000,\"projectName\":\"Peer test\"}}"));
            Require(ReceiveControl(server).header.type == 9, "Peer never acknowledged startup.");
            Require(ReceiveControl(server).header.type == 10, "Peer never logged project info.");
            server.Dispose();
        });
        var messages = new List<string>();
        Gh2Peer.AttachSelected(new Gh2Peer.Candidate(name, uint.Parse(name.Split('.')[^1])), messages.Add);
        host.GetAwaiter().GetResult();
        Require(messages.Any(message => message.Contains("disconnected", StringComparison.Ordinal)),
            "Peer did not release a lost connection.");
    }

    private static (uint type, uint requestId, uint correlationId, int size) Header(ReadOnlySpan<byte> bytes) =>
        (Read(bytes, 4), Read(bytes, 8), Read(bytes, 12), checked((int)Read(bytes, 16)));

    private static ((uint type, uint requestId, uint correlationId, int size) header, byte[] body)
        Receive(Stream stream)
    {
        byte[] frame = new byte[20];
        stream.ReadExactly(frame);
        Require(Read(frame, 0) == 5, "Wrong bridge version.");
        var header = Header(frame);
        Require(header.size <= 4096, "Fake host received an unexpected large frame.");
        byte[] payload = new byte[header.size];
        if (payload.Length != 0)
            stream.ReadExactly(payload);
        return (header, payload);
    }

    private static void Send(Stream stream, uint type, uint requestId, uint correlationId, byte[] body)
    {
        byte[] header = new byte[20];
        Write(header, 0, 5);
        Write(header, 4, type);
        Write(header, 8, requestId);
        Write(header, 12, correlationId);
        Write(header, 16, (uint)body.Length);
        stream.Write(header);
        if (body.Length != 0)
            stream.Write(body);
    }

    private static byte[] Word(uint value)
    {
        byte[] word = new byte[4];
        Write(word, 0, value);
        return word;
    }

    private static uint Read(ReadOnlySpan<byte> bytes, int offset) =>
        BinaryPrimitives.ReadUInt32LittleEndian(bytes[offset..]);

    private static void Write(Span<byte> bytes, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[offset..], value);

    private static void Require(bool truth, string reason)
    {
        if (!truth)
            throw new InvalidOperationException(reason);
    }
}
