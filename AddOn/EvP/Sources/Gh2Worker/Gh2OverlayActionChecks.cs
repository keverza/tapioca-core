using System.Buffers.Binary;
using System.IO.Pipes;
using System.Reflection;
using System.Text;
using System.Text.Json;
using Grasshopper2.Components;
using Grasshopper2.Doc;
using Grasshopper2.Parameters.Standard;
using Rhino.Geometry;

namespace Tapioca.Gh2Worker;

// Runs the actual harvested components and their plugin-owned connection against
// an independent fake host. A solve must stage data; only Show may send it.
internal static class Gh2OverlayActionChecks
{
    private const BindingFlags HiddenStatic = BindingFlags.NonPublic | BindingFlags.Static;
    private const BindingFlags HiddenInstance = BindingFlags.NonPublic | BindingFlags.Instance;
    private const string Info = "{\"ok\":true,\"data\":{\"projectName\":\"Overlay test\",\"projectPath\":\"C:/overlay.pln\",\"untitled\":false}}";

    internal static void Run(Document document, Component[] components)
    {
        Assembly plugin = components[0].GetType().Assembly;
        Type pipeType = plugin.GetType("Tapioca.Gh2Worker.Gh2PipeClient")!;
        Type status = plugin.GetType("TapiocaGH2.Gh2ConnectionStatus")!;
        Type options = plugin.GetType("TapiocaGH2.ArchicadProjectOptions")!;
        string project = (string)options.GetMethod("ParseIdentity", HiddenStatic)!.Invoke(null, [Info])!;
        string name = $"Tapioca.Gh2.v5.{Environment.ProcessId}.42";
        using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
        int solves = 0;
        using var removedLayerCleared = new ManualResetEventSlim();
        Task host = Task.Run(() =>
        {
            try
            {
                server.WaitForConnection();
                var hello = Receive(server);
                if (hello.Type != 1) throw new InvalidOperationException("Overlay did not connect.");
                byte[] granted = new byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(granted, 2);
                Send(server, 2, 0, granted);
                for (int i = 0; i < components.Length; i++)
                {
                    Expect(server, "Tapioca.GetGhConnectionInfo", Info, json =>
                    {
                        if (Volatile.Read(ref solves) != i + 1 || json != "{}")
                            throw new InvalidOperationException("Overlay made a live query during Process.");
                    });
                    Expect(server, i == 0 ? "Tapioca.Overlay2D" : "Tapioca.Overlay3D",
                        "{\"ok\":true,\"data\":{\"running\":true,\"refused\":false,\"message\":\"on\"}}",
                        json => { if (json != "{\"action\":\"on\"}") throw new InvalidOperationException("Overlay changed global intent unexpectedly."); });
                    Expect(server, "Tapioca.GetGhConnectionInfo", Info);
                    string? layer = null;
                    Expect(server, "Tapioca.SetOverlayLayer", "{\"ok\":true,\"data\":{\"generation\":\"1\"}}", json =>
                    {
                        using JsonDocument payload = JsonDocument.Parse(json);
                        JsonElement data = payload.RootElement;
                        layer = data.GetProperty("layer").GetString();
                        if (layer is null || !layer.StartsWith("gh2.42.", StringComparison.Ordinal) ||
                            data.GetProperty("views").GetString() != (i == 0 ? "2d" : "3d") ||
                            data.GetProperty("polylines").GetArrayLength() != 1)
                            throw new InvalidOperationException("Component published wrong surface/identity/geometry.");
                    });
                    Expect(server, "Tapioca.GetGhConnectionInfo", Info);
                    Expect(server, "Tapioca.ClearOverlayLayer", "{\"ok\":true,\"data\":{\"cleared\":1}}", json =>
                    {
                        if (json != "{\"layer\":\"" + layer + "\"}")
                            throw new InvalidOperationException("Component Clear did not address its own layer.");
                    });
                    if (i == components.Length - 1)
                    {
                        Expect(server, "Tapioca.ClearOverlayLayer", "{\"ok\":true,\"data\":{\"cleared\":0}}", json =>
                        {
                            if (json != "{\"layer\":\"" + layer + "\"}")
                                throw new InvalidOperationException("Removal cleanup targeted a new component lifetime.");
                        });
                        removedLayerCleared.Set();
                    }
                }
                Send(server, 8, 0, []);
            }
            catch { server.Dispose(); throw; }
        });
        using var client = (IDisposable)pipeType.GetMethod("Connect", HiddenStatic)!.Invoke(null, [name])!;
        Task reader = Task.Run(() => pipeType.GetMethod("WaitForShutdown", HiddenInstance)!.Invoke(client, [false]));
        try
        {
            Type snapshotType = options.GetNestedType("Snapshot", BindingFlags.NonPublic)!;
            Type storyType = options.GetNestedType("Story", BindingFlags.NonPublic)!;
            object snapshot = Activator.CreateInstance(snapshotType, [Array.Empty<string>(), Array.Empty<string>(), Array.CreateInstance(storyType, 0)])!;
            options.GetMethod("Set", HiddenStatic)!.Invoke(null, [snapshot, project, null, null]);
            status.GetMethod("Set", HiddenStatic)!.Invoke(null, ["Connected", name, "Overlay test", client, null]);
            for (int i = 0; i < components.Length; i++)
            {
                Component component = components[i];
                var line = new LineCurve(new Point3d(0, 0, 1), new Point3d(2, 3, 4));
                ((CurveParameter)component.Parameters.Input(0)).Set(line);
                component.Parameters.Input(0).Expire();
                component.Expire();
                if (document.Solution.StartWait(null, SolutionMode.Headless).Phase != SolutionPhase.Completed ||
                    component.GetType().BaseType!.GetField("prepared", HiddenInstance)!.GetValue(component) is null)
                    throw new InvalidOperationException("Connected overlay did not prepare a complete solution: " +
                        string.Join(" | ", component.State.Data.Messages.Errors.Concat(component.State.Data.Messages.Warnings)));
                Interlocked.Increment(ref solves);
                Type componentBase = component.GetType().BaseType!;
                componentBase.GetMethod("Show", HiddenInstance)!.Invoke(component, null);
                if (componentBase.GetField("shownLease", HiddenInstance)!.GetValue(component) is null)
                {
                    object batch = componentBase.GetField("prepared", HiddenInstance)!.GetValue(component)!;
                    object geometry = batch.GetType().GetProperty("Geometry")!.GetValue(batch)!;
                    object lease = batch.GetType().GetProperty("Lease")!.GetValue(batch)!;
                    throw new InvalidOperationException("Show declined its prepared solution: phase=" + component.State.Phase +
                        "; current=" + component.State.CurrentId + "; batch=" + batch.GetType().GetProperty("Solution")!.GetValue(batch) +
                        "; curves=" + ((Array)geometry.GetType().GetProperty("Lines")!.GetValue(geometry)!).Length +
                        "; input=" + component.Parameters.Input(0).State.Data.Tree()?.ItemCount +
                        "; leaseCurrent=" + lease.GetType().GetProperty("Current", HiddenInstance)!.GetValue(lease));
                }
                WaitIdle(component, host);
                componentBase.GetMethod("Clear", HiddenInstance)!.Invoke(component, null);
                WaitIdle(component, host);
                if (i == components.Length - 1)
                {
                    FieldInfo producer = componentBase.GetField("producer", HiddenInstance)!;
                    Guid oldProducer = (Guid)producer.GetValue(component)!;
                    document.Objects.Remove(component);
                    if (!document.Objects.Add(component) || (Guid)producer.GetValue(component)! == oldProducer ||
                        componentBase.GetField("prepared", HiddenInstance)!.GetValue(component) is not null)
                        throw new InvalidOperationException("Undo/re-add retained an old overlay action/layer identity.");
                    if (!removedLayerCleared.Wait(TimeSpan.FromSeconds(30)))
                        throw new TimeoutException("Component removal did not clear its old layer.");
                }
            }
            host.WaitAsync(TimeSpan.FromSeconds(30)).GetAwaiter().GetResult();
            reader.WaitAsync(TimeSpan.FromSeconds(30)).GetAwaiter().GetResult();
        }
        finally
        {
            options.GetMethod("Clear", HiddenStatic)!.Invoke(null, null);
            status.GetMethod("Set", HiddenStatic)!.Invoke(null, ["Disconnected", "(none)", "(not connected)", null, null]);
        }
        Console.WriteLine("GH2 connected solves are inert; Show/Clear on both surfaces and removal/re-add cleanup passed.");
    }

