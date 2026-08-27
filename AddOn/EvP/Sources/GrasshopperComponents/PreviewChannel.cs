using System;
using System.Collections.Generic;
using System.Text;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Frames a captured batch for the bridge. The GHA's copy of the wire format
    /// in <c>Sources/AddOn/Grasshopper/GhPreviewProtocol.hpp</c>.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ A COPY, DELIBERATELY, AND NEITHER HALF INFERS THE OTHER'S. Exactly the
    /// rule <c>PreviewPrimitives.cs</c>'s header states and
    /// <c>BridgeProtocol.cs</c> keeps for the control protocol: the two ends
    /// are a C++ <c>.apx</c> and a C# <c>.gha</c>, there is no shared header they
    /// could both include and no compiler they both trust to lay out a struct the
    /// same way. So the format is written out twice, by hand, little-endian, and
    /// the HANDSHAKE compares version numbers rather than assuming they match.
    /// </para>
    /// <para>
    /// ⚠️ CHANGING ANYTHING HERE MEANS CHANGING GhPreviewProtocol.hpp IN THE SAME
    /// EDIT AND BUMPING BridgeProtocol.Version IN BOTH. Renumbering one side is
    /// not a build error; it is a silent corruption that draws the wrong shape.
    /// </para>
    /// <para>
    /// ⚠️ GEOMETRY IS NARROWED TO float32 HERE, AND ONLY HERE. Identity and the
    /// content hash are computed on the original doubles, so change detection
    /// keeps full precision; the WIRE carries floats because everything on this
    /// path is drawn and never measured. PREVIEW GEOMETRY IS NOT BIM GEOMETRY —
    /// nothing downstream may treat a preview float as a coordinate an element
    /// could be built from.
    /// </para>
    /// <para>
    /// ⚠️ WHAT THIS DOES NOT DO YET. It frames; it does not send. The pipe write
    /// and the shared-memory segment are P2 of the preview work, and
    /// <see cref="EncodeBatch"/> deliberately returns the frames rather than
    /// pushing them, so the format is exercised and byte-pinned before a
    /// transport exists to hide a mistake in it.
    /// </para>
    /// </remarks>
    internal static class PreviewChannel
    {
        /// <summary>Mirrors PreviewHeaderSize in GhPreviewProtocol.hpp.</summary>
        internal const int PrimitiveHeaderSize = 80;

        /// <summary>Mirrors PreviewDescriptorSize.</summary>
        internal const int DescriptorSize = 20;

        /// <summary>
        /// Bulk over this many bytes goes to shared memory rather than the pipe.
        /// A preview number, not a protocol constant: the pipe's budget is
        /// sub-millisecond for control and small primitives, and a mesh is
        /// neither.
        /// </summary>
        internal const int InlineArrayLimit = 8 * 1024;

        /// <summary>Mirrors MaxPreviewInlinePayloadBytes.</summary>
        internal const int MaxInlinePayloadBytes = 1024 * 1024;

        /// <summary>Message types, mirroring BridgeProtocol.MessageType.</summary>
        internal enum PreviewMessage : uint
        {
            BeginBatch = 14,
            Added = 15,
            Changed = 16,
            Removed = 17,
            Visibility = 18,
            Selection = 19,
            EndBatch = 20,
            DropAll = 21,
        }

        /// <summary>One framed message, ready for the bridge to write.</summary>
        internal sealed class PreviewFrame
        {
            internal PreviewMessage Message;

            internal byte[] Payload;

            /// <summary>
            /// True when this primitive's arrays are in the batch's segment
            /// rather than in <see cref="Payload"/>. The bytes themselves live
            /// once, in <see cref="PreviewWireBatch.Segment"/>, and the header's
            /// offset is what finds them; a second copy per frame would double
            /// the memory the volume case is trying to keep off the pipe.
            /// </summary>
            internal bool InSegment;
        }

        /// <summary>
        /// What one solve produces on the wire: the frames, and the segment they
        /// reference.
        /// </summary>
        internal sealed class PreviewWireBatch
        {
            internal readonly List<PreviewFrame> Frames = new List<PreviewFrame>();

            internal byte[] Segment = new byte[0];

            internal uint Epoch;

            internal uint Revision;

            internal string SegmentName = string.Empty;
        }

        /// <summary>
        /// Frames a diffed batch. The BeginBatch frame is emitted last-minute
        /// with the segment size the primitives turned out to need, which is why
        /// the frames are assembled before it is prepended.
        /// </summary>
        /// <remarks>
        /// ⚠️ ONE SEGMENT PER BATCH, CREATED BY THE WORKER AND MAPPED READ-ONLY
        /// BY THE HOST. The producer is the untrusted side on this path, so the
        /// host copies out before acknowledging and never holds a view across
        /// frames. The worker keeps the segment alive until that ack arrives.
        /// </remarks>
        internal static PreviewWireBatch EncodeBatch(PreviewBatch batch, uint epoch)
        {
            PreviewWireBatch wire = new PreviewWireBatch();
            if (batch == null)
            {
                return wire;
            }

            wire.Epoch = epoch;
            wire.Revision = batch.Revision;
            wire.SegmentName = "Tapioca.GhPreview." + epoch + "." + batch.Revision;

            List<byte> segment = new List<byte>();
            List<ulong> removed = new List<ulong>();

            foreach (PreviewDeltaEntry entry in batch.Entries)
            {
                if (entry.Change == PreviewChange.Removed)
                {
                    // Id only. A removal never carries geometry, and batching
                    // them into one run is what keeps a definition that dropped a
                    // thousand primitives from costing a thousand messages.
                    removed.Add(entry.Id);
                    continue;
                }

                if (entry.Change == PreviewChange.Visibility)
                {
                    // ⚠️ A BYTE, NOT A RETRANSMISSION. Toggling a component's
                    // preview off must not resend what it was drawing, or
                    // toggling it back on costs the whole thing again.
                    wire.Frames.Add(new PreviewFrame
                    {
                        Message = PreviewMessage.Visibility,
                        Payload = EncodeFlagRun(
                            epoch,
                            batch.Revision,
                            (byte)(entry.Primitive.Flags & PreviewFlags.Visible),
                            (byte)PreviewFlags.Visible,
                            new List<ulong> { entry.Id }),
                    });
                    continue;
                }

                wire.Frames.Add(EncodePrimitive(entry, batch.Revision, segment));
            }

            if (removed.Count > 0)
            {
                wire.Frames.Add(new PreviewFrame
                {
                    Message = PreviewMessage.Removed,
                    Payload = EncodeIdRun(epoch, batch.Revision, removed),
                });
            }

            wire.Segment = segment.ToArray();

            PreviewFrame begin = new PreviewFrame
            {
                Message = PreviewMessage.BeginBatch,
                Payload = EncodeBeginBatch(
                    epoch,
                    batch.Revision,
                    (uint)batch.Entries.Count,
                    (uint)wire.Segment.Length,
                    wire.Segment.Length > 0 ? wire.SegmentName : string.Empty),
            };
            wire.Frames.Insert(0, begin);

            wire.Frames.Add(new PreviewFrame
            {
                Message = PreviewMessage.EndBatch,
                Payload = EncodeEndBatch(epoch, batch.Revision, (uint)batch.Entries.Count, batch.Checksum()),
            });

            return wire;
        }

        internal static byte[] EncodeBeginBatch(
            uint epoch, uint revision, uint primitiveCount, uint segmentBytes, string segmentName)
        {
            byte[] name = Encoding.UTF8.GetBytes(segmentName ?? string.Empty);
            List<byte> payload = new List<byte>(20 + name.Length);
            WriteUInt32(payload, epoch);
            WriteUInt32(payload, revision);
            WriteUInt32(payload, primitiveCount);
            WriteUInt32(payload, segmentBytes);
            WriteUInt32(payload, (uint)name.Length);
            payload.AddRange(name);
            return payload.ToArray();
        }

        internal static byte[] EncodeEndBatch(uint epoch, uint revision, uint entryCount, ulong checksum)
        {
            List<byte> payload = new List<byte>(20);
            WriteUInt32(payload, epoch);
            WriteUInt32(payload, revision);
            WriteUInt32(payload, entryCount);
            WriteUInt64(payload, checksum);
            return payload.ToArray();
        }

        internal static byte[] EncodeIdRun(uint epoch, uint revision, IList<ulong> ids)
        {
            List<byte> payload = new List<byte>(12 + ids.Count * 8);
            WriteUInt32(payload, epoch);
            WriteUInt32(payload, revision);
            WriteUInt32(payload, (uint)ids.Count);
            foreach (ulong id in ids)
            {
                WriteUInt64(payload, id);
            }

            return payload.ToArray();
        }

        internal static byte[] EncodeFlagRun(
            uint epoch, uint revision, byte flagValue, byte flagMask, IList<ulong> ids)
        {
            List<byte> payload = new List<byte>(16 + ids.Count * 8);
            WriteUInt32(payload, epoch);
            WriteUInt32(payload, revision);
            WriteUInt32(payload, (uint)ids.Count);
            payload.Add(flagValue);
            payload.Add(flagMask);
            payload.Add(0);
            payload.Add(0);
            foreach (ulong id in ids)
            {
                WriteUInt64(payload, id);
            }

            return payload.ToArray();
        }

        internal static byte[] EncodeDropAll(uint epoch, string reason)
        {
            byte[] text = Encoding.UTF8.GetBytes(reason ?? string.Empty);
            List<byte> payload = new List<byte>(8 + text.Length);
            WriteUInt32(payload, epoch);
            WriteUInt32(payload, (uint)text.Length);
            payload.AddRange(text);
            return payload.ToArray();
        }

        /// <summary>
        /// Header, descriptor, and the arrays — inline when they are small,
        /// appended to <paramref name="segment"/> otherwise.
        /// </summary>
        private static PreviewFrame EncodePrimitive(PreviewDeltaEntry entry, uint revision, List<byte> segment)
        {
            PreviewPrimitive primitive = entry.Primitive;

            byte[] text = string.IsNullOrEmpty(primitive.Text)
                ? new byte[0]
                : Encoding.UTF8.GetBytes(primitive.Text);

            int positionFloats = primitive.Positions == null ? 0 : primitive.Positions.Length;
            int normalFloats = primitive.Normals == null ? 0 : primitive.Normals.Length;
            int indexCount = primitive.Indices == null ? 0 : primitive.Indices.Length;
            int arrayBytes = positionFloats * 4 + normalFloats * 4 + indexCount * 4 + text.Length;

            List<byte> arrays = new List<byte>(arrayBytes);
            for (int index = 0; index < positionFloats; index++)
            {
                WriteFloat(arrays, primitive.Positions[index]);
            }

            for (int index = 0; index < normalFloats; index++)
            {
                WriteFloat(arrays, primitive.Normals[index]);
            }

            for (int index = 0; index < indexCount; index++)
            {
                WriteUInt32(arrays, unchecked((uint)primitive.Indices[index]));
            }

            arrays.AddRange(text);

            bool inSegment = arrays.Count > InlineArrayLimit;
            ulong segmentOffset = 0;
            if (inSegment)
            {
                segmentOffset = (ulong)segment.Count;
                segment.AddRange(arrays);
            }

            List<byte> payload = new List<byte>(PrimitiveHeaderSize + DescriptorSize + (inSegment ? 0 : arrays.Count));

            // ---- header, 80 bytes ------------------------------------------
            WriteUInt64(payload, primitive.Id);
            payload.Add((byte)primitive.Kind);
            payload.Add((byte)primitive.Flags);
            payload.Add(0); // reserved
            payload.Add(0);
            WriteUInt32(payload, primitive.ItemIndex);
            payload.AddRange(primitive.ComponentGuid.ToByteArray());
            payload.AddRange(primitive.ParameterGuid.ToByteArray());
            WriteUInt32(payload, primitive.BranchHash);
            WriteUInt64(payload, primitive.ContentHash);
            WriteUInt32(payload, revision);
            WriteUInt32(payload, (uint)(inSegment ? DescriptorSize : DescriptorSize + arrays.Count));
            WriteUInt64(payload, segmentOffset);
            WriteUInt32(payload, (uint)(inSegment ? arrays.Count : 0));

            // ---- descriptor, 20 bytes --------------------------------------
            WriteUInt32(payload, (uint)positionFloats);
            WriteUInt32(payload, (uint)normalFloats);
            WriteUInt32(payload, (uint)indexCount);
            WriteUInt32(payload, (uint)text.Length);
            payload.Add(primitive.Closed ? (byte)1 : (byte)0);
            payload.Add(0);
            payload.Add(0);
            payload.Add(0);

            if (!inSegment)
            {
                payload.AddRange(arrays);
            }

            return new PreviewFrame
            {
                Message = entry.Change == PreviewChange.Added ? PreviewMessage.Added : PreviewMessage.Changed,
                Payload = payload.ToArray(),
                InSegment = inSegment,
            };
        }

        private static void WriteUInt32(List<byte> buffer, uint value)
        {
            buffer.Add((byte)(value & 0xFF));
            buffer.Add((byte)((value >> 8) & 0xFF));
            buffer.Add((byte)((value >> 16) & 0xFF));
            buffer.Add((byte)((value >> 24) & 0xFF));
        }

        private static void WriteUInt64(List<byte> buffer, ulong value)
        {
            WriteUInt32(buffer, (uint)(value & 0xFFFFFFFFUL));
            WriteUInt32(buffer, (uint)((value >> 32) & 0xFFFFFFFFUL));
        }

        /// <summary>
        /// ⚠️ THE ONE NARROWING IN THE WHOLE PIPELINE, AND IT HAPPENS AFTER THE
        /// CONTENT HASH. Hashing the float would make an edit below float
        /// precision invisible to the delta; hashing the double and sending the
        /// float means the host draws what it can draw and the worker still knows
        /// exactly what changed.
        /// </summary>
        private static void WriteFloat(List<byte> buffer, double value)
        {
            byte[] bytes = BitConverter.GetBytes((float)value);
            if (!BitConverter.IsLittleEndian)
            {
                Array.Reverse(bytes);
            }

            buffer.AddRange(bytes);
        }
    }
}
