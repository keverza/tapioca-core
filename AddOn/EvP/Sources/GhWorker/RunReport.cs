using System.Collections.Generic;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// What one Run produced. Mirrors <c>protocol::RunReportPayload</c> in
    /// Sources/AddOn/Grasshopper/GhProtocol.hpp.
    /// </summary>
    /// <remarks>
    /// ⚠️ <see cref="Ok"/> MEANS "NO COMPONENT REPORTED AN ERROR". It does NOT
    /// mean the project was left unchanged, and no layer in this worker is in a
    /// position to promise that: Tapir write components reach Archicad over
    /// Tapir's own loopback HTTP, which Tapioca neither owns nor intercepts. See
    /// <see cref="DefinitionRunner"/>.
    /// </remarks>
    internal sealed class RunReport
    {
        internal RunReport(bool ok, string headline, long elapsedMs, List<string> errors, List<string> warnings)
        {
            Ok = ok;
            Headline = headline ?? string.Empty;
            ElapsedMs = elapsedMs;
            Errors = errors ?? new List<string>();
            Warnings = warnings ?? new List<string>();
            Ledger = new List<KeyValuePair<string, uint>>();
        }

        internal bool Ok { get; private set; }

        internal string Headline { get; private set; }

        internal long ElapsedMs { get; private set; }

        internal List<string> Errors { get; private set; }

        internal List<string> Warnings { get; private set; }

        /// <summary>
        /// What the run called through the Tapir proxy, and how often. Empty when
        /// the proxy is off, which is the default. The HOST classifies and judges
        /// it — see Sources/AddOn/Grasshopper/GhUndoBudget.hpp — because that is
        /// where the offline tests are; the worker only counts.
        /// </summary>
        internal List<KeyValuePair<string, uint>> Ledger { get; set; }

        /// <summary>
        /// A run that did not get as far as a solution. It still carries an
        /// elapsed time, because "refused after 0 ms" and "faulted after 40 s"
        /// are different failures.
        /// </summary>
        internal static RunReport Failed(string headline, long elapsedMs)
        {
            return new RunReport(false, headline, elapsedMs, null, null);
        }
    }
}
