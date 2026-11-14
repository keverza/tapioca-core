using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.IO.Pipes;
using System.Reflection;
using System.Text;
using System.Text.Json;
using Grasshopper2.Components;
using Grasshopper2.Doc;
using Grasshopper2.Framework;
using Grasshopper2.Parameters;

namespace Tapioca.Gh2Worker;

// The Archicad-to-definition half of the roundtrip against the real GH2 solver:
// two query components fed by one selection, a scripted host that owns element
// values and a change journal, and the fixture's own UI queue. What is counted
// is solutions and which rows were re-read, never a sleep.
internal static class Gh2InputRevisionChecks
{
    private const BindingFlags HiddenStatic = BindingFlags.NonPublic | BindingFlags.Static;
    private const BindingFlags HiddenInstance = BindingFlags.NonPublic | BindingFlags.Instance;
    private const string Info = "{\"ok\":true,\"data\":{\"projectName\":\"Input test\",\"projectPath\":\"C:/input.pln\",\"untitled\":false,\"projectEpoch\":3}}";

    private sealed class Host(NamedPipeServerStream pipe, Guid[] elements)
    {
        private readonly object sync = new();
        private readonly Dictionary<Guid, string> values = elements.ToDictionary(id => id, _ => "v1");
        private readonly List<(long Revision, Guid Id)> journal = [];
        private readonly List<(string Kind, Guid[] Ids)> reads = [];
        private long revision = 1, lastRequest = Environment.TickCount64;
        internal long QuietMs => Environment.TickCount64 - Interlocked.Read(ref lastRequest);

        internal void Change(Guid id, string? value = null)
        {
            lock (sync)
            {
                if (value is not null) values[id] = value;
                journal.Add((++revision, id));
            }
        }

        internal (string Kind, Guid[] Ids)[] TakeReads()
        {
            lock (sync) { var taken = reads.ToArray(); reads.Clear(); return taken; }
        }

        internal void Wake() => Send(41, 0, []);
        internal void Shutdown() => Send(8, 0, []);

        internal void Serve()
        {
            pipe.WaitForConnection();
            if (Receive().Type != 1) throw new InvalidOperationException("The input fixture peer did not say hello.");
            byte[] granted = new byte[4];
            BinaryPrimitives.WriteUInt32LittleEndian(granted, 6); // overlay + element changes
            Send(2, 0, granted);
            while (true)
            {
                (uint Type, uint Id, byte[] Body) request;
                try { request = Receive(); }
                catch (Exception error) when (error is IOException or EndOfStreamException or ObjectDisposedException) { return; }
                if (request.Type != 4) continue;
                Interlocked.Exchange(ref lastRequest, Environment.TickCount64);
                int count = (int)BinaryPrimitives.ReadUInt32LittleEndian(request.Body);
                string command = Encoding.UTF8.GetString(request.Body, 8, count);
                using JsonDocument json = JsonDocument.Parse(request.Body.AsMemory(8 + count));
                Send(5, request.Id, Encoding.UTF8.GetBytes(Answer(command, json.RootElement)));
            }
        }