    private static void WaitIdle(Component component, Task host)
    {
        FieldInfo busy = component.GetType().BaseType!.GetField("busy", HiddenInstance)!;
        if (!SpinWait.SpinUntil(() => (int)busy.GetValue(component)! == 0 || host.IsFaulted, TimeSpan.FromSeconds(30)))
            throw new TimeoutException("Overlay action did not complete.");
        if (host.IsFaulted) host.GetAwaiter().GetResult();
    }

    private static void Expect(Stream stream, string command, string response, Action<string>? validate = null)
    {
        var request = Receive(stream);
        while (request.Type == 3) request = Receive(stream);
        if (request.Type != 4) throw new InvalidOperationException("Unexpected overlay message.");
        int count = (int)BinaryPrimitives.ReadUInt32LittleEndian(request.Body);
        if (Encoding.UTF8.GetString(request.Body, 8, count) != command)
            throw new InvalidOperationException("Overlay request order changed: expected " + command);
        Console.WriteLine("Overlay action check received " + command);
        validate?.Invoke(Encoding.UTF8.GetString(request.Body, 8 + count, request.Body.Length - 8 - count));
        Send(stream, 5, request.Id, Encoding.UTF8.GetBytes(response));
    }

    private static (uint Type, uint Id, byte[] Body) Receive(Stream stream)
    {
        byte[] header = new byte[20]; stream.ReadExactly(header);
        int count = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(16)));
        if (count > 4096) throw new InvalidOperationException("Unexpected oversized test frame.");
        byte[] body = new byte[count]; stream.ReadExactly(body);
        return (BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(4)), BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(8)), body);
    }

    private static void Send(Stream stream, uint type, uint correlation, byte[] body)
    {
        byte[] header = new byte[20];
        BinaryPrimitives.WriteUInt32LittleEndian(header, 5);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(4), type);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(12), correlation);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(16), (uint)body.Length);
        stream.Write(header); stream.Write(body); stream.Flush();
    }
}
