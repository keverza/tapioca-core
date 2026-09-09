using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// The worker's copy of the session wire format in
    /// <c>Sources/AddOn/Grasshopper/GhSessionProtocol.hpp</c>.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ A COPY, DELIBERATELY, FOR THE REASON <see cref="BridgeProtocol"/>
    /// GIVES: a C++ .apx and a C# process share no header and no compiler they
    /// both trust to lay out a struct the same way. Changing anything here means
    /// changing GhSessionProtocol.hpp in the same edit, and bumping
    /// <see cref="BridgeProtocol.Version"/> in both.
    /// </para>
    /// <para>
    /// ⚠️ THIS DECODES WHAT THE HOST SENDS AND ENCODES WHAT THE WORKER ANSWERS,
    /// AND THAT ASYMMETRY IS THE POINT. The direction of every message is part of
    /// the contract; a worker that could encode an OpenSession would be a worker
    /// that could invent a session the host does not know about, which is exactly
    /// what the host-assigned session id exists to prevent.
    /// </para>
    /// <para>
    /// No Rhino or Grasshopper type is named here. This file is reachable before
    /// the resolver is installed — <see cref="BridgeClient"/> decodes on its
    /// reader thread from the first message on — and naming one would make the
    /// CLR try to load RhinoCommon from wherever it is not.
    /// </para>
    /// </remarks>
    internal static class SessionProtocol
    {
        /// <summary>hostGeneration, sessionId, requestRevision.</summary>
        internal const int EnvelopeSize = 12;

        internal const uint MaxSessionTextBytes = 64u * 1024u;

        internal const uint MaxDefinitionPathBytes = 4096;

        internal const uint MaxSchemaBytes = 4u * 1024u * 1024u;

        internal const uint MaxSessionInputs = 4096;

        internal const uint MaxSessionOutputs = 4096;

        internal const uint MaxSessionDiagnostics = 1024;

        internal enum SessionMode : uint
        {
            Headless = 0,
            Authoring = 1,
        }

        internal enum SessionState : uint
        {
            Empty = 0,
            Loading = 1,
            Loaded = 2,
            Solving = 3,
            Published = 4,
            Invalid = 5,
            Cancelling = 6,
            Reloading = 7,
            HostRestartRequired = 8,
        }

        [Flags]
        internal enum SolveWants : uint
        {
            Nothing = 0,
            Preview = 1u << 0,
            Data = 1u << 1,
            Commit = 1u << 2,
        }

        internal const uint SolveWantsAll =
            (uint)(SolveWants.Preview | SolveWants.Data | SolveWants.Commit);

        internal enum FailureCode : uint
        {
            None = 0,
            RuntimeMissing = 1,
            LicenceUnavailable = 2,
            RuntimeStartFailed = 3,
            DefinitionInvalid = 4,
            DependencyMissing = 5,
            ContractInvalid = 6,
            InputInvalid = 7,
            SolveFailed = 8,
            SolveTimedOut = 9,
            Cancelled = 10,
            HostDisconnected = 11,
            HostCrashed = 12,
            PreviewInvalid = 13,
            CommitInvalid = 14,
            StaleRevision = 15,
            AccessDenied = 16,
            CapacityExceeded = 17,
            ProtocolMismatch = 18,
            SessionUnknown = 19,
        }

        internal enum DiagnosticLevel : uint
        {
            Error = 0,
            Warning = 1,
            Remark = 2,
        }

        /// <summary>The routing prefix on every session message.</summary>
        internal struct Envelope
        {
            internal uint HostGeneration;

            internal uint SessionId;

            internal uint RequestRevision;

            internal Envelope(uint hostGeneration, uint sessionId, uint requestRevision)
            {
                HostGeneration = hostGeneration;
                SessionId = sessionId;
                RequestRevision = requestRevision;
            }

            public override string ToString()
            {
                return "generation " + HostGeneration.ToString(CultureInfo.InvariantCulture)
                       + ", session " + SessionId.ToString(CultureInfo.InvariantCulture)
                       + ", request " + RequestRevision.ToString(CultureInfo.InvariantCulture);
            }
        }

        internal sealed class InputValue
        {
            internal InputValue(string id, string value)
            {
                Id = id ?? string.Empty;
                Value = value ?? string.Empty;
            }

            internal string Id { get; private set; }

            internal string Value { get; private set; }
        }

        internal sealed class OutputValue
        {
            internal OutputValue(string id, string type, string path, string value)
            {
                Id = id ?? string.Empty;
                Type = type ?? string.Empty;
                Path = path ?? string.Empty;
                Value = value ?? string.Empty;
            }

            internal string Id { get; private set; }

            internal string Type { get; private set; }

            /// <summary>
            /// The complete GH tree path, as Grasshopper renders it ("{0;1}"),
            /// or empty for a value with none. Never flattened away: a flattened
            /// tree cannot be unflattened, and the panel is required to keep an
            /// unsupported one inspectable.
            /// </summary>
            internal string Path { get; private set; }

            internal string Value { get; private set; }
        }

        internal sealed class Diagnostic
        {
            internal Diagnostic(DiagnosticLevel level, string component, string text)
            {
                Level = level;
                Component = component ?? string.Empty;
                Text = text ?? string.Empty;
            }

            internal DiagnosticLevel Level { get; private set; }

            internal string Component { get; private set; }

            internal string Text { get; private set; }
        }

        // ---- host -> worker: decoding ------------------------------------

        internal static bool DecodeEnvelopeOnly(byte[] payload, string what, out Envelope envelope, out string error)
        {
            int offset;
            if (!ReadEnvelope(payload, out envelope, out offset, out error))
            {
                return false;
            }

            if (payload.Length != EnvelopeSize)
            {
                error = "The " + what + " carried " + payload.Length + " bytes where " + EnvelopeSize
                        + " were expected.";
                return false;
            }

            return true;
        }

        internal static bool DecodeOpenSession(
            byte[] payload, out Envelope envelope, out SessionMode mode, out string error)
        {
            mode = SessionMode.Headless;
            int offset;
            if (!ReadEnvelope(payload, out envelope, out offset, out error))
            {
                return false;
            }

            uint word;
            if (!ReadUInt32(payload, ref offset, "the open-session request's mode", out word, out error))
            {
                return false;
            }

            if (word > (uint)SessionMode.Authoring)
            {
                error = "The open-session request asked for an unknown mode " + word + ".";
                return false;
            }

            if (envelope.SessionId == 0)
            {
                // The host assigns ids. A zero is the host failing to fill one
                // in, and every later message for that session would then
                // address a session nothing owns.
                error = "The open-session request carried no session id.";
                return false;
            }

            if (!AtEnd(payload, offset, "open-session request", out error))
            {
                return false;
            }

            mode = (SessionMode)word;
            return true;
        }

        internal static bool DecodeSetSessionMode(
            byte[] payload, out Envelope envelope, out SessionMode mode, out string error)
        {
            mode = SessionMode.Headless;
            int offset;
            if (!ReadEnvelope(payload, out envelope, out offset, out error))
            {
                return false;
            }

            uint word;
            if (!ReadUInt32(payload, ref offset, "the set-mode request's mode", out word, out error))
            {
                return false;
            }

            if (word > (uint)SessionMode.Authoring)
            {
                error = "The set-mode request asked for an unknown mode " + word + ".";
                return false;
            }

            if (!AtEnd(payload, offset, "set-mode request", out error))
            {
                return false;
            }

            mode = (SessionMode)word;
            return true;
        }

        internal static bool DecodeLoadDefinition(
            byte[] payload, out Envelope envelope, out string path, out string error)
        {
            path = string.Empty;
            int offset;
            if (!ReadEnvelope(payload, out envelope, out offset, out error))
            {
                return false;
            }

            if (!ReadString(payload, ref offset, MaxDefinitionPathBytes, "the definition path", out path, out error))
            {
                return false;
            }

            if (path.Length == 0)
            {
                error = "The load request carried no definition path.";
                return false;
            }

            return AtEnd(payload, offset, "load request", out error);
        }

        internal static bool DecodeSetInputs(
            byte[] payload, out Envelope envelope, out IList<InputValue> inputs, out string error)
        {
            inputs = null;
            int offset;
            if (!ReadEnvelope(payload, out envelope, out offset, out error))
            {
                return false;
            }

            uint count;
            if (!ReadUInt32(payload, ref offset, "the set-inputs request's count", out count, out error))
            {
                return false;
            }

            if (count > MaxSessionInputs)
            {
                error = "The set-inputs request declared " + count + " inputs, over the " + MaxSessionInputs
                        + " limit.";
                return false;
            }

            List<InputValue> decoded = new List<InputValue>((int)count);
            for (uint index = 0; index < count; index++)
            {
                string id;
                string value;
                if (!ReadString(payload, ref offset, MaxSessionTextBytes, "an input id", out id, out error))
                {
                    return false;
                }

                if (id.Length == 0)
                {
                    error = "The set-inputs request carried a value with no input id.";
                    return false;
                }

                if (!ReadString(payload, ref offset, MaxSessionTextBytes, "an input value", out value, out error))
                {
                    return false;
                }

                decoded.Add(new InputValue(id, value));
            }

            if (!AtEnd(payload, offset, "set-inputs request", out error))
            {
                return false;
            }

            inputs = decoded;
            return true;
        }

        internal static bool DecodeSolve(byte[] payload, out Envelope envelope, out uint wants, out string error)
        {
            wants = 0;
            int offset;
            if (!ReadEnvelope(payload, out envelope, out offset, out error))
            {
                return false;
            }

            if (!ReadUInt32(payload, ref offset, "the solve request's wants word", out wants, out error))
            {
                return false;
            }

            // ⚠️ AN UNKNOWN BIT IS REFUSED, UNLIKE AN UNKNOWN CAPABILITY BIT.
            // Capabilities are additive within a version; a solve's wants are
            // not. Honouring only the bits this build knows would collect less
            // than the host asked for and report success for it.
            if ((wants & ~SolveWantsAll) != 0)
            {
                error = "The solve request asked for collection this worker does not know: " + wants + ".";
                return false;
            }

            if (wants == 0)
            {
                error = "The solve request asked for nothing to be collected.";
                return false;
            }

            return AtEnd(payload, offset, "solve request", out error);
        }

        // ---- worker -> host: encoding ------------------------------------

        internal static byte[] EncodeEnvelopeOnly(Envelope envelope)
        {
            List<byte> payload = new List<byte>(EnvelopeSize);
            WriteEnvelope(payload, envelope);
            return payload.ToArray();
        }

        internal static byte[] EncodeSessionAck(
            Envelope envelope, SessionState state, FailureCode failure, string message)
        {
            List<byte> payload = new List<byte>(EnvelopeSize + 32);
            WriteEnvelope(payload, envelope);
            WriteUInt32(payload, (uint)state);
            WriteUInt32(payload, (uint)failure);
            WriteString(payload, Clip(message, MaxSessionTextBytes));
            return payload.ToArray();
        }

        internal static byte[] EncodeSchemaResult(Envelope envelope, string definitionIdentity, string schemaJson)
        {
            List<byte> payload = new List<byte>(EnvelopeSize + 64);
            WriteEnvelope(payload, envelope);
            WriteString(payload, Clip(definitionIdentity, MaxSessionTextBytes));
            WriteString(payload, Clip(schemaJson, MaxSchemaBytes));
            return payload.ToArray();
        }

        internal static byte[] EncodeSolutionResult(
            Envelope envelope,
            uint solutionRevision,
            uint elapsedMs,
            uint previewEpoch,
            uint previewRevision,
            string definitionIdentity,
            string inputSnapshotHash,
            IList<OutputValue> outputs,
            IList<Diagnostic> diagnostics)
        {
            IList<OutputValue> safeOutputs = outputs ?? new List<OutputValue>();
            IList<Diagnostic> safeDiagnostics = diagnostics ?? new List<Diagnostic>();

            List<byte> payload = new List<byte>(EnvelopeSize + 128);
            WriteEnvelope(payload, envelope);
            WriteUInt32(payload, solutionRevision);
            WriteUInt32(payload, elapsedMs);
            WriteUInt32(payload, previewEpoch);
            WriteUInt32(payload, previewRevision);
            WriteUInt32(payload, (uint)safeOutputs.Count);
            WriteUInt32(payload, (uint)safeDiagnostics.Count);
            WriteString(payload, Clip(definitionIdentity, MaxSessionTextBytes));
            WriteString(payload, Clip(inputSnapshotHash, MaxSessionTextBytes));
            foreach (OutputValue output in safeOutputs)
            {
                WriteString(payload, Clip(output.Id, MaxSessionTextBytes));
                WriteString(payload, Clip(output.Type, MaxSessionTextBytes));
                WriteString(payload, Clip(output.Path, MaxSessionTextBytes));
                WriteString(payload, Clip(output.Value, MaxSessionTextBytes));
            }

            WriteDiagnostics(payload, safeDiagnostics);
            return payload.ToArray();
        }

        internal static byte[] EncodeSolutionFailed(
            Envelope envelope, FailureCode failure, uint elapsedMs, string message, IList<Diagnostic> diagnostics)
        {
            IList<Diagnostic> safeDiagnostics = diagnostics ?? new List<Diagnostic>();
            List<byte> payload = new List<byte>(EnvelopeSize + 64);
            WriteEnvelope(payload, envelope);
            WriteUInt32(payload, (uint)failure);
            WriteUInt32(payload, elapsedMs);
            WriteUInt32(payload, (uint)safeDiagnostics.Count);
            WriteString(payload, Clip(message, MaxSessionTextBytes));
            WriteDiagnostics(payload, safeDiagnostics);
            return payload.ToArray();
        }

        internal static byte[] EncodeDiagnosticsResult(Envelope envelope, string report)
        {
            List<byte> payload = new List<byte>(EnvelopeSize + 64);
            WriteEnvelope(payload, envelope);
            WriteString(payload, Clip(report, MaxSchemaBytes));
            return payload.ToArray();
        }

        internal static string Describe(FailureCode failure)
        {
            switch (failure)
            {
                case FailureCode.None:
                    return "no failure";
                case FailureCode.RuntimeMissing:
                    return "Rhino 8 was not found on this machine";
                case FailureCode.LicenceUnavailable:
                    return "Rhino is installed but no licence is available";
                case FailureCode.RuntimeStartFailed:
                    return "Rhino would not start";
                case FailureCode.DefinitionInvalid:
                    return "the definition could not be read";
                case FailureCode.DependencyMissing:
                    return "the definition needs a Grasshopper package that is not installed";
                case FailureCode.ContractInvalid:
                    return "the definition's Tapioca inputs or outputs are not usable";
                case FailureCode.InputInvalid:
                    return "an input value was rejected";
                case FailureCode.SolveFailed:
                    return "the solution failed";
                case FailureCode.SolveTimedOut:
                    return "the solution ran past its time limit";
                case FailureCode.Cancelled:
                    return "the solution was cancelled";
                case FailureCode.HostDisconnected:
                    return "the Grasshopper host disconnected";
                case FailureCode.HostCrashed:
                    return "the Grasshopper host stopped unexpectedly";
                case FailureCode.PreviewInvalid:
                    return "the preview this solution produced could not be read";
                case FailureCode.CommitInvalid:
                    return "the commit this solution produced did not validate";
                case FailureCode.StaleRevision:
                    return "a newer request replaced this one";
                case FailureCode.AccessDenied:
                    return "that is not allowed in this session mode";
                case FailureCode.CapacityExceeded:
                    return "the request was over a size or rate limit";
                case FailureCode.ProtocolMismatch:
                    return "the add-on and the Grasshopper host speak different protocols";
                case FailureCode.SessionUnknown:
                    return "the Grasshopper host has no such session";
                default:
                    return "an unrecognised failure";
            }
        }

        // ---- primitives --------------------------------------------------

        private static void WriteDiagnostics(List<byte> payload, IList<Diagnostic> diagnostics)
        {
            foreach (Diagnostic diagnostic in diagnostics)
            {
                WriteUInt32(payload, (uint)diagnostic.Level);
                WriteString(payload, Clip(diagnostic.Component, MaxSessionTextBytes));
                WriteString(payload, Clip(diagnostic.Text, MaxSessionTextBytes));
            }
        }

        /// <summary>
        /// Cuts a string to the ceiling its field declares, at a whole character.
        /// </summary>
        /// <remarks>
        /// ⚠️ CLIPPED HERE RATHER THAN REFUSED BY THE HOST. A component that
        /// emits a megabyte of text in one runtime message is a definition
        /// problem, not a transport problem, and dropping the whole solution over
        /// it would hide the fifty useful messages beside it. The truncation says
        /// so; the decoder's own ceiling is what stops a hostile length.
        /// </remarks>
        private static string Clip(string text, uint maxBytes)
        {
            if (string.IsNullOrEmpty(text))
            {
                return string.Empty;
            }

            // An embedded NUL is refused by the far end's decoder, so it is
            // stripped here rather than sent to be rejected.
            if (text.IndexOf('\0') >= 0)
            {
                text = text.Replace('\0', ' ');
            }

            if (Encoding.UTF8.GetByteCount(text) <= maxBytes)
            {
                return text;
            }

            const string Ellipsis = "... (truncated)";
            int budget = (int)maxBytes - Encoding.UTF8.GetByteCount(Ellipsis);
            if (budget <= 0)
            {
                return string.Empty;
            }

            // One character at a time from the front: cutting by byte index can
            // land inside a multi-byte sequence and produce a string the far end
            // reads as mojibake rather than as a truncated sentence.
            int taken = 0;
            int bytes = 0;
            while (taken < text.Length)
            {
                int width = Encoding.UTF8.GetByteCount(text, taken, char.IsHighSurrogate(text[taken]) ? 2 : 1);
                if (bytes + width > budget)
                {
                    break;
                }

                bytes += width;
                taken += char.IsHighSurrogate(text[taken]) ? 2 : 1;
            }

            return text.Substring(0, taken) + Ellipsis;
        }

        private static void WriteEnvelope(List<byte> payload, Envelope envelope)
        {
            WriteUInt32(payload, envelope.HostGeneration);
            WriteUInt32(payload, envelope.SessionId);
            WriteUInt32(payload, envelope.RequestRevision);
        }

        private static bool ReadEnvelope(byte[] payload, out Envelope envelope, out int offset, out string error)
        {
            envelope = default(Envelope);
            offset = 0;
            error = string.Empty;
            if (payload == null || payload.Length < EnvelopeSize)
            {
                error = "The session message was shorter than its routing envelope.";
                return false;
            }

            envelope.HostGeneration = ReadUInt32At(payload, 0);
            envelope.SessionId = ReadUInt32At(payload, 4);
            envelope.RequestRevision = ReadUInt32At(payload, 8);
            offset = EnvelopeSize;

            // Generation 0 cannot be placed in time, so a result answering it
            // could never be told from one produced by a worker that has since
            // been restarted.
            if (envelope.HostGeneration == 0)
            {
                error = "The session message carried no host generation.";
                return false;
            }

            return true;
        }

        private static bool ReadUInt32(byte[] payload, ref int offset, string what, out uint value, out string error)
        {
            value = 0;
            error = string.Empty;
            if (offset + 4 > payload.Length)
            {
                error = "The message ended before " + what + ".";
                return false;
            }

            value = ReadUInt32At(payload, offset);
            offset += 4;
            return true;
        }

        private static bool ReadString(
            byte[] payload, ref int offset, uint maxBytes, string what, out string text, out string error)
        {
            text = string.Empty;
            uint length;
            if (!ReadUInt32(payload, ref offset, "the length of " + what, out length, out error))
            {
                return false;
            }

            if (length > maxBytes)
            {
                error = "The message's " + what + " claimed " + length + " bytes, over the " + maxBytes
                        + "-byte limit.";
                return false;
            }

            // Checked against what is LEFT, not against the whole payload, and in
            // long arithmetic so a 32-bit length cannot wrap the comparison.
            if ((long)offset + length > payload.Length)
            {
                error = "The message declared more " + what + " than it carried.";
                return false;
            }

            text = length == 0 ? string.Empty : Encoding.UTF8.GetString(payload, offset, (int)length);
            offset += (int)length;
            if (text.IndexOf('\0') >= 0)
            {
                error = "The message's " + what + " contained an embedded NUL.";
                return false;
            }

            return true;
        }

        /// <summary>
        /// Nothing may follow the last field a message declares.
        /// </summary>
        /// <remarks>
        /// A payload with a tail nobody reads is either a version skew the
        /// handshake was supposed to catch or a sender writing a shape this build
        /// does not have. Both are worth a refusal rather than a silent ignore.
        /// </remarks>
        private static bool AtEnd(byte[] payload, int offset, string what, out string error)
        {
            error = string.Empty;
            if (offset == payload.Length)
            {
                return true;
            }

            error = "The " + what + " carried " + (payload.Length - offset)
                    + " trailing bytes this worker does not know.";
            return false;
        }

        private static void WriteUInt32(List<byte> payload, uint value)
        {
            payload.Add((byte)(value & 0xFFu));
            payload.Add((byte)((value >> 8) & 0xFFu));
            payload.Add((byte)((value >> 16) & 0xFFu));
            payload.Add((byte)((value >> 24) & 0xFFu));
        }

        private static void WriteString(List<byte> payload, string text)
        {
            byte[] encoded = Encoding.UTF8.GetBytes(text ?? string.Empty);
            WriteUInt32(payload, (uint)encoded.Length);
            payload.AddRange(encoded);
        }

        private static uint ReadUInt32At(byte[] payload, int offset)
        {
            return (uint)payload[offset]
                   | ((uint)payload[offset + 1] << 8)
                   | ((uint)payload[offset + 2] << 16)
                   | ((uint)payload[offset + 3] << 24);
        }
    }
}