        private string Answer(string command, JsonElement request)
        {
            switch (command)
            {
                case "Tapioca.GetGhConnectionInfo": return Info;
                case "Tapioca.GetGhElementChanges":
                {
                    Guid[] scope = request.GetProperty("guids").EnumerateArray().Select(id => id.GetGuid()).ToArray();
                    long since = request.GetProperty("sinceRevision").GetInt64();
                    lock (sync)
                        return JsonSerializer.Serialize(new
                        {
                            ok = true,
                            data = new
                            {
                                projectEpoch = 3, revision, sourceActive = true, resyncRequired = false,
                                coverage = "modelwatch-hints",
                                changes = journal.Where(entry => entry.Revision > since && scope.Contains(entry.Id))
                                    .Select(entry => new { guid = entry.Id, revision = entry.Revision, changeMask = 32 })
                            }
                        });
                }
                case "Tapioca.GetGhElementQuery":
                {
                    Guid[] ids = request.GetProperty("guids").EnumerateArray().Select(id => id.GetGuid()).ToArray();
                    string kind = request.GetProperty("kind").GetString()!;
                    lock (sync)
                    {
                        reads.Add((kind, ids));
                        return JsonSerializer.Serialize(new
                        {
                            ok = true,
                            data = new
                            {
                                elements = ids.Select(id => new
                                {
                                    guid = id, status = "Success", diagnostic = "", more = false,
                                    items = kind == "properties"
                                        ? [new
                                        {
                                            key = new Guid("5a0c2ad8-9f0f-4a39-8a42-0f6b4c3d2e11"), group = "Fixture", name = "Mark",
                                            dataType = 0, definitionType = 0, userDefined = true, collectionType = 0,
                                            valueStatus = "HasValue", value = values[id], hasNumber = false, number = 0.0,
                                            hasBoolean = false, boolean = false
                                        }]
                                        : new object[] { new { minX = 0.0, minY = 0.0, minZ = 0.0, maxX = 1.0, maxY = 2.0, maxZ = 3.0 } }
                                })
                            }
                        });
                    }
                }
                default: return "{\"ok\":false,\"error\":\"Unexpected fixture command " + command + "\"}";
            }
        }

