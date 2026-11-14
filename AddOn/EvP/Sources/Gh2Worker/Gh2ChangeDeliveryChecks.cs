using System.Buffers.Binary;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using Grasshopper2.Doc;
using TapiocaGH2;

namespace Tapioca.Gh2Worker;

internal static class Gh2ChangeDeliveryChecks
{
    private sealed class Consumer : IArchicadInputConsumer
    {
        internal int Count, Commits;
        internal volatile bool Unadopted;
        internal readonly List<Guid[]?> Changed = [];
        internal readonly AutoResetEvent Invalidated = new(false);
        internal bool Active = true;
        private readonly object group = new();
        public bool IsActive => Active;
        public object? InputGroup => group;
        public Task<bool> StageInputsAsync(IReadOnlyCollection<Guid>? changed, bool lastAttempt, CancellationToken cancel)
        {
            lock (Changed) Changed.Add(changed?.ToArray());
            Unadopted = true;
            Interlocked.Increment(ref Count);
            Invalidated.Set();
            return Task.FromResult(true);
        }
        public bool CommitInputs() { Unadopted = false; Interlocked.Increment(ref Commits); return true; }
    }

    // A definition that is never solving, and a UI queue that runs inline.
    private sealed class Host(bool failFirstPost) : IArchicadSolveHost
    {
        private int posts;
        internal int Starts;
        public long NowMs => Environment.TickCount64;
        public Task Delay(int milliseconds, CancellationToken cancel) => Task.Delay(milliseconds, cancel);
        public void Post(Action action)
        {
            if (Interlocked.Increment(ref posts) == 1 && failFirstPost)
                throw new InvalidOperationException("Injected UI scheduling failure.");
            action();
        }
        public bool IsSolving(object group) => false;
        public void WhenIdle(object group, Action callback) => callback();
        public void Start(object group) => Interlocked.Increment(ref Starts);
    }

    internal static void Run(bool failFirstDispatch = false)
    {
        string name = "Tapioca.ChangeDelivery." + Guid.NewGuid().ToString("N");
        Guid id = Guid.NewGuid();
        var consumer = new Consumer();
        using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        async Task<(Gh2Wire.Header Header, byte[] Body)> Read()
        {
            while (true)
            {
                byte[] header = new byte[20]; await server.ReadExactlyAsync(header, deadline.Token);
                var frame = Gh2Wire.DecodeHeader(header);
                byte[] body = new byte[frame.PayloadBytes]; await server.ReadExactlyAsync(body, deadline.Token);
                if (frame.Type != Gh2Wire.Message.Heartbeat) return (frame, body);
            }
        }
        async Task Send(Gh2Wire.Message type, uint correlation, byte[] body)
        {
            await server.WriteAsync(Gh2Wire.EncodeHeader(type, 0, correlation, body.Length), deadline.Token);
            await server.WriteAsync(body, deadline.Token);
        }
        Task host = Task.Run(async () =>
        {
            await server.WaitForConnectionAsync(deadline.Token);
            await Read();
            await Send(Gh2Wire.Message.HelloAck, 0, [6, 0, 0, 0]);
            var info = await Read();
            await Send(Gh2Wire.Message.ApiResponse, info.Header.RequestId, Encoding.UTF8.GetBytes(
                "{\"ok\":true,\"data\":{\"projectEpoch\":7}}"));
            for (int revision = 1; revision <= 6; revision++)
            {
                if (revision > 1) await Send(Gh2Wire.Message.ElementChangesAvailable, 0, []);
                var request = await Read();
                int length = (int)BinaryPrimitives.ReadUInt32LittleEndian(request.Body);
                if (Encoding.UTF8.GetString(request.Body, 8, length) != "Tapioca.GetGhElementChanges")
                    throw new InvalidOperationException("Change delivery performed a full refresh.");
                using var json = JsonDocument.Parse(request.Body.AsMemory(8 + length));
                var cursor = json.RootElement;
                long expected = revision == 6 ? 4 : revision - 1;
                if (cursor.GetProperty("guids")[0].GetGuid() != id || cursor.GetProperty("fields").GetInt32() != 8 ||
                    cursor.GetProperty("sinceRevision").GetInt64() != expected)
                    throw new InvalidOperationException("Scoped cursor or inactive-source rebasing failed.");
                object[] changes = revision == 3 ? [new { guid = id, revision, changeMask = 32 }] : [];
                await Send(Gh2Wire.Message.ApiResponse, request.Header.RequestId, JsonSerializer.SerializeToUtf8Bytes(new
                {
                    ok = true, data = new { projectEpoch = 7, revision, sourceActive = revision != 5,
                        resyncRequired = revision is 1 or 5 or 6, coverage = "modelwatch-hints", changes }
                }));
                bool expectInvalidation = revision is 1 or 3 or 6;
                if (expectInvalidation && !consumer.Invalidated.WaitOne(TimeSpan.FromSeconds(10)))
                    throw new TimeoutException("Relevant change did not invalidate its consumer.");
            }
            await Send(Gh2Wire.Message.Shutdown, 0, []);
        });
        using var client = Gh2PipeClient.Connect(name);
        client.ReadProjectInfo();
        Task reader = Task.Run(() => client.WaitForShutdown(false));
        ArchicadChangeDelivery.Subscribe(consumer, name, [id], 8);
        var solver = new Host(failFirstDispatch);
        ArchicadChangeDelivery.Attach(client, new ArchicadInputRevisions(solver));
        try
        {
            host.WaitAsync(deadline.Token).GetAwaiter().GetResult();
            reader.WaitAsync(deadline.Token).GetAwaiter().GetResult();
            if (consumer.Count != 3) throw new InvalidOperationException("Unrelated/duplicate/inactive changes re-read the consumer.");
            lock (consumer.Changed)
                if (consumer.Changed.Any(ids => ids is null || !ids.SequenceEqual([id])))
                    throw new InvalidOperationException("A scoped change was widened beyond its own rows.");
            // A refused UI post must not strand rows that were already read.
            if (!SpinWait.SpinUntil(() => !consumer.Unadopted, TimeSpan.FromSeconds(10)) || consumer.Commits == 0 ||
                solver.Starts != consumer.Commits)
                throw new InvalidOperationException("Read rows were not adopted, or adoption did not start one solution each.");
        }
        finally { consumer.Active = false; ArchicadChangeDelivery.Detach(client); consumer.Invalidated.Dispose(); }
        Console.WriteLine("GH2 native-wake scoped delivery: baseline, relevant/unrelated, duplicate and source restart passed.");
    }
}
