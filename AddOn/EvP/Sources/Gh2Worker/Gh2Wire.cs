using System.Buffers.Binary;
using System.Text;

namespace Tapioca.Gh2Worker;

// Both GH2 clients speak the existing bridge's v5 control framing, with an
// additive engine capability. The C++ authority is Grasshopper/GhProtocol.hpp.
// GH1's BridgeProtocol.cs and its wire semantics are left untouched.
internal static class Gh2Wire
{
    internal const uint Version = 5;
    internal const int HeaderSize = 20;
    internal const int MaxPayload = 16 * 1024 * 1024;
    internal const int MaxPingResponse = 64 * 1024;
    internal const uint CapabilityGh2 = 1u << 1;

    internal enum Message : uint
    {
        Hello = 1,
        HelloAck = 2,
        Heartbeat = 3,
        ApiRequest = 4,
        ApiResponse = 5,
        Shutdown = 8,
        Ack = 9,
        Log = 10,
    }

    internal readonly record struct Header(uint Version, Message Type, uint RequestId, uint CorrelationId, int PayloadBytes);

    internal static byte[] EncodeHeader(Message type, uint requestId, uint correlationId, int payloadBytes)
    {
        if (payloadBytes < 0 || payloadBytes > MaxPayload)
            throw new ArgumentOutOfRangeException(nameof(payloadBytes));
        byte[] bytes = new byte[HeaderSize];
        Write(bytes, 0, Version);
        Write(bytes, 4, (uint)type);
        Write(bytes, 8, requestId);
        Write(bytes, 12, correlationId);
        Write(bytes, 16, (uint)payloadBytes);
        return bytes;
    }

    internal static Header DecodeHeader(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length != HeaderSize)
            throw new InvalidDataException("The bridge header was not 20 bytes.");
        uint version = Read(bytes, 0);
        if (version != Version)
            throw new InvalidDataException($"The add-on speaks protocol {version}; this GH2 client speaks {Version}.");
        uint size = Read(bytes, 16);
        if (size > MaxPayload)
            throw new InvalidDataException("The bridge payload exceeds its limit.");
        Message type = (Message)Read(bytes, 4);
        if (!Enum.IsDefined(type))
            throw new InvalidDataException($"Unknown GH2 bridge message {(uint)type}.");
        return new Header(version, type, Read(bytes, 8), Read(bytes, 12), (int)size);
    }

    internal static byte[] Hello(int pid)
    {
        if (pid <= 0)
            throw new ArgumentOutOfRangeException(nameof(pid));
        byte[] bytes = new byte[8];
        Write(bytes, 0, (uint)pid);
        Write(bytes, 4, CapabilityGh2);
        return bytes;
    }

    internal static void VerifyHelloAck(ReadOnlySpan<byte> body)
    {
        if (body.Length < 4)
            throw new InvalidDataException("The bridge's hello answer is short.");
        string refusal = Encoding.UTF8.GetString(body[4..]);
        if (refusal.IndexOf('\0') >= 0)
            throw new InvalidDataException("The bridge refused with an embedded NUL.");
        if (refusal.Length != 0)
            throw new InvalidDataException(refusal);
        if ((Read(body, 0) & CapabilityGh2) == 0)
            throw new InvalidDataException("The bridge did not grant GH2; refusing to speak to a GH1 session.");
    }

    internal static byte[] ApiRequest(string command, string parameters)
    {
        byte[] name = Encoding.UTF8.GetBytes(command);
        byte[] data = Encoding.UTF8.GetBytes(parameters);
        if (name.Length == 0 || name.Length > 1024 || 8L + name.Length + data.Length > MaxPayload)
            throw new ArgumentOutOfRangeException(nameof(command));
        byte[] bytes = new byte[8 + name.Length + data.Length];
        Write(bytes, 0, (uint)name.Length);
        Write(bytes, 4, (uint)data.Length);
        name.CopyTo(bytes, 8);
        data.CopyTo(bytes, 8 + name.Length);
        return bytes;
    }

    internal static byte[] Ack(bool ready, string message)
    {
        byte[] text = Encoding.UTF8.GetBytes(message);
        byte[] bytes = new byte[4 + text.Length];
        Write(bytes, 0, ready ? 0u : 1u);
        text.CopyTo(bytes, 4);
        return bytes;
    }

    private static void Write(Span<byte> bytes, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[offset..], value);

    private static uint Read(ReadOnlySpan<byte> bytes, int offset) =>
        BinaryPrimitives.ReadUInt32LittleEndian(bytes[offset..]);
}