        private (uint Type, uint Id, byte[] Body) Receive()
        {
            byte[] header = new byte[20];
            pipe.ReadExactly(header);
            byte[] body = new byte[checked((int)BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(16)))];
            pipe.ReadExactly(body);
            return (BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(4)),
                BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(8)), body);
        }

        private void Send(uint type, uint correlation, byte[] body)
        {
            byte[] frame = new byte[20 + body.Length];
            BinaryPrimitives.WriteUInt32LittleEndian(frame, 5);
            BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(4), type);
            BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(12), correlation);
            BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(16), (uint)body.Length);
            body.CopyTo(frame, 20);
            lock (pipe) { pipe.Write(frame); pipe.Flush(); } // replies and wakes come from two threads
        }
    }

    internal static void Run(Assembly plugin)
    {
        Type pipeType = plugin.GetType("Tapioca.Gh2Worker.Gh2PipeClient")!;
        Type status = plugin.GetType("TapiocaGH2.Gh2ConnectionStatus")!;
        Type options = plugin.GetType("TapiocaGH2.ArchicadProjectOptions")!;
        Type delivery = plugin.GetType("TapiocaGH2.ArchicadChangeDelivery")!;
        Type solveHost = plugin.GetType("TapiocaGH2.ArchicadSolveHost")!;
        object shared = plugin.GetType("TapiocaGH2.ArchicadInputs")!.GetField("Shared", HiddenStatic)!.GetValue(null)!;
        FieldInfo postOverride = solveHost.GetField("PostOverride", HiddenStatic)!;
        string project = (string)options.GetMethod("ParseIdentity", HiddenStatic)!.Invoke(null, [Info])!;
        Guid first = Guid.NewGuid(), second = Guid.NewGuid();
        string name = "Tapioca.InputRevisions." + Guid.NewGuid().ToString("N");

        using var ui = new BlockingCollection<Action>();
        using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
        var host = new Host(server, [first, second]);
        Task serving = Task.Run(host.Serve);
        void Pump(Func<bool> done, string what)
        {
            long deadline = Environment.TickCount64 + 30_000;
            while (!done())
            {
                if (serving.IsFaulted) serving.GetAwaiter().GetResult();
                if (Environment.TickCount64 > deadline) throw new TimeoutException("Input revisions: " + what);
                if (ui.TryTake(out Action? action, 20)) action();
            }
        }
        // Nothing asked of the host, nothing queued, and every hold-back timer expired.
        void Settle() => Pump(() => host.QuietMs > 700 && ui.Count == 0, "the fixture never became quiet");

        Document document = Document.NewInertDocument();
        using var client = (IDisposable)pipeType.GetMethod("Connect", HiddenStatic)!.Invoke(null, [name])!;
        pipeType.GetMethod("ReadProjectInfo", HiddenInstance)!.Invoke(client, null);
        Task reader = Task.Run(() => pipeType.GetMethod("WaitForShutdown", HiddenInstance)!.Invoke(client, [false]));
        try
        {
            postOverride.SetValue(null, (Action<Action>)ui.Add);
            Type snapshotType = options.GetNestedType("Snapshot", BindingFlags.NonPublic)!;
            Type storyType = options.GetNestedType("Story", BindingFlags.NonPublic)!;
            options.GetMethod("Set", HiddenStatic)!.Invoke(null, [Activator.CreateInstance(snapshotType,
                [Array.Empty<string>(), Array.Empty<string>(), Array.CreateInstance(storyType, 0)])!, project, null, null]);
            status.GetMethod("Set", HiddenStatic)!.Invoke(null, ["Connected", name, "Input test", client, null]);
            delivery.GetMethod("Attach", HiddenStatic)!.Invoke(null, [client, shared]);

            var selection = (Component)(ObjectProxies.TryEmit<IDocumentObject>(new Guid("b31976a0-3e53-47f6-8c4e-696696c48972"))
                ?? throw new InvalidOperationException("GH2 did not register the Archicad selection."));
            var properties = (Component)ObjectProxies.TryEmit<IDocumentObject>(new Guid("72d1643e-0e64-4e37-95c4-5ffbc73cd7a7"))!;
            var geometry = (Component)ObjectProxies.TryEmit<IDocumentObject>(new Guid("da78e1ea-f789-4246-ae25-1a42166ef404"))!;
            if (!document.Objects.Add(selection) || !document.Objects.Add(properties) || !document.Objects.Add(geometry) ||
                !Connections.Connect(selection.Parameters.Output(1), properties.Parameters.Input(0)) ||
                !Connections.Connect(selection.Parameters.Output(1), geometry.Parameters.Input(0)))
                throw new InvalidOperationException("The input fixture definition could not be wired.");
            selection.GetType().GetField("elements", HiddenInstance)!.SetValue(selection, new[] { first, second });
            selection.GetType().GetField("projectKey", HiddenInstance)!.SetValue(selection, project);
            document.State = DocumentState.Active;

            int started = 0, stopped = 0;
            var emptied = new ConcurrentQueue<string>();
            bool watching = false;
            string[] Texts(Component component, int output) => component.Parameters.Output(output).State.Data.Tree()?.AllItems
                .Select(item => item?.ToString() ?? "").ToArray() ?? [];
            document.Solution.SolutionStarted += (_, _) => Interlocked.Increment(ref started);
            document.Solution.SolutionStopped += (_, _) =>
            {
                // Once data has been shown, a refresh must never blank it on the way.
                if (Volatile.Read(ref watching))
                    foreach (string state in Texts(properties, 1).Concat(Texts(geometry, 1)))
                        if (state != "Success") emptied.Enqueue(state);
                Interlocked.Increment(ref stopped);
            };

            // 1. First reads of both components are adopted together: one solution.
            if (document.Solution.StartWait(null, SolutionMode.Headless).Phase != SolutionPhase.Completed ||
                !Texts(properties, 1).SequenceEqual(["Pending", "Pending"]))
                throw new InvalidOperationException("A first solve did not report its reads as pending: " +
                    string.Join(",", Texts(properties, 1)));
            int baseline = Volatile.Read(ref started);
            Pump(() => Volatile.Read(ref stopped) > baseline && Texts(properties, 4).Length == 2 &&
                Texts(geometry, 1).SequenceEqual(["Success", "Success"]), "first reads were never adopted");
            Settle();
            if (Volatile.Read(ref started) != baseline + 1)
                throw new InvalidOperationException($"Two components' first reads cost {Volatile.Read(ref started) - baseline} solutions, not one.");
            if (!Texts(properties, 4)[0].Contains("v1") || !Texts(properties, 4)[1].Contains("v1"))
                throw new InvalidOperationException("The adopted property rows are not the host's values.");
            Volatile.Write(ref watching, true);
            host.TakeReads();

            // 2. A change the queries do not depend on: its rows are re-read, nothing solves.
            int before = Volatile.Read(ref started);
            host.Change(first);
            host.Wake();
            Pump(() => host.QuietMs < 200, "an unrelated change was never read");
            Settle();
            var reads = host.TakeReads();
            if (reads.Length != 2 || reads.Any(read => !read.Ids.SequenceEqual([first])) ||
                reads.Select(read => read.Kind).Order().SequenceEqual(["geometry", "properties"]) == false)
                throw new InvalidOperationException("A one-element hint did not re-read exactly that element once per query: " +
                    string.Join(" | ", reads.Select(read => read.Kind + ":" + read.Ids.Length)));
            if (Volatile.Read(ref started) != before)
                throw new InvalidOperationException("Identical rows expired a component and solved the definition.");

            // 3. A relevant change: one row re-read, one solution, the other row untouched.
            host.Change(first, "v2");
            host.Wake();
            Pump(() => Volatile.Read(ref stopped) == before + 1 && Texts(properties, 4).FirstOrDefault()?.Contains("v2") == true,
                "a changed property never reached the definition");
            Settle();
            reads = host.TakeReads();
            if (Volatile.Read(ref started) != before + 1 || reads.Any(read => !read.Ids.SequenceEqual([first])) ||
                !Texts(properties, 4)[1].Contains("v1"))
                throw new InvalidOperationException("A one-row change did not cost exactly one row read per query and one solution.");

            // 4. A burst: three edits, one read of the newest value, one solution.
            before = Volatile.Read(ref started);
            host.Change(first, "v3");
            host.Change(second, "w4");
            host.Change(first, "v5");
            host.Wake(); host.Wake(); host.Wake();
            Pump(() => Volatile.Read(ref stopped) > before && Texts(properties, 4).FirstOrDefault()?.Contains("v5") == true &&
                Texts(properties, 4)[1].Contains("w4"), "a burst never reached the definition");
            Settle();
            if (Volatile.Read(ref started) != before + 1)
                throw new InvalidOperationException($"A burst of three edits cost {Volatile.Read(ref started) - before} solutions.");
            if (!emptied.IsEmpty)
                throw new InvalidOperationException("A refresh blanked shown data on the way: " + string.Join(",", emptied.Distinct()));

            // 5. The real solver's idle gate: busy while running, one callback when it stops.
            object gate = Activator.CreateInstance(solveHost, true)!;
            MethodInfo isSolving = solveHost.GetMethod("IsSolving")!, whenIdle = solveHost.GetMethod("WhenIdle")!;
            int idle = 0, armed = 0;
            bool busyWhileRunning = false;
            document.Solution.SolutionStarted += (_, _) =>
            {
                if (Interlocked.Exchange(ref armed, 1) != 0) return;
                busyWhileRunning = (bool)isSolving.Invoke(gate, [document])!;
                whenIdle.Invoke(gate, [document, (Action)(() => Interlocked.Increment(ref idle))]);
            };
            selection.Expire();
            document.Solution.Start().GetAwaiter().GetResult();
            Pump(() => Volatile.Read(ref idle) == 1, "the idle callback never ran after the solution stopped");
            if (!busyWhileRunning || (bool)isSolving.Invoke(gate, [document])!)
                throw new InvalidOperationException("The solver gate misreports a running or a stopped solution.");
            whenIdle.Invoke(gate, [document, (Action)(() => Interlocked.Increment(ref idle))]);
            Pump(() => Volatile.Read(ref idle) == 2, "an already idle definition never called back");
            Settle();
            if (Volatile.Read(ref idle) != 2) throw new InvalidOperationException("The idle callback is not one-shot.");

            host.Shutdown();
            serving.WaitAsync(TimeSpan.FromSeconds(30)).ContinueWith(_ => { }).GetAwaiter().GetResult();
            reader.WaitAsync(TimeSpan.FromSeconds(30)).GetAwaiter().GetResult();
        }
        finally
        {
            delivery.GetMethod("Detach", HiddenStatic)!.Invoke(null, [client]);
            options.GetMethod("Clear", HiddenStatic)!.Invoke(null, null);
            status.GetMethod("Set", HiddenStatic)!.Invoke(null, ["Disconnected", "(none)", "(not connected)", null, null]);
            postOverride.SetValue(null, null);
            document.Close();
        }
        Console.WriteLine("GH2 input revisions: joint first adoption, unrelated-change silence, row-level refresh, burst coalescing and solver gate passed.");
    }
}
