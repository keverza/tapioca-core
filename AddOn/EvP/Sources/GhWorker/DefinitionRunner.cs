using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.CompilerServices;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// Solves the definition on the canvas once, deliberately, and reports what
    /// happened (PLAT-GH-PLAYER, slice 2).
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ ONE PRESS PRODUCES ONE SOLUTION, AND THAT IS WHAT THIS CLASS IS FOR.
    /// HANDOFF-GrasshopperInsideArchicad.md, "Reuse Tapioca's run shell": "Player
    /// runs must disable or constrain automatic execution so one press produces
    /// one controlled solution and write components do not re-fire unexpectedly."
    /// <see cref="Run"/> delivers the first half — one deliberate solution, on
    /// demand, reported. It does NOT yet deliver the second: Grasshopper still
    /// re-solves on its own when the user edits the canvas, which is correct
    /// while the editor is the only way anyone reaches this and would be wrong
    /// the moment a write definition is admitted. Constraining that is slice 4's,
    /// where it is paid for by the execution gating the handoff requires. Do not
    /// claim it is done here.
    /// </para>
    /// <para>
    /// ⚠️ THIS DOES NOT MAKE A RUN READ-ONLY, AND NOTHING HERE CLAIMS IT DOES.
    /// The bridge refuses Tapioca write commands (GhBridge.cpp), but Tapir talks
    /// to Archicad over its own loopback HTTP, which Tapioca neither owns nor
    /// intercepts — that is Tapir's supported architecture. So a definition
    /// holding Tapir write components CAN change the project, and killing the
    /// worker afterwards cannot un-commit what it wrote. The run report says so
    /// rather than implying a safety that is not there. Slice 4 is where writes
    /// are actually gated, after execution gating proves one Run is one executor
    /// pass.
    /// </para>
    /// <para>
    /// ⚠️ MAIN/UI THREAD ONLY. A Grasshopper solution must run on the thread that
    /// owns the document; <see cref="Program"/> marshals every request there.
    /// </para>
    /// <para>
    /// No Rhino or Grasshopper type is named outside a non-inlinable method, for
    /// the reason spelled out in <see cref="RhinoBoot"/>.
    /// </para>
    /// </remarks>
    internal static class DefinitionRunner
    {
        /// <summary>How many component messages a report will carry.</summary>
        private const int MaxReportedMessages = 12;

        private static bool _running;

        internal static bool IsRunning
        {
            get { return _running; }
        }

        /// <summary>
        /// Solves the active definition once. Returns a JSON report; never
        /// throws.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static RunReport Run()
        {
            if (!WorkerSession.IsRunning)
            {
                return RunReport.Failed("Grasshopper is not running in this worker yet.", 0);
            }

            if (_running)
            {
                // Refused rather than queued: two solutions of one document at
                // once is not a thing Grasshopper supports, and a queue would
                // turn one impatient double-click into two write passes.
                return RunReport.Failed("A Grasshopper solution is already running.", 0);
            }

            _running = true;
            Stopwatch clock = Stopwatch.StartNew();
            try
            {
                return Solve(clock);
            }
            catch (Exception exception)
            {
                return RunReport.Failed(
                    "The solution faulted. " + WorkerLog.Describe(exception), clock.ElapsedMilliseconds);
            }
            finally
            {
                _running = false;
            }
        }

        /// <summary>
        /// Asks Grasshopper to abort the running solution.
        /// </summary>
        /// <remarks>
        /// ⚠️ A REQUEST, NOT A GUARANTEE, AND THE CALLER MUST BE TOLD WHICH.
        /// RequestAbortSolution is only honoured BETWEEN objects, so it cannot
        /// recover a component stuck in native code, in a blocking socket or in a
        /// loop — which is most of the ways a real definition hangs. The
        /// guarantee is killing this process, and it belongs to the add-on.
        /// </remarks>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string RequestCancel()
        {
            if (!_running)
            {
                return "No Grasshopper solution is running.";
            }

            try
            {
                Grasshopper.Kernel.GH_Document document = ActiveDocument();
                if (document == null)
                {
                    return "There is no Grasshopper document to cancel.";
                }

                document.RequestAbortSolution();
                return "Cancellation requested. Grasshopper only honours it between components, so a component "
                       + "stuck in native code or a blocking call will not stop; close Grasshopper to stop it "
                       + "for certain.";
            }
            catch (Exception exception)
            {
                return "The cancellation request failed: " + WorkerLog.Describe(exception);
            }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static Grasshopper.Kernel.GH_Document ActiveDocument()
        {
            return Grasshopper.Instances.ActiveCanvas == null ? null : Grasshopper.Instances.ActiveCanvas.Document;
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static RunReport Solve(Stopwatch clock)
        {
            Grasshopper.Kernel.GH_Document document = ActiveDocument();
            if (document == null)
            {
                return RunReport.Failed(
                    "There is no definition open on the Grasshopper canvas. Open a .gh or .ghx file in the "
                    + "Grasshopper editor first.",
                    clock.ElapsedMilliseconds);
            }

            string name = string.IsNullOrWhiteSpace(document.DisplayName)
                ? "(unsaved definition)"
                : document.DisplayName;

            // ⚠️ GH_Document.EnableSolutions IS THE GATE, AND IT IS RESTORED IN A
            // finally FOR A REASON. It is a static: turning it off and failing to
            // turn it back on leaves the user's canvas permanently inert, with no
            // error and nothing on screen to explain it — a far worse bug than
            // the one being prevented. It is forced ON here only so that a canvas
            // the user had paused still answers Run.
            //
            // What this deliberately does NOT do is hold it off around the run to
            // stop Grasshopper re-solving afterwards. That belongs with slice 4,
            // where writes are admitted and "one Run is one executor pass" has to
            // be proved; doing it now would freeze the canvas of a user who is
            // sitting in the editor editing, which is the only way anyone uses
            // this today.
            bool previouslyEnabled = Grasshopper.Kernel.GH_Document.EnableSolutions;
            try
            {
                Grasshopper.Kernel.GH_Document.EnableSolutions = true;
                document.NewSolution(true);
            }
            finally
            {
                Grasshopper.Kernel.GH_Document.EnableSolutions = previouslyEnabled;
            }

            // ⚠️ AFTER THE SOLVE, AND INSIDE THE TIMED REGION ON PURPOSE. Tapir's
            // writes fire from a capsule button rather than from SolveInstance, so
            // a definition that creates elements creates nothing until this runs —
            // measured twice on 2026-08-27, a CreateLineElements definition solving
            // in 9 and 16 ms and writing nothing. The press is part of what a Run
            // costs, so it is part of what a Run reports.
            string executed = TapirExecutor.PressExecuteButtons(document);

            clock.Stop();

            List<string> errors = new List<string>();
            List<string> warnings = new List<string>();
            Collect(document, errors, warnings);

            bool ok = errors.Count == 0;
            string headline = ok
                ? "Solved " + name + "."
                : "Solved " + name + " with " + errors.Count.ToString(CultureInfo.InvariantCulture) + " error(s).";
            if (!string.IsNullOrEmpty(executed))
            {
                headline += Environment.NewLine + executed;
            }

            return new RunReport(ok, headline, clock.ElapsedMilliseconds, Clip(errors), Clip(warnings));
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static void Collect(
            Grasshopper.Kernel.GH_Document document,
            List<string> errors,
            List<string> warnings)
        {
            foreach (Grasshopper.Kernel.IGH_DocumentObject candidate in document.Objects)
            {
                Grasshopper.Kernel.IGH_ActiveObject active = candidate as Grasshopper.Kernel.IGH_ActiveObject;
                if (active == null)
                {
                    continue;
                }

                Append(errors, active, Grasshopper.Kernel.GH_RuntimeMessageLevel.Error);
                Append(warnings, active, Grasshopper.Kernel.GH_RuntimeMessageLevel.Warning);
            }
        }

        /// <summary>
        /// Caps a message list, replacing the tail with a count.
        /// </summary>
        /// <remarks>
        /// A definition that is broken in one way is usually broken in it a
        /// hundred times, and a dialog listing a hundred identical errors tells a
        /// user less than one listing twelve and a count.
        /// </remarks>
        private static List<string> Clip(List<string> messages)
        {
            if (messages.Count <= MaxReportedMessages)
            {
                return messages;
            }

            List<string> clipped = messages.GetRange(0, MaxReportedMessages);
            clipped.Add(
                "... and " + (messages.Count - MaxReportedMessages).ToString(CultureInfo.InvariantCulture) + " more");
            return clipped;
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static void Append(
            List<string> into,
            Grasshopper.Kernel.IGH_ActiveObject active,
            Grasshopper.Kernel.GH_RuntimeMessageLevel level)
        {
            foreach (string message in active.RuntimeMessages(level))
            {
                // The component's own name, always: a bare "1. Solution exception"
                // in a definition with forty components is not actionable, and
                // the name is the only thing that makes it so.
                into.Add(active.NickName + ": " + message);
            }
        }

    }
}
