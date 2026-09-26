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
        PeerRefreshAndDetach();
        SelectionActions();
        PeerRefusedPing();
        PeerLostConnection();
        ProjectOptionValidation();
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
            ExpectPingDiagnostic(server);
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
            ExpectPingDiagnostic(server);
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

    private static void ExpectPingDiagnostic(Stream stream)
    {
        var (log, text) = ReceiveControl(stream);
        string diagnostic = Encoding.UTF8.GetString(text);
        Require(log.type == 10 && diagnostic.Contains("GH2 Ping diagnostic: peer process", StringComparison.Ordinal) &&
                diagnostic.Contains("request OS thread", StringComparison.Ordinal) &&
                diagnostic.Contains("round trip", StringComparison.Ordinal) &&
                !diagnostic.Contains("private/project/path", StringComparison.Ordinal),
            "The peer did not report bounded Ping thread/timing diagnostics.");
    }

    private static void ExpectRead(Stream stream, string command, string parameters, string answer,
        Action? beforeReply = null)
    {
        var (request, bytes) = ReceiveControl(stream);
        Require(request.type == 4 && request.requestId != 0 && bytes.Length >= 8,
            "No GH2 project-options request arrived.");
        int commandBytes = checked((int)Read(bytes, 0));
        int paramsBytes = checked((int)Read(bytes, 4));
        Require(8L + commandBytes + paramsBytes == bytes.Length &&
                Encoding.UTF8.GetString(bytes, 8, commandBytes) == command &&
                Encoding.UTF8.GetString(bytes, 8 + commandBytes, paramsBytes) == parameters,
            "GH2 requested an unapproved project-options command or parameters.");
        beforeReply?.Invoke();
        Send(stream, 5, 0, request.requestId, Encoding.UTF8.GetBytes(answer));
    }

    private static void ExpectProjectOptions(Stream stream, string lineType = "Solid")
    {
        (string command, string parameters, string answer)[] reads =
        [
            ("Tapioca.ListAttributes", "{\"kind\":\"layer\"}",
                "{\"ok\":true,\"data\":{\"kind\":\"layer\",\"attributes\":[{\"name\":\"Walls\"},{\"name\":\"Furniture\"}]}}"),
            ("Tapioca.ListAttributes", "{\"kind\":\"lineType\"}",
                "{\"ok\":true,\"data\":{\"kind\":\"lineType\",\"attributes\":[{\"name\":\"" + lineType + "\"}]}}"),
            ("Tapioca.GetStories", "{}",
                "{\"ok\":true,\"data\":{\"indices\":[-1,0],\"names\":[\"Basement\",\"Ground\"],\"levels\":[-3.2,0]}}")
        ];
        foreach ((string command, string parameters, string answer) in reads)
            ExpectRead(stream, command, parameters, answer);
    }

    private static void PeerRefreshAndDetach()
    {
        string name = $"Tapioca.Gh2.v5.{Environment.ProcessId}.{Random.Shared.Next(100000, int.MaxValue)}";
        string identity = "{\"ok\":true,\"data\":{\"archicadVersion\":29,\"archicadBuild\":3000," +
            "\"projectName\":\"Refresh test\",\"projectPath\":\"C:/sample.pln\",\"untitled\":false," +
            "\"modelStamp\":100,\"selectionStamp\":10}}";
        string selectionOnly = identity.Replace("\"modelStamp\":100,\"selectionStamp\":10",
            "\"modelStamp\":101,\"selectionStamp\":11", StringComparison.Ordinal);
        string edited = selectionOnly.Replace("\"modelStamp\":101", "\"modelStamp\":102", StringComparison.Ordinal);
        using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);
        Task host = Host(server, () =>
        {
            server.WaitForConnection();
            Receive(server); // Hello
            Send(server, 2, 0, 0, Word(Gh2Wire.CapabilityGh2));
            uint pingId = ExpectPing(server);
            Send(server, 5, 0, pingId, Encoding.UTF8.GetBytes(identity));
            Require(ReceiveControl(server).header.type == 9, "Peer did not acknowledge the refresh-test Ping.");
            Require(ReceiveControl(server).header.type == 10, "Peer did not log its refresh-test Ping.");
            ExpectPingDiagnostic(server);
            ExpectProjectOptions(server);
            Require(SpinWait.SpinUntil(() => !ArchicadProjectOptions.ReadStatus().Stale &&
                ArchicadProjectOptions.Read().Layers.Length == 2, TimeSpan.FromSeconds(3)),
                "Initial project choices were not published before refresh.");
            string[] originalLayers = ArchicadProjectOptions.Read().Layers;
            ExpectRead(server, "Tapioca.GetGhConnectionInfo", "{}", identity);
            ExpectProjectOptions(server, lineType: "Dashed");
            ExpectRead(server, "Tapioca.GetGhConnectionInfo", "{}", identity);
            Require(SpinWait.SpinUntil(() => !ArchicadProjectOptions.ReadStatus().Checking &&
                ArchicadProjectOptions.ReadStatus().Revision >= 2, TimeSpan.FromSeconds(3)),
                "The asynchronously refreshed snapshot did not publish.");
            var state = ArchicadProjectOptions.ReadStatus();
            Require(!state.Stale && state.LastChanged == ArchicadProjectOptions.Changed.LineTypes &&
                    ReferenceEquals(originalLayers, ArchicadProjectOptions.Read().Layers) &&
                    ArchicadProjectOptions.Read().LineTypes.SequenceEqual(["Dashed"]),
                "Refresh replaced unchanged lists, or did not update the changed line types.");
            string[] originalLineTypes = ArchicadProjectOptions.Read().LineTypes;
            ExpectRead(server, "Tapioca.GetGhConnectionInfo", "{}", selectionOnly);
            // A single lightweight poll detects a model edit; it marks the
            // snapshot stale without clearing selectors or fetching catalogs.
            ExpectRead(server, "Tapioca.GetGhConnectionInfo", "{}", edited, () =>
                Require(!ArchicadProjectOptions.ReadStatus().Stale &&
                        ReferenceEquals(originalLayers, ArchicadProjectOptions.Read().Layers),
                    "Changing only Archicad's live selection invalidated the project snapshot."));
            Require(SpinWait.SpinUntil(() => ArchicadProjectOptions.ReadStatus().Stale,
                TimeSpan.FromSeconds(3)), "The model-stamp hint did not mark the snapshot stale.");
            Require(ReferenceEquals(originalLayers, ArchicadProjectOptions.Read().Layers) &&
                    ReferenceEquals(originalLineTypes, ArchicadProjectOptions.Read().LineTypes),
                "A stale model hint discarded the last good selector values.");
            Require(SpinWait.SpinUntil(Gh2ConnectionStatus.RequestRefresh, TimeSpan.FromSeconds(3)),
                "The next explicit refresh remained blocked after the previous one completed.");
            Require(SpinWait.SpinUntil(() => ArchicadProjectOptions.ReadStatus().Checking &&
                ArchicadProjectOptions.ReadStatus().Stale, TimeSpan.FromSeconds(3)),
                "A refresh lost the pending stale warning before it completed.");
            ExpectRead(server, "Tapioca.GetGhConnectionInfo", "{}", edited);
            Require(ReferenceEquals(originalLayers, ArchicadProjectOptions.Read().Layers) &&
                    ReferenceEquals(originalLineTypes, ArchicadProjectOptions.Read().LineTypes),
                "The previous choices disappeared while the same project was being checked.");
            ExpectProjectOptions(server, lineType: "Dashed");
            ExpectRead(server, "Tapioca.GetGhConnectionInfo", "{}", edited);
            Require(SpinWait.SpinUntil(() => !ArchicadProjectOptions.ReadStatus().Checking &&
                ArchicadProjectOptions.ReadStatus().LastChanged == ArchicadProjectOptions.Changed.None,
                TimeSpan.FromSeconds(3)), "The unchanged refresh did not finish.");
            Require(ArchicadProjectOptions.ReadStatus().Revision == state.Revision &&
                    ReferenceEquals(originalLineTypes, ArchicadProjectOptions.Read().LineTypes),
                "An unchanged project list was republished as a new revision.");
            string nextProject = edited.Replace("C:/sample.pln", "C:/another.pln", StringComparison.Ordinal);
            Require(SpinWait.SpinUntil(Gh2ConnectionStatus.RequestRefresh, TimeSpan.FromSeconds(3)),
                "A project-change check could not start.");
            ExpectRead(server, "Tapioca.GetGhConnectionInfo", "{}", nextProject);
            Require(SpinWait.SpinUntil(() => ArchicadProjectOptions.ReadStatus().Stale &&
                ArchicadProjectOptions.Read().Layers.Length == 0, TimeSpan.FromSeconds(3)),
                "The previous project's choices remained selectable after a confirmed switch.");
            ExpectProjectOptions(server, lineType: "Dashed");
            ExpectRead(server, "Tapioca.GetGhConnectionInfo", "{}", nextProject);
            Require(SpinWait.SpinUntil(() => !ArchicadProjectOptions.ReadStatus().Checking &&
                ArchicadProjectOptions.ReadStatus().Revision > state.Revision, TimeSpan.FromSeconds(3)),
                "The changed project did not publish its replacement snapshot.");
            Require(ArchicadProjectOptions.ReadStatus().LastChanged == ArchicadProjectOptions.Changed.All &&
                    !ReferenceEquals(originalLayers, ArchicadProjectOptions.Read().Layers),
                "A different Archicad project reused the previous project's choices.");
            Require(SpinWait.SpinUntil(Gh2ConnectionStatus.RequestRefresh, TimeSpan.FromSeconds(3)),
                "A mid-refresh project switch could not be checked.");
            ExpectRead(server, "Tapioca.GetGhConnectionInfo", "{}", nextProject);
            ExpectProjectOptions(server, lineType: "Dashed");
            ExpectRead(server, "Tapioca.GetGhConnectionInfo", "{}", identity);
            Require(SpinWait.SpinUntil(() => !ArchicadProjectOptions.ReadStatus().Checking &&
                ArchicadProjectOptions.ReadStatus().Stale, TimeSpan.FromSeconds(3)),
                "The mid-refresh project change did not invalidate the snapshot.");
            Require(ArchicadProjectOptions.Read().Layers.Length == 0 &&
                    ArchicadProjectOptions.ReadStatus().Issue.Contains("changed while refreshing", StringComparison.Ordinal),
                "A mixed-project choice list was exposed.");
            Send(server, 8, 0, 0, []);
            Require(server.ReadByte() == -1, "Peer did not detach after project refresh.");
        });
        Gh2Peer.AttachSelected(new Gh2Peer.Candidate(name, uint.Parse(name.Split('.')[^1])), message =>
        {
            if (message.StartsWith("GH2 attached to", StringComparison.Ordinal))
                Require(Gh2ConnectionStatus.RequestRefresh(), "A connected peer refused the refresh request.");
        });
        host.GetAwaiter().GetResult();
        Require(ArchicadProjectOptions.ReadStatus().Stale && ArchicadProjectOptions.Read().Layers.Length == 0,
            "Detach did not invalidate the refreshed project snapshot.");
    }

    private static void ProjectOptionValidation()
    {
        Require(ArchicadProjectOptions.ParseSelectionStamp(
            "{\"ok\":true,\"data\":{\"selectionStamp\":-98}}") == -98,
            "A valid Archicad selection fingerprint was not parsed.");
        Require(ArchicadProjectOptions.ParseAttributes(
            "{\"ok\":true,\"data\":{\"kind\":\"layer\",\"attributes\":[{\"name\":\"Walls\"}]}}", "layer")
                .SequenceEqual(["Walls"]), "A project layer name was not retained.");
        Require(ArchicadProjectOptions.ParseStories(
            "{\"ok\":true,\"data\":{\"indices\":[-1],\"names\":[\"Basement\"],\"levels\":[-3.2]}}")
                .Single().Index == -1, "A story index was confused with its list position.");
        try
        {
            ArchicadProjectOptions.ParseStories(
                "{\"ok\":true,\"data\":{\"indices\":[0,1],\"names\":[\"Ground\"],\"levels\":[0,3]}}");
            throw new InvalidOperationException("Mismatched project story arrays were accepted.");
        }
        catch (InvalidDataException) { }
    }

    private static void SelectionActions()
    {
        string name = "Tapioca.Gh2.Selection." + Guid.NewGuid().ToString("N");
        Guid id = Guid.NewGuid();
        using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);
        Task host = Host(server, () =>
        {
            server.WaitForConnection();
            Receive(server); // hello
            Send(server, 2, 0, 0, Word(Gh2Wire.CapabilityGh2));
            ExpectRead(server, "Tapioca.GetSelection", "{}", "{\"ok\":true,\"data\":{\"elements\":[{" +
                "\"elementId\":{\"guid\":\"" + id + "\"}}]}}");
            string payload = "{\"elements\":[{\"elementId\":{\"guid\":\"" + id + "\"}}],\"add\":false}";
            ExpectRead(server, "Tapioca.SetSelection", payload,
                "{\"ok\":true,\"data\":{\"selected\":1,\"missing\":[],\"count\":1}}");
            Send(server, 8, 0, 0, []);
        });
        using var client = Gh2PipeClient.Connect(name);
        Task reader = Task.Run(() => client.WaitForShutdown(false));
        Require(client.ReadSelectionAsync().GetAwaiter().GetResult().Contains(id.ToString(), StringComparison.OrdinalIgnoreCase),
            "GH2 did not read the current Archicad selection.");
        Require(client.ReplaceSelectionAsync([id]).GetAwaiter().GetResult().Contains("\"selected\":1", StringComparison.Ordinal),
            "GH2 did not send the explicit reselect action.");
        reader.GetAwaiter().GetResult();
        host.GetAwaiter().GetResult();
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
                Encoding.UTF8.GetBytes("{\"ok\":true,\"data\":{\"archicadVersion\":29,\"archicadBuild\":3000,\"projectName\":\"Peer test\",\"projectPath\":\"C:/sample.pln\",\"untitled\":false}}"));
            var (ack, startup) = ReceiveControl(server);
            Require(ack.type == 9 && Read(startup, 0) == 0 &&
                    Encoding.UTF8.GetString(startup, 4, startup.Length - 4).Contains("Attached Rhino", StringComparison.Ordinal),
                "Peer did not acknowledge the validated Ping exactly once.");
            var (log, text) = ReceiveControl(server);
            Require(log.type == 10 && Encoding.UTF8.GetString(text).Contains("Peer test", StringComparison.Ordinal),
                "Peer did not report the validated project.");
            ExpectPingDiagnostic(server);
            ExpectProjectOptions(server);
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
                    state.LocalAddresses.Contains("127.0.0.1", StringComparison.Ordinal) &&
                    ArchicadProjectOptions.Read().Layers.SequenceEqual(["Walls", "Furniture"]) &&
                    ArchicadProjectOptions.Read().LineTypes.SequenceEqual(["Solid"]) &&
                    ArchicadProjectOptions.Read().Stories[0].Index == -1,
                "Status component did not receive a cached, healthy local-pipe snapshot.");
        });
        host.GetAwaiter().GetResult();
        Require(messages.Any(message => message.Contains("Rhino remains open", StringComparison.Ordinal)),
            "Peer did not report a non-destructive detach.");
        Require(Gh2ConnectionStatus.Read().Phase == "Disconnected" &&
                Gh2ConnectionStatus.Read().Archicad == "(not connected)" &&
                ArchicadProjectOptions.Read().Layers.Length == 0,
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
                Encoding.UTF8.GetBytes("{\"ok\":true,\"data\":{\"archicadVersion\":29,\"archicadBuild\":3000,\"projectName\":\"Peer test\",\"projectPath\":\"C:/sample.pln\",\"untitled\":false}}"));
            Require(ReceiveControl(server).header.type == 9, "Peer never acknowledged startup.");
            Require(ReceiveControl(server).header.type == 10, "Peer never logged project info.");
            ExpectPingDiagnostic(server);
            ExpectProjectOptions(server);
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
