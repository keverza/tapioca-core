using System;
using System.Globalization;
using System.IO;
using System.Runtime.CompilerServices;

namespace Tapioca.GhWorker
{
    internal enum StartOutcomeKind
    {
        Started,
        RhinoMissing,
        LicenceUnavailable,
        RhinoInitFailed,
        GrasshopperFailed,
    }

    internal readonly struct StartOutcome
    {
        internal StartOutcome(StartOutcomeKind kind, string message)
        {
            Kind = kind;
            Message = message;
        }

        internal StartOutcomeKind Kind { get; }

        internal string Message { get; }
    }

    /// <summary>
    /// The one Rhino/Grasshopper session in this worker process.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Deliberately free of Rhino types. It holds the core as an
    /// <see cref="IDisposable"/> and hands every operation that names a Rhino
    /// type to <see cref="RhinoBoot"/>, whose methods are all non-inlinable. A
    /// single <c>RhinoCore</c>-typed field here would be enough to make the CLR
    /// load RhinoCommon when this class is first touched — which is before the
    /// resolver that knows where RhinoCommon lives has been installed.
    /// </para>
    /// <para>
    /// ⚠️ ONE CORE, ONE WORKER. Editor and Player share this session; neither
    /// gets a Rhino of its own, and neither may dispose it. HANDOFF §"Editor and
    /// Player share one runtime", one level out: they share a WORKER now, and
    /// the rule is otherwise unchanged.
    /// </para>
    /// </remarks>
    internal static class WorkerSession
    {
        private static IDisposable _core;
        private static bool _running;

        /// <summary>
        /// The Archicad JSON port the add-on told this worker about.
        /// </summary>
        /// <remarks>
        /// Recorded, never re-derived: only the add-on can answer which Archicad
        /// this worker belongs to, and it already has. Tapir's own default is
        /// 19723 and it has no instance discovery, so a guess is wrong the moment
        /// a second Archicad is open — and wrong quietly, by driving the other
        /// instance's model.
        /// </remarks>
        internal static uint ArchicadJsonPort { get; private set; }

        /// <summary>
        /// The port Tapir is pointed at. The same as
        /// <see cref="ArchicadJsonPort"/> unless the counting proxy is running,
        /// in which case Tapir talks to the proxy and the proxy talks to Archicad.
        /// </summary>
        internal static uint TapirPort { get; private set; }

