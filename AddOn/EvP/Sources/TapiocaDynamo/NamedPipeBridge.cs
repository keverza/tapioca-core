using System.Buffers.Binary;
using System.IO.Pipes;
using System.Text;

namespace Tapioca;

internal sealed class NamedPipeBridge : ITapiocaBridge
{
    private const int MaxResponseBytes = 16 * 1024 * 1024;
    private const byte ResponseAck = 0x06;
    private readonly string pipeName;

    internal NamedPipeBridge()
    {
        pipeName = Environment.GetEnvironmentVariable("TAPIOCA_DYNAMO_PIPE")
            ?? throw new InvalidOperationException(
                "TAPIOCA_DYNAMO_PIPE is unavailable. Open Dynamo from Tapioca's Archicad menu.");
    }

    public string Call(string command, string paramsJson)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(command);
        ArgumentNullException.ThrowIfNull(paramsJson);

        byte[] commandBytes = Encoding.UTF8.GetBytes(command);
        byte[] paramsBytes = Encoding.UTF8.GetBytes(paramsJson);
        if (commandBytes.Length > 1024 || paramsBytes.Length > 8 * 1024 * 1024)
            throw new ArgumentOutOfRangeException(nameof(paramsJson), "The Tapioca request is too large.");

        using var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.None);
        pipe.Connect(5000);

        Span<byte> header = stackalloc byte[8];
        BinaryPrimitives.WriteUInt32LittleEndian(header, (uint)commandBytes.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(header[4..], (uint)paramsBytes.Length);
        pipe.Write(header);
        pipe.Write(commandBytes);
        pipe.Write(paramsBytes);
        pipe.Flush();

        Span<byte> responseHeader = stackalloc byte[4];
        pipe.ReadExactly(responseHeader);
        uint responseLength = BinaryPrimitives.ReadUInt32LittleEndian(responseHeader);
        if (responseLength == 0 || responseLength > MaxResponseBytes)
            throw new InvalidDataException("Tapioca returned an invalid response length.");

        byte[] response = new byte[responseLength];
        pipe.ReadExactly(response);
        pipe.WriteByte(ResponseAck);
        return Encoding.UTF8.GetString(response);
    }
}
