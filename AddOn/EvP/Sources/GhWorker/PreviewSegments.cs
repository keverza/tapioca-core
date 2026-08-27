using System;
using System.Collections.Generic;
using System.IO.MemoryMappedFiles;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// The shared-memory segments this worker has published and Archicad has not
    /// finished with yet.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ THE WORKER CREATES THE SEGMENT AND THE HOST MAPS IT READ-ONLY. The
    /// direction is the opposite of the host-to-worker geometry case, and it is
    /// what makes the lifetime rule below necessary rather than merely tidy: the
    /// producer here is the process Archicad does not trust, so Archicad copies
    /// the batch out and unmaps before it acknowledges, and only then may these
    /// bytes be reused.
    /// </para>
    /// <para>
    /// ⚠️ A SEGMENT IS RELEASED BY AN ACK, NEVER BY A TIMER AND NEVER BY THE NEXT
    /// SOLVE. Freeing it early hands the host a view over memory this process has
    /// gone on to reuse, and the corruption surfaces as a wrong-looking mesh
    /// several batches later, with nothing pointing back here. Freeing it late is
    /// the cheaper failure and is what <see cref="Ceiling"/> bounds.
    /// </para>
    /// <para>
    /// ⚠️ BOUNDED, BECAUSE A HOST THAT STOPS ACKNOWLEDGING MUST NOT TAKE THIS
    /// PROCESS DOWN WITH IT. A wedged or crashed Archicad cannot be allowed to
    /// grow this worker's address space one solve at a time; past the ceiling the
    /// OLDEST unacknowledged segment is dropped, which loses a batch the host was
    /// never going to read anyway.
    /// </para>
    /// </remarks>
    internal sealed class PreviewSegments : IDisposable
    {
        /// <summary>
        /// How many unacknowledged batches may be outstanding at once. Small on
        /// purpose: batches are acknowledged within a frame on a healthy host, so
        /// anything above a handful means the host is not answering and keeping
        /// more would only enlarge the eventual loss.
        /// </summary>
        private const int Ceiling = 4;

        private readonly object _sync = new object();

        private readonly Dictionary<ulong, MemoryMappedFile> _open = new Dictionary<ulong, MemoryMappedFile>();

        private readonly List<ulong> _order = new List<ulong>();

        private bool _disposed;

        /// <summary>
        /// Publishes one batch's bytes under <paramref name="name"/> and keeps
        /// the mapping alive until <see cref="Release"/> names it. Returns false
        /// with a reason rather than throwing: a segment that cannot be created
        /// must cost this batch's preview, never the solve that produced it.
        /// </summary>
        internal bool Publish(string name, uint epoch, uint revision, byte[] content, out string error)
        {
            error = string.Empty;
            if (content == null || content.Length == 0)
            {
                error = "A preview batch asked for a segment of no bytes.";
                return false;
            }

            MemoryMappedFile mapping;
            try
            {
                mapping = MemoryMappedFile.CreateNew(name, content.Length, MemoryMappedFileAccess.ReadWrite);
                using (MemoryMappedViewAccessor view = mapping.CreateViewAccessor(
                           0, content.Length, MemoryMappedFileAccess.Write))
                {
                    view.WriteArray(0, content, 0, content.Length);
                }
            }
            catch (Exception exception)
            {
                error = "The preview batch's shared memory \"" + name + "\" could not be created: "
                        + exception.GetType().Name + ": " + exception.Message;
                return false;
            }

            lock (_sync)
            {
                if (_disposed)
                {
                    mapping.Dispose();
                    error = "This worker is shutting down.";
                    return false;
                }

                ulong key = Key(epoch, revision);
                MemoryMappedFile previous;
                if (_open.TryGetValue(key, out previous))
                {
                    // A revision is monotonic within an epoch, so this only
                    // happens if a batch was published twice. The newer bytes
                    // win; the older mapping is the one nothing will ever ask
                    // for again.
                    previous.Dispose();
                    _order.Remove(key);
                }

                _open[key] = mapping;
                _order.Add(key);
                TrimLocked();
            }

            return true;
        }

        /// <summary>
        /// The host has copied the batch out and unmapped it. Unknown keys are
        /// ignored: an ack for a batch this worker already dropped is not an
        /// error, it is the ceiling having done its job.
        /// </summary>
        internal void Release(uint epoch, uint revision)
        {
            lock (_sync)
            {
                Drop(Key(epoch, revision));
            }
        }

        /// <summary>
        /// Everything, at once. What a DropAll, a disconnect and shutdown all
        /// mean: nothing outstanding will ever be acknowledged now.
        /// </summary>
        internal void ReleaseAll()
        {
            lock (_sync)
            {
                foreach (KeyValuePair<ulong, MemoryMappedFile> entry in _open)
                {
                    entry.Value.Dispose();
                }

                _open.Clear();
                _order.Clear();
            }
        }

        internal int OutstandingCount
        {
            get
            {
                lock (_sync)
                {
                    return _order.Count;
                }
            }
        }

        public void Dispose()
        {
            lock (_sync)
            {
                _disposed = true;
            }

            ReleaseAll();
        }

        private void TrimLocked()
        {
            while (_order.Count > Ceiling)
            {
                Drop(_order[0]);
            }
        }

        private void Drop(ulong key)
        {
            MemoryMappedFile mapping;
            if (!_open.TryGetValue(key, out mapping))
            {
                return;
            }

            mapping.Dispose();
            _open.Remove(key);
            _order.Remove(key);
        }

        private static ulong Key(uint epoch, uint revision)
        {
            return ((ulong)epoch << 32) | revision;
        }
    }
}
