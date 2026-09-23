using System;
using System.Collections.Concurrent;
using Grasshopper2.Doc;

namespace TapiocaGH2
{
    /// <summary>
    /// Transient input for the first headless solve. No document parameter or
    /// persistent component field changes when an override is installed.
    /// </summary>
    public static class PlayerOverride
    {
        private sealed class Snapshot
        {
            internal Snapshot(string exposureId, double value)
            {
                ExposureId = exposureId;
                Value = value;
            }

            internal string ExposureId { get; }
            internal double Value { get; }
        }

        private static readonly ConcurrentDictionary<Document, Snapshot> Active = new();

        public static IDisposable Install(Document document, string exposureId, double value)
        {
            ArgumentNullException.ThrowIfNull(document);
            ArgumentException.ThrowIfNullOrWhiteSpace(exposureId);
            if (!double.IsFinite(value))
                throw new ArgumentOutOfRangeException(nameof(value));

            if (!Active.TryAdd(document, new Snapshot(exposureId, value)))
                throw new InvalidOperationException("A solution override is already active for this document.");

            return new Lease(document);
        }

        internal static bool TryGet(Document document, string exposureId, out double value)
        {
            if (Active.TryGetValue(document, out Snapshot? snapshot) && snapshot.ExposureId == exposureId)
            {
                value = snapshot.Value;
                return true;
            }

            value = default;
            return false;
        }

        private sealed class Lease : IDisposable
        {
            private Document? document;

            internal Lease(Document document) => this.document = document;

            public void Dispose()
            {
                Document? released = System.Threading.Interlocked.Exchange(ref document, null);
                if (released is not null)
                    Active.TryRemove(released, out _);
            }
        }
    }
}
