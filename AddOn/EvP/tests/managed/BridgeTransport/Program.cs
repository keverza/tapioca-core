using System.Buffers.Binary;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using Tapioca.Gh2Worker;

using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(10));
string name = "Tapioca.Transport.Test." + Guid.NewGuid().ToString("N");
using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
Task<Gh2PipeClient> connecting = Task.Run(() => Gh2PipeClient.Connect(name));
await server.WaitForConnectionAsync(deadline.Token);
var hello = await Read();
Check(hello.Header.Type == Gh2Wire.Message.Hello, "hello");
await server.WriteAsync(Gh2Wire.EncodeHeader(Gh2Wire.Message.HelloAck, 0, 0, 4), deadline.Token);
await server.WriteAsync(new byte[] { 2, 0, 0, 0 }, deadline.Token);
using Gh2PipeClient client = await connecting.WaitAsync(deadline.Token);
Task reader = Task.Run(() => client.WaitForShutdown());
Guid subscribed = Guid.NewGuid();
Task<string> changeRead = client.ReadElementChangesAsync([subscribed], 9, 12, 8);
var changeRequest = await ReadRequest();
int changeCommandLength = (int)BinaryPrimitives.ReadUInt32LittleEndian(changeRequest.Body);
Check(Encoding.UTF8.GetString(changeRequest.Body, 8, changeCommandLength) == "Tapioca.GetGhElementChanges", "scoped change command");
using (var json = JsonDocument.Parse(changeRequest.Body.AsMemory(8 + changeCommandLength)))
{
    var data = json.RootElement;
    Check(data.GetProperty("guids")[0].GetGuid() == subscribed &&
        data.GetProperty("projectEpoch").GetInt64() == 9 &&
        data.GetProperty("sinceRevision").GetInt64() == 12 &&
        data.GetProperty("fields").GetInt32() == 8, "subscription cursor and mask on wire");
}
await Reply(changeRequest.Header.RequestId);
await changeRead.WaitAsync(deadline.Token);
try { _ = client.ReadElementChangesAsync([Guid.Empty], 9, 0); throw new Exception("Invalid subscription accepted"); }
catch (ArgumentOutOfRangeException) { }
Task<string> active = client.ReadSelectionAsync();
var first = await ReadRequest();
int current = 1;
Task<string> obsolete = client.SetOverlayAsync("{}", () => Volatile.Read(ref current) != 0);
var reads = Enumerable.Range(0, 15).Select(_ => client.ReadStoriesAsync()).ToArray();
await Refuses(client.ReadStoriesAsync());
Guid document = Guid.NewGuid(), producer = Guid.NewGuid();
var clears = Enumerable.Range(1, 4).Select(revision => client.ClearOverlayAsync(7, producer, 9, document, revision)).ToArray();
await Refuses(client.ClearOverlayAsync(7, producer, 9, document, 5));
Volatile.Write(ref current, 0);
await Reply(first.Header.RequestId);
await active.WaitAsync(deadline.Token);
var priority = await ReadRequest();
int commandLength = (int)BinaryPrimitives.ReadUInt32LittleEndian(priority.Body);
Check(Encoding.UTF8.GetString(priority.Body, 8, commandLength) == "Tapioca.ClearOverlayLayer", "Clear priority");
using (var json = JsonDocument.Parse(priority.Body.AsMemory(8 + commandLength)))
{
    Check(json.RootElement.GetProperty("projectEpoch").GetInt64() == 9, "epoch on wire");
    Check(json.RootElement.GetProperty("documentSession").GetString() == document.ToString("N"), "document on wire");
}
await Reply(priority.Header.RequestId);
for (int i = 1; i < clears.Length; ++i)
{
    var clear = await ReadRequest();
    int length = (int)BinaryPrimitives.ReadUInt32LittleEndian(clear.Body);
    Check(Encoding.UTF8.GetString(clear.Body, 8, length) == "Tapioca.ClearOverlayLayer", "all Clears precede normal reads");
    await Reply(clear.Header.RequestId);
}
await Task.WhenAll(clears).WaitAsync(deadline.Token);
try { await obsolete.WaitAsync(deadline.Token); throw new Exception("Obsolete Show accepted"); }
catch (OperationCanceledException) { }
var resumed = await ReadRequest();
int resumedLength = (int)BinaryPrimitives.ReadUInt32LittleEndian(resumed.Body);
Check(Encoding.UTF8.GetString(resumed.Body, 8, resumedLength) == "Tapioca.GetStories", "obsolete Show sends no frame and preserves connection");
client.Dispose();
foreach (var task in reads)
{
    try { await task.WaitAsync(deadline.Token); throw new Exception("Disposed request completed successfully"); }
    catch (Exception error) when (error is OperationCanceledException or IOException or ObjectDisposedException) { }
}
try { await reader.WaitAsync(deadline.Token); }
catch (Exception error) when (error is IOException or ObjectDisposedException) { }
Check(!deadline.IsCancellationRequested, "shutdown drains queues without waiting for request timeout");
Console.WriteLine("PASS: handshake, bounded queues, Clear priority, obsolete Show cancellation, epoch/document wire, shutdown cancellation");

async Task Refuses(Task<string> task)
{
    try { await task.WaitAsync(deadline.Token); throw new Exception("Queue overflow accepted"); }
    catch (InvalidOperationException) { }
}
async Task Reply(uint id)
{
    byte[] body = Encoding.UTF8.GetBytes("{\"ok\":true,\"data\":{}}");
    await server.WriteAsync(Gh2Wire.EncodeHeader(Gh2Wire.Message.ApiResponse, 0, id, body.Length), deadline.Token);
    await server.WriteAsync(body, deadline.Token);
}
async Task<(Gh2Wire.Header Header, byte[] Body)> ReadRequest()
{
    while (true) { var message = await Read(); if (message.Header.Type != Gh2Wire.Message.Heartbeat) return message; }
}
async Task<(Gh2Wire.Header Header, byte[] Body)> Read()
{
    byte[] frame = new byte[Gh2Wire.HeaderSize];
    await server.ReadExactlyAsync(frame, deadline.Token);
    var header = Gh2Wire.DecodeHeader(frame);
    byte[] body = new byte[header.PayloadBytes];
    await server.ReadExactlyAsync(body, deadline.Token);
    return (header, body);
}
static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