        internal static bool IsRunning
        {
            get { return _running; }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static StartOutcome Start(uint archicadJsonPort, uint tapirPort)
        {
            ArchicadJsonPort = archicadJsonPort;
            TapirPort = tapirPort == 0 ? archicadJsonPort : tapirPort;
            try
            {
                // 1. The resolver, before anything that could pull a Rhino
                //    assembly in. This is the order the official startup
                //    sequence prescribes and the order the whole file is built
                //    around; steps 2 and 3 only work because of step 1.
                string systemDirectory;
                try
                {
                    systemDirectory = RhinoBoot.InitializeResolver(null);
                }
                catch (Exception exception)
                {
                    return new StartOutcome(
                        StartOutcomeKind.RhinoMissing,
                        "No usable Rhino 8 installation was found. Tapioca does not install or redistribute Rhino; "
                        + "install Rhino 8 (64-bit) and try again. " + WorkerLog.Describe(exception));
                }

                WorkerLog.Write("Rhino system directory: " + systemDirectory);
                if (!Directory.Exists(systemDirectory))
                {
                    return new StartOutcome(
                        StartOutcomeKind.RhinoMissing,
                        "The resolved Rhino system directory does not exist: " + systemDirectory);
                }

                // 2. One hidden core. Rhino's own window never appears: Rhino is
                //    an implementation dependency of Grasshopper here, not a
                //    product surface.
                //
                // ⚠️ THE OPENNURBS PREREQUISITE IS GONE, AND ITS ABSENCE IS A
                // FEATURE RETURNED. In process, Rhino bound to the opennurbs.dll
                // Archicad's own hidden Rhino_In/Rhino_Out add-ons had already
                // loaded, 27 service releases older, and access-violated inside
                // StartupInProcess; the only workaround was disabling Archicad's
                // 3DM import and export. Out here Rhino loads its own copy,
                // nothing has to be disabled, and Archicad keeps its 3DM support.
                try
                {
                    _core = RhinoBoot.CreateHiddenCore();
                }
                catch (Exception exception)
                {
                    string text = WorkerLog.Describe(exception);
                    bool licence = text.IndexOf("licen", StringComparison.OrdinalIgnoreCase) >= 0;
                    return new StartOutcome(
                        licence ? StartOutcomeKind.LicenceUnavailable : StartOutcomeKind.RhinoInitFailed,
                        licence
                            ? "Rhino is installed but no licence is available for it. " + text
                            : "Rhino would not start in the Tapioca worker process. " + text);
                }

                WorkerLog.Write("RhinoCore constructed (hidden).");

                // 3. Only now Grasshopper, which depends on the core existing.
                string tapirReport;
                string failure;
                if (!RhinoBoot.LoadGrasshopper(ArchicadJsonPort, TapirPort, out tapirReport, out failure))
                {
                    // The core stays up: it started cleanly, and tearing it down
                    // here would destroy the evidence of what stopped Grasshopper.
                    _running = true;
                    return new StartOutcome(
                        StartOutcomeKind.GrasshopperFailed,
                        "Rhino started but Grasshopper did not load. " + failure);
                }

                WorkerLog.Write("Grasshopper loaded.");
                _running = true;
                return new StartOutcome(
                    StartOutcomeKind.Started,
                    "Rhino " + RhinoBoot.DescribeVersion() + " hosted from " + systemDirectory
                    + ", Grasshopper loaded."
                    + (ArchicadJsonPort != 0
                        ? " Archicad JSON port " + ArchicadJsonPort.ToString(CultureInfo.InvariantCulture) + "."
                        : " Archicad JSON port unavailable.")
                    + (string.IsNullOrWhiteSpace(tapirReport) ? string.Empty : " " + tapirReport.Trim()));
            }
            catch (Exception exception)
            {
                return new StartOutcome(StartOutcomeKind.RhinoInitFailed, WorkerLog.Describe(exception));
            }
        }

        /// <summary>
        /// Shows or hides the Grasshopper canvas. Must be called on the UI
        /// thread; <see cref="Program"/> marshals for the bridge.
        /// </summary>
        /// <remarks>
        /// ⚠️ SHOWING AND HIDING THE CANVAS NEVER TOUCHES THE CORE. The window's
        /// lifetime and the runtime's are deliberately unrelated: closing the
        /// canvas must not dispose Rhino, and re-opening it must not construct a
        /// second one.
        /// </remarks>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static bool SetEditorVisible(bool visible, out string failure)
        {
            failure = string.Empty;
            if (!_running)
            {
                failure = "Rhino is not running in this worker, so there is no Grasshopper editor to show.";
                return false;
            }

            return RhinoBoot.SetEditorVisible(visible, out failure);
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string Stop()
        {
            if (!_running && _core == null)
            {
                return "Nothing to stop.";
            }

            _running = false;
            string report;
            try
            {
                if (_core != null)
                {
                    _core.Dispose();
                    report = "RhinoCore disposed.";
                }
                else
                {
                    report = "No RhinoCore to dispose.";
                }
            }
            catch (Exception exception)
            {
                // A core that will not dispose is worth recording and is NOT
                // worth failing over: this process is about to end anyway, and
                // the add-on's TerminateProcess is the guarantee behind it.
                report = "RhinoCore did not dispose cleanly: " + WorkerLog.Describe(exception);
            }
            finally
            {
                _core = null;
            }

            return report;
        }
    }
}
