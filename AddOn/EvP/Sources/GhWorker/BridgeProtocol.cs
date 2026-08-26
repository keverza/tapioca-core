using System;
using System.Collections.Generic;
using System.Text;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// The worker's copy of the wire format in
    /// <c>Sources/AddOn/Grasshopper/GhProtocol.hpp</c>.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ A COPY, DELIBERATELY, AND NEITHER HALF INFERS THE OTHER'S. The two ends
    /// are a C++ .apx and a C# process; there is no shared header they could both
    /// include and no compiler they both trust to lay out a struct the same way.
    /// So the format is written out twice, by hand, little-endian, and the
    /// HANDSHAKE compares the two version numbers rather than assuming they
    /// match. A mismatch is a refusal that names both numbers, which is the one
    /// diagnostic that makes a stale worker beside an upgraded add-on obvious.
    /// </para>
    /// <para>
    /// ⚠️ CHANGING ANYTHING HERE MEANS CHANGING GhProtocol.hpp IN THE SAME EDIT
    /// AND BUMPING <see cref="Version"/> IN BOTH.
    /// </para>
    /// </remarks>
    internal static class BridgeProtocol
    {
        /// <summary>v2 added RunDefinition, CancelRun and RunResult.</summary>
        internal const uint Version = 2;

        /// <summary>protocolVersion, messageType, requestId, correlationId, payloadBytes.</summary>
        internal const int HeaderSize = 20;

        internal const uint MaxPayloadBytes = 16u * 1024u * 1024u;

        internal const uint MaxCommandBytes = 1024;

        internal enum MessageType : uint
        {
            Hello = 1,
            HelloAck = 2,
            Heartbeat = 3,
            ApiRequest = 4,
            ApiResponse = 5,
            ShowEditor = 6,
            HideEditor = 7,
            Shutdown = 8,
            Ack = 9,
            Log = 10,
            RunDefinition = 11,
            CancelRun = 12,
            RunResult = 13,
        }

        internal enum AckStatus : uint
        {
            Ok = 0,
            Failed = 1,
            NotReady = 2,
        }

        internal struct Header
        {
            internal uint ProtocolVersion;
            internal MessageType Type;
            internal uint RequestId;
            internal uint CorrelationId;
            internal uint PayloadBytes;
        }

        internal static byte[] EncodeHeader(MessageType type, uint requestId, uint correlationId, int payloadBytes)
        {
            byte[] header = new byte[HeaderSize];
            WriteUInt32(header, 0, Version);
            WriteUInt32(header, 4, (uint)type);
            WriteUInt32(header, 8, requestId);
            WriteUInt32(header, 12, correlationId);
            WriteUInt32(header, 16, (uint)payloadBytes);
            return header;
        }

        /// <summary>
        /// Reads a header, refusing a wrong version and an oversized payload
        /// before either can be acted on.
        /// </summary>
        internal static bool DecodeHeader(byte[] bytes, out Header header, out string error)
        {
            header = default(Header);
            error = string.Empty;
            if (bytes == null || bytes.Length < HeaderSize)
            {
                error = "The message header was short.";
                return false;
            }

            uint version = ReadUInt32(bytes, 0);
            if (version != Version)
            {
                error = "The add-on speaks bridge protocol " + version + " and this worker speaks " + Version
                        + ". Rebuild and redeploy both halves together.";
                return false;
            }

            uint payloadBytes = ReadUInt32(bytes, 16);
            if (payloadBytes > MaxPayloadBytes)
            {
                error = "The message payload claimed " + payloadBytes + " bytes, over the " + MaxPayloadBytes
                        + "-byte limit.";
                return false;
            }

            header.ProtocolVersion = version;
            header.Type = (MessageType)ReadUInt32(bytes, 4);
            header.RequestId = ReadUInt32(bytes, 8);
            header.CorrelationId = ReadUInt32(bytes, 12);
            header.PayloadBytes = payloadBytes;
            return true;
        }

        internal static byte[] EncodeHelloPayload(int processId)
        {
            byte[] payload = new byte[8];
            WriteUInt32(payload, 0, (uint)processId);
            WriteUInt32(payload, 4, 0u); // capabilities: none beyond protocol v1
            return payload;
        }

        internal static byte[] EncodeApiRequestPayload(string command, string parametersJson)
        {
            byte[] commandBytes = Encoding.UTF8.GetBytes(command ?? string.Empty);
            byte[] parameterBytes = Encoding.UTF8.GetBytes(parametersJson ?? string.Empty);
            byte[] payload = new byte[8 + commandBytes.Length + parameterBytes.Length];
            WriteUInt32(payload, 0, (uint)commandBytes.Length);
            WriteUInt32(payload, 4, (uint)parameterBytes.Length);
            Buffer.BlockCopy(commandBytes, 0, payload, 8, commandBytes.Length);
            Buffer.BlockCopy(parameterBytes, 0, payload, 8 + commandBytes.Length, parameterBytes.Length);
            return payload;
        }

        internal static byte[] EncodeAckPayload(AckStatus status, string message)
        {
            byte[] messageBytes = Encoding.UTF8.GetBytes(message ?? string.Empty);
            byte[] payload = new byte[4 + messageBytes.Length];
            WriteUInt32(payload, 0, (uint)status);
            Buffer.BlockCopy(messageBytes, 0, payload, 4, messageBytes.Length);
            return payload;
        }

        /// <summary>
        /// Mirrors protocol::EncodeRunReportPayload: ok, elapsed, the two counts,
        /// then the headline and every message as a length-prefixed UTF-8 run.
        /// </summary>
        internal static byte[] EncodeRunReportPayload(RunReport report)
        {
            List<byte[]> strings = new List<byte[]>();
            strings.Add(Encoding.UTF8.GetBytes(report.Headline));
            foreach (string message in report.Errors)
            {
                strings.Add(Encoding.UTF8.GetBytes(message ?? string.Empty));
            }

            foreach (string message in report.Warnings)
            {
                strings.Add(Encoding.UTF8.GetBytes(message ?? string.Empty));
            }

            int total = 16;
            foreach (byte[] encoded in strings)
            {
                total += 4 + encoded.Length;
            }

            byte[] payload = new byte[total];
            WriteUInt32(payload, 0, report.Ok ? 1u : 0u);
            // Clamped rather than cast: a solve that somehow ran for 50 days must
            // report a large number, not wrap round to a small one.
            WriteUInt32(payload, 4, report.ElapsedMs < 0 ? 0u
                : report.ElapsedMs > uint.MaxValue ? uint.MaxValue : (uint)report.ElapsedMs);
            WriteUInt32(payload, 8, (uint)report.Errors.Count);
            WriteUInt32(payload, 12, (uint)report.Warnings.Count);

            int offset = 16;
            foreach (byte[] encoded in strings)
            {
                WriteUInt32(payload, offset, (uint)encoded.Length);
                offset += 4;
                Buffer.BlockCopy(encoded, 0, payload, offset, encoded.Length);
                offset += encoded.Length;
            }

            return payload;
        }

        internal static byte[] EncodeTextPayload(string text)
        {
            return Encoding.UTF8.GetBytes(text ?? string.Empty);
        }

        internal static string DecodeTextPayload(byte[] payload)
        {
            return payload == null || payload.Length == 0 ? string.Empty : Encoding.UTF8.GetString(payload);
        }

        private static void WriteUInt32(byte[] buffer, int offset, uint value)
        {
            buffer[offset] = (byte)(value & 0xFFu);
            buffer[offset + 1] = (byte)((value >> 8) & 0xFFu);
            buffer[offset + 2] = (byte)((value >> 16) & 0xFFu);
            buffer[offset + 3] = (byte)((value >> 24) & 0xFFu);
        }

        private static uint ReadUInt32(byte[] buffer, int offset)
        {
            return (uint)buffer[offset]
                   | ((uint)buffer[offset + 1] << 8)
                   | ((uint)buffer[offset + 2] << 16)
                   | ((uint)buffer[offset + 3] << 24);
        }
    }
}
