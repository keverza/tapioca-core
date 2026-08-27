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
        /// <summary>
        /// v2 added RunDefinition, CancelRun and RunResult.
        /// v3 added the undo ledger to the run report.
        /// v4 added the preview messages and the Preview capability bit that
        /// gates them. Their payload codec lives in the GHA
        /// (Sources/GrasshopperComponents/PreviewChannel.cs), not here: capture
        /// is the .gha's job and the worker only relays the frames.
        /// </summary>
        internal const uint Version = 4;

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

            // ---- preview, worker -> host unless noted --------------------
            // Gated by Capabilities.Preview below. A worker that did not
            // negotiate it must not send any of these, and a host that cleared
            // it must refuse them: OFF COSTS NOTHING is a rule about the
            // handshake, not about dropping messages after they arrive.
            PreviewBeginBatch = 14,
            PreviewAdded = 15,
            PreviewChanged = 16,
            PreviewRemoved = 17,
            PreviewVisibility = 18,
            PreviewSelection = 19,
            PreviewEndBatch = 20,
            PreviewDropAll = 21,
            PreviewResyncRequest = 22,
            PreviewBatchAck = 23,
            PreviewPicked = 24,
        }

        /// <summary>
        /// Bits in the hello's capabilities word. Mirrors CapabilityPreview in
        /// Sources/AddOn/Grasshopper/GhPreviewProtocol.hpp.
        /// </summary>
        [System.Flags]
        internal enum Capabilities : uint
        {
            None = 0,
            Preview = 1u << 0,
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

        internal static byte[] EncodeHelloPayload(int processId, Capabilities offered)
        {
            byte[] payload = new byte[8];
            WriteUInt32(payload, 0, (uint)processId);
            // ⚠️ AN OFFER, NOT A DECISION. The host answers with what it
            // GRANTED, and only that word may be acted on: a worker that treated
            // its own offer as the answer would collect, convert and send
            // preview into an add-on that has it switched off, which is exactly
            // the cost "preview off costs nothing" promises never to pay.
            WriteUInt32(payload, 4, (uint)offered);
            return payload;
        }

        /// <summary>
        /// The add-on's answer: the granted capability word, and a refusal
        /// reason that is empty on acceptance.
        /// </summary>
        /// <remarks>
        /// ⚠️ ONE SHAPE FOR BOTH OUTCOMES. Mirrors HelloAckPayload in
        /// GhProtocol.hpp, including its named cost: an add-on speaking an OLDER
        /// protocol answers a version refusal in raw UTF-8, so its first four
        /// characters land here as a capability word and its reason reads four
        /// characters short. That path is already an error, and the alternative
        /// -- discriminating the shape by payload length -- cannot tell a
        /// refusal sentence from a capability word at all.
        /// </remarks>
        internal static bool DecodeHelloAckPayload(
            byte[] payload, out Capabilities granted, out string refusal, out string error)
        {
            granted = Capabilities.None;
            refusal = string.Empty;
            error = string.Empty;

            if (payload == null || payload.Length < 4)
            {
                error = "The add-on's handshake answer was short.";
                return false;
            }

            granted = (Capabilities)ReadUInt32(payload, 0);
            refusal = payload.Length > 4
                ? Encoding.UTF8.GetString(payload, 4, payload.Length - 4)
                : string.Empty;
            return true;
        }

        /// <summary>
        /// The batch ack that RELEASES a segment: which batch, and whether the
        /// host took it. Mirrors PreviewBatchAckPayload in GhPreviewProtocol.hpp.
        /// </summary>
        internal static bool DecodePreviewBatchAckPayload(
            byte[] payload, out uint epoch, out uint revision, out bool accepted, out string reason, out string error)
        {
            epoch = 0;
            revision = 0;
            accepted = false;
            reason = string.Empty;
            error = string.Empty;

            // epoch, revision, accepted (as a uint32), reasonLength, reason.
            if (payload == null || payload.Length < 16)
            {
                error = "A preview batch ack was short.";
                return false;
            }

            uint acceptedWord = ReadUInt32(payload, 8);
            if (acceptedWord > 1u)
            {
                error = "A preview batch ack carried " + acceptedWord + " where a flag was expected.";
                return false;
            }

            long reasonBytes = ReadUInt32(payload, 12);
            if (16 + reasonBytes > payload.Length)
            {
                error = "A preview batch ack declared more reason text than it carried.";
                return false;
            }

            epoch = ReadUInt32(payload, 0);
            revision = ReadUInt32(payload, 4);
            accepted = acceptedWord != 0;
            reason = reasonBytes == 0 ? string.Empty : Encoding.UTF8.GetString(payload, 16, (int)reasonBytes);
            return true;
        }

        /// <summary>
        /// "Your next batch must be a full one." Mirrors
        /// PreviewResyncRequestPayload in GhPreviewProtocol.hpp.
        /// </summary>
        internal static bool DecodePreviewResyncPayload(
            byte[] payload, out uint epoch, out string reason, out string error)
        {
            epoch = 0;
            reason = string.Empty;
            error = string.Empty;

            if (payload == null || payload.Length < 8)
            {
                error = "A preview resync request was short.";
                return false;
            }

            long reasonBytes = ReadUInt32(payload, 4);
            if (8 + reasonBytes > payload.Length)
            {
                error = "A preview resync request declared more reason text than it carried.";
                return false;
            }

            epoch = ReadUInt32(payload, 0);
            reason = reasonBytes == 0 ? string.Empty : Encoding.UTF8.GetString(payload, 8, (int)reasonBytes);
            return true;
        }

        /// <summary>
        /// A viewport pick resolved to one primitive id. Mirrors
        /// PreviewPickedPayload in GhPreviewProtocol.hpp.
        /// </summary>
        internal static bool DecodePreviewPickedPayload(byte[] payload, out ulong primitiveId, out string error)
        {
            primitiveId = 0;
            error = string.Empty;
            if (payload == null || payload.Length != 8)
            {
                error = "A preview pick was not 8 bytes.";
                return false;
            }

            primitiveId = ReadUInt32(payload, 0) | ((ulong)ReadUInt32(payload, 4) << 32);
            if (primitiveId == 0)
            {
                error = "A preview pick carried no primitive id.";
                return false;
            }

            return true;
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

            List<KeyValuePair<string, uint>> ledger =
                report.Ledger ?? new List<KeyValuePair<string, uint>>();

            int total = 20;
            foreach (byte[] encoded in strings)
            {
                total += 4 + encoded.Length;
            }

            List<byte[]> ledgerNames = new List<byte[]>();
            foreach (KeyValuePair<string, uint> entry in ledger)
            {
                byte[] encoded = Encoding.UTF8.GetBytes(entry.Key ?? string.Empty);
                ledgerNames.Add(encoded);
                total += 4 + encoded.Length + 4;
            }

            byte[] payload = new byte[total];
            WriteUInt32(payload, 0, report.Ok ? 1u : 0u);
            // Clamped rather than cast: a solve that somehow ran for 50 days must
            // report a large number, not wrap round to a small one.
            WriteUInt32(payload, 4, report.ElapsedMs < 0 ? 0u
                : report.ElapsedMs > uint.MaxValue ? uint.MaxValue : (uint)report.ElapsedMs);
            WriteUInt32(payload, 8, (uint)report.Errors.Count);
            WriteUInt32(payload, 12, (uint)report.Warnings.Count);
            WriteUInt32(payload, 16, (uint)ledger.Count);

            int offset = 20;
            foreach (byte[] encoded in strings)
            {
                WriteUInt32(payload, offset, (uint)encoded.Length);
                offset += 4;
                Buffer.BlockCopy(encoded, 0, payload, offset, encoded.Length);
                offset += encoded.Length;
            }

            for (int index = 0; index < ledger.Count; index++)
            {
                byte[] encoded = ledgerNames[index];
                WriteUInt32(payload, offset, (uint)encoded.Length);
                offset += 4;
                Buffer.BlockCopy(encoded, 0, payload, offset, encoded.Length);
                offset += encoded.Length;
                WriteUInt32(payload, offset, ledger[index].Value);
                offset += 4;
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
