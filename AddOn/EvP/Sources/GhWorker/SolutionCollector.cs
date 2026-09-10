// ⚠️ global::Grasshopper THROUGHOUT, AND IT IS LOAD-BEARING RATHER THAN
// FUSSY. This file is compiled into Tapioca.Grasshopper.gha as well as into the
// worker (see the .gha's .csproj: one set of files, so the two ends of the
// bridge cannot skew). That assembly declares the namespace Tapioca.Grasshopper,
// and from inside namespace Tapioca.GhWorker the compiler walks outward and
// finds Tapioca.Grasshopper before it ever considers the global one -- so a bare
// "Grasshopper.Kernel" resolves to Tapioca.Grasshopper.Kernel, which does not
// exist. The qualification says which Grasshopper is meant.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.CompilerServices;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// What one session solve produced: outputs, diagnostics and timings.
    /// </summary>
    internal sealed class SolutionCollection
    {
        internal SolutionCollection()
        {
            Outputs = new List<SessionProtocol.OutputValue>();
            Diagnostics = new List<SessionProtocol.Diagnostic>();
        }

        internal bool Ok { get; set; }

        internal SessionProtocol.FailureCode Failure { get; set; }

        internal string Message { get; set; }

        internal uint ElapsedMs { get; set; }

        internal IList<SessionProtocol.OutputValue> Outputs { get; private set; }

        internal IList<SessionProtocol.Diagnostic> Diagnostics { get; private set; }

        /// <summary>How many of the diagnostics are errors.</summary>
        internal int ErrorCount { get; set; }
    }

    /// <summary>
    /// Runs one solution on a session-owned document and collects what it made.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ ENGINE THREAD ONLY, AND ONE SOLVE AT A TIME. A Grasshopper solution
    /// must run on the thread that owns the document, and two solutions of one
    /// document at once is not a thing Grasshopper supports. Serialisation is
    /// the engine queue's job (<see cref="GhEngineThread"/>); this class assumes
    /// it and does not re-implement it.
    /// </para>
    /// <para>
    /// ⚠️ THIS IS NOT <see cref="DefinitionRunner"/> AND IT DOES NOT REPLACE IT.
    /// That one solves whatever is on the ACTIVE CANVAS and presses Tapir's
    /// execute buttons afterwards, which is the Authoring gesture. This one
    /// solves the document a session was told to load, presses nothing, and
    /// returns a result the host can address by revision. A Player that pressed
    /// write buttons would be committing without the validated semantic path
    /// HANDOFF-GHHost.md §9 requires.
    /// </para>
    /// <para>
    /// No Grasshopper type is named outside a non-inlinable method.
    /// </para>
    /// </remarks>
    internal static class SolutionCollector
    {
        private static volatile global::Grasshopper.Kernel.GH_Document _solving;

        /// <summary>
        /// The document a solution is running on, or null. Read by the cancel
        /// path, which deliberately does not go through the engine queue.
        /// </summary>
        internal static bool IsSolving
        {
            get { return _solving != null; }
        }

        /// <summary>
        /// Solves once and collects. Never throws.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static SolutionCollection Solve(global::Grasshopper.Kernel.GH_Document document, uint wants)
        {
            SolutionCollection collection = new SolutionCollection();
            collection.Message = string.Empty;

            if (document == null)
            {
                collection.Failure = SessionProtocol.FailureCode.DefinitionInvalid;
                collection.Message = "This session has no definition loaded.";
                return collection;
            }

            Stopwatch clock = Stopwatch.StartNew();
            try
            {
                _solving = document;

                // ⚠️ THE DOCUMENT'S OWN Enabled, NOT THE STATIC EnableSolutions.
                // GH_Document.EnableSolutions is process-wide: turning it off
                // around a session solve would freeze the editor's canvas too,
                // and failing to turn it back on would leave a user's canvas
                // permanently inert with nothing on screen to explain it.
                // Definition.Enabled is per document, which is the scope a
                // session actually owns. Compute sets exactly this.
                document.Enabled = true;

                // `false`: the inputs the host set were expired by the
                // assignment that set them, and expiring everything else would
                // re-run components whose data has not changed -- which is the
                // difference between a slider drag that re-solves one branch and
                // one that re-solves the definition.
                document.NewSolution(false, global::Grasshopper.Kernel.GH_SolutionMode.CommandLine);
                clock.Stop();
                collection.ElapsedMs = Clamp(clock.ElapsedMilliseconds);

                CollectDiagnostics(document, collection);
                if ((wants & (uint)SessionProtocol.SolveWants.Data) != 0)
                {
                    CollectOutputs(document, collection);
                }

                // ⚠️ ERRORS DO NOT MAKE THIS A FAILED SOLVE. A definition whose
                // third component reports an error still produced outputs from
                // the other forty, and a panel that showed nothing would be
                // hiding the evidence of what went wrong. The failure categories
                // are for solves that did not COMPLETE; a completed solution
                // with errors is a result carrying diagnostics, and the host
                // decides what to do about it.
                collection.Ok = true;
                collection.Failure = SessionProtocol.FailureCode.None;
                return collection;
            }
            catch (Exception exception)
            {
                clock.Stop();
                collection.ElapsedMs = Clamp(clock.ElapsedMilliseconds);
                collection.Failure = SessionProtocol.FailureCode.SolveFailed;
                collection.Message = "The solution faulted. " + WorkerLog.Describe(exception);

                // Collected even now: the runtime messages a faulted solve left
                // behind are usually the only account of where it got to.
                CollectDiagnostics(document, collection);
                return collection;
            }
            finally
            {
                _solving = null;
            }
        }

        /// <summary>
        /// Asks Grasshopper to abort the running solution.
        /// </summary>
        /// <remarks>
        /// ⚠️ A REQUEST, NOT A GUARANTEE, AND CALLED OFF THE ENGINE THREAD ON
        /// PURPOSE. RequestAbortSolution is honoured only BETWEEN objects, so it
        /// cannot recover a component stuck in native code, in a blocking socket
        /// or in a loop. Queueing it behind the very solution it is meant to
        /// interrupt would deliver it after that solution finished, which is
        /// never useful; the guarantee remains killing the worker, and that
        /// belongs to the add-on.
        /// </remarks>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string RequestCancel()
        {
            global::Grasshopper.Kernel.GH_Document document = _solving;
            if (document == null)
            {
                return "No session solution is running.";
            }

            try
            {
                document.RequestAbortSolution();
                return "Cancellation requested. Grasshopper only honours it between components, so a component "
                       + "stuck in native code or a blocking call will not stop; closing Grasshopper is the only "
                       + "thing that always works.";
            }
            catch (Exception exception)
            {
                return "The cancellation request failed: " + WorkerLog.Describe(exception);
            }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static void CollectOutputs(global::Grasshopper.Kernel.GH_Document document, SolutionCollection collection)
        {
            string[] flat = WorkflowFacade.CollectOutputs(document);
            for (int index = 0; index + 3 < flat.Length; index += 4)
            {
                collection.Outputs.Add(
                    new SessionProtocol.OutputValue(flat[index], flat[index + 1], flat[index + 2], flat[index + 3]));
            }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static void CollectDiagnostics(global::Grasshopper.Kernel.GH_Document document, SolutionCollection collection)
        {
            collection.Diagnostics.Clear();
            collection.ErrorCount = 0;

            string[] flat = WorkflowFacade.CollectDiagnostics(document);
            for (int index = 0; index + 2 < flat.Length; index += 3)
            {
                SessionProtocol.DiagnosticLevel level = LevelOf(flat[index]);
                if (level == SessionProtocol.DiagnosticLevel.Error)
                {
                    collection.ErrorCount++;
                }

                collection.Diagnostics.Add(
                    new SessionProtocol.Diagnostic(level, flat[index + 1], flat[index + 2]));
            }
        }

        /// <summary>
        /// The level name the package emitted. An unrecognised one is a WARNING
        /// rather than an error: a level this build does not know is a version
        /// skew, and inventing an error out of it would put a red message on a
        /// panel over a naming difference.
        /// </summary>
        private static SessionProtocol.DiagnosticLevel LevelOf(string name)
        {
            if (string.Equals(name, "error", StringComparison.OrdinalIgnoreCase))
            {
                return SessionProtocol.DiagnosticLevel.Error;
            }

            if (string.Equals(name, "remark", StringComparison.OrdinalIgnoreCase))
            {
                return SessionProtocol.DiagnosticLevel.Remark;
            }

            return SessionProtocol.DiagnosticLevel.Warning;
        }

        /// <summary>
        /// Clamped rather than cast: a solve that somehow ran for fifty days must
        /// report a large number, not wrap round to a small one.
        /// </summary>
        private static uint Clamp(long elapsedMs)
        {
            if (elapsedMs <= 0)
            {
                return 0;
            }

            return elapsedMs > uint.MaxValue ? uint.MaxValue : (uint)elapsedMs;
        }
    }
}
