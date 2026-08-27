using System.Collections.Generic;
using System.Threading;

namespace Tapioca.Grasshopper
{
    /// <summary>What a batch does to one primitive.</summary>
    internal enum PreviewChange
    {
        Added,
        Changed,
        Removed,
        Visibility,
    }

    internal sealed class PreviewDeltaEntry
    {
        internal PreviewChange Change;

        internal ulong Id;

        /// <summary>Null for Removed, which carries an id and nothing else.</summary>
        internal PreviewPrimitive Primitive;
    }

    internal sealed class PreviewBatch
    {
        internal readonly List<PreviewDeltaEntry> Entries = new List<PreviewDeltaEntry>();

        internal uint Revision;

        internal int Added;

        internal int Changed;

        internal int Removed;

        internal int Unchanged;

        internal bool IsEmpty
        {
            get { return Entries.Count == 0; }
        }

        /// <summary>
        /// The checksum PreviewEndBatch carries. A host whose own sum disagrees
        /// asks for a full resync rather than drawing a cache it cannot trust —
        /// cheap insurance against a dropped or reordered message becoming a
        /// permanently wrong viewport.
        /// </summary>
        internal ulong Checksum()
        {
            ulong hash = PreviewHash.Start();
            foreach (PreviewDeltaEntry entry in Entries)
            {
                hash = PreviewHash.UInt64(hash, entry.Id);
                hash = PreviewHash.Byte(hash, (byte)entry.Change);
            }

            return hash;
        }

        internal string Describe()
        {
            return string.Format(
                System.Globalization.CultureInfo.InvariantCulture,
                "revision {0}: {1} added, {2} changed, {3} removed, {4} unchanged",
                Revision, Added, Changed, Removed, Unchanged);
        }
    }

    /// <summary>
    /// The worker's mirror of what the host has, and the diff against it.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ THE WORKER OWNS THE DIFF. The host never asks "what do you have"; it
    /// applies what it is told. That is what makes a batch cheap — a dense
    /// definition re-solving on a slider drag changes one component's output, and
    /// the delta for that is kilobytes where the full preview is tens of
    /// megabytes.
    /// </para>
    /// <para>
    /// ⚠️ PREVIEW-OFF IS A VISIBILITY DELTA, NOT A REMOVAL. Toggling a component's
    /// preview back on must cost a byte, not a retransmission, so a primitive that
    /// goes invisible stays in the mirror with its flag cleared.
    /// </para>
    /// <para>
    /// This class deliberately names no Rhino or Grasshopper type, so the delta
    /// rules can be exercised without either.
    /// </para>
    /// </remarks>
    /// <summary>
    /// The batch counter for this whole worker.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ ONE COUNTER FOR THE PROCESS, NOT ONE PER MIRROR, AND THE REASON IS THE
    /// HOST'S RULE. <c>revision</c> is monotonic within an EPOCH — the worker
    /// generation — and Archicad refuses a batch whose revision it has already
    /// seen, because a repeated or reversed one means messages were reordered or
    /// replayed. A mirror per component with a counter per mirror gives two
    /// preview components on one canvas the same revision 1, and the second
    /// component's geometry is refused for the life of the definition. The
    /// symptom is "only one of my preview components works", which points
    /// nowhere near a counter.
    /// </para>
    /// <para>
    /// Identity is what separates the components' primitives, and it already
    /// does: <c>primitiveId</c> hashes the component guid. The revision separates
    /// BATCHES, and the worker only ever has one batch stream.
    /// </para>
    /// </remarks>
    internal static class PreviewRevision
    {
        private static int _next;

        internal static uint Next()
        {
            return (uint)Interlocked.Increment(ref _next);
        }
    }

    internal sealed class PreviewMirror
    {
        private readonly Dictionary<ulong, PreviewPrimitive> _sent = new Dictionary<ulong, PreviewPrimitive>();

        private uint _revision;

        internal int Count
        {
            get { return _sent.Count; }
        }

        internal uint Revision
        {
            get { return _revision; }
        }

        /// <summary>
        /// Diffs one capture against the mirror and ADVANCES it, so the caller is
        /// committing to send what comes back.
        /// </summary>
        /// <remarks>
        /// ⚠️ THE MIRROR ADVANCES HERE, WHICH MEANS A BATCH THAT IS NEVER SENT
        /// LEAVES THE TWO SIDES DISAGREEING. The transport must treat a failed send
        /// as a reason to <see cref="DropAll"/> and start again, not as something to
        /// retry against a mirror that has already moved on.
        /// </remarks>
        internal PreviewBatch Diff(IList<PreviewPrimitive> captured)
        {
            PreviewBatch batch = new PreviewBatch();
            _revision = PreviewRevision.Next();
            batch.Revision = _revision;

            HashSet<ulong> seen = new HashSet<ulong>();
            foreach (PreviewPrimitive primitive in captured ?? new List<PreviewPrimitive>())
            {
                seen.Add(primitive.Id);
                PreviewPrimitive previous;
                if (!_sent.TryGetValue(primitive.Id, out previous))
                {
                    batch.Entries.Add(new PreviewDeltaEntry
                    {
                        Change = PreviewChange.Added,
                        Id = primitive.Id,
                        Primitive = primitive,
                    });
                    batch.Added++;
                }
                else if (previous.ContentHash != primitive.ContentHash)
                {
                    batch.Entries.Add(new PreviewDeltaEntry
                    {
                        Change = PreviewChange.Changed,
                        Id = primitive.Id,
                        Primitive = primitive,
                    });
                    batch.Changed++;
                }
                else if (previous.Flags != primitive.Flags)
                {
                    // Flags alone: a byte, never the geometry again.
                    batch.Entries.Add(new PreviewDeltaEntry
                    {
                        Change = PreviewChange.Visibility,
                        Id = primitive.Id,
                        Primitive = primitive,
                    });
                }
                else
                {
                    batch.Unchanged++;
                }

                _sent[primitive.Id] = primitive;
            }

            List<ulong> gone = new List<ulong>();
            foreach (KeyValuePair<ulong, PreviewPrimitive> entry in _sent)
            {
                if (!seen.Contains(entry.Key))
                {
                    gone.Add(entry.Key);
                }
            }

            foreach (ulong id in gone)
            {
                batch.Entries.Add(new PreviewDeltaEntry { Change = PreviewChange.Removed, Id = id });
                batch.Removed++;
                _sent.Remove(id);
            }

            return batch;
        }

        /// <summary>
        /// Forgets everything. The correct answer after a worker restart or a
        /// failed send: the host is told PreviewDropAll and both sides start from
        /// nothing rather than from a disagreement neither can detect.
        /// </summary>
        internal void DropAll()
        {
            _sent.Clear();
        }
    }
}
