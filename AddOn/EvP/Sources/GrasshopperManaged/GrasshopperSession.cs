using System;
using System.Globalization;
using System.IO;
using System.Runtime.CompilerServices;

namespace Tapioca.GrasshopperHost
{
    /// <summary>Mirrors evp::grasshopper::HostState and TapiocaGhState.</summary>
    public enum HostState
    {
        NotStarted = 0,
        Starting = 1,
        Running = 2,
        Stopping = 3,
        Stopped = 4,
        Failed = 5,
    }

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
    /// The one Rhino/Grasshopper session in the process: the managed mirror of
    /// evp::grasshopper::HostLifecycle, keeping its own state so the two sides
    /// can be COMPARED rather than assumed to agree.
    /// </summary>
    /// <remarks>
    /// Deliberately free of Rhino types. It holds the core as an
    /// <see cref="IDisposable"/> and hands every operation that names a Rhino
    /// type to <see cref="RhinoBoot"/>, whose methods are all non-inlinable.
    /// A single <c>RhinoCore</c>-typed field here would be enough to make the
    /// CLR load RhinoCommon when this class is first touched — which is before
    /// the resolver that knows where RhinoCommon lives has been installed.
    /// </remarks>
    internal static class GrasshopperSession
    {
        private static IDisposable _core;
        private static HostState _state = HostState.NotStarted;

        /// <summary>
        /// The Archicad JSON port this process listens on, handed over by the
        /// native side at start.
        /// </summary>
        /// <remarks>
        /// Kept here so the Tapir step of slice 1 has one place to read it from,
        /// and so nothing on this side is ever tempted to guess it. Tapir's own
        /// default is 19723 and it has no instance discovery, so a guess is
        /// wrong the moment a second Archicad is open — and wrong quietly, by
        /// driving the other instance's model.
        /// </remarks>
        internal static uint ArchicadJsonPort { get; set; }

        internal static HostState State
        {
            get { return _state; }
        }

        internal static StartOutcome Start(string rhinoSystemDirectory, bool loadGrasshopper, bool showEditor)
        {
            _state = HostState.Starting;
            try
            {
                // 1. The resolver, before anything that could pull a Rhino
                //    assembly in. This is the order the official startup
                //    sequence prescribes and the order the whole file is built
                //    around; steps 2 and 3 only work because of step 1.
                string systemDirectory;
                try
                {
                    systemDirectory = RhinoBoot.InitializeResolver(rhinoSystemDirectory);
                }
                catch (Exception exception)
                {
                    _state = HostState.Failed;
                    return new StartOutcome(
                        StartOutcomeKind.RhinoMissing,
                        "No usable Rhino 8 installation was found. Tapioca does not install or redistribute Rhino; "
                        + "install Rhino 8 (64-bit) and try again. " + Flatten(exception));
                }

                Log.Write("Rhino system directory: " + systemDirectory);
                if (!Directory.Exists(systemDirectory))
                {
                    _state = HostState.Failed;
                    return new StartOutcome(
                        StartOutcomeKind.RhinoMissing,
                        "The resolved Rhino system directory does not exist: " + systemDirectory);
                }

                // 2. One hidden core. Rhino's own window never appears: Rhino is
                //    an implementation dependency of Grasshopper here, not a
                //    product surface.
                try
                {
                    _core = RhinoBoot.CreateHiddenCore();
                }
                catch (Exception exception)
                {
                    _state = HostState.Failed;
                    string text = Flatten(exception);
                    bool licence = text.IndexOf("licen", StringComparison.OrdinalIgnoreCase) >= 0;
                    return new StartOutcome(
                        licence ? StartOutcomeKind.LicenceUnavailable : StartOutcomeKind.RhinoInitFailed,
                        licence
                            ? "Rhino is installed but no licence is available for it in this process. " + text
                            : "Rhino would not start inside Archicad. " + text);
                }

                Log.Write("RhinoCore constructed (hidden).");

                // 3. Only now Grasshopper, which depends on the core existing.
                if (loadGrasshopper)
                {
                    string failure;
                    if (!RhinoBoot.LoadGrasshopper(showEditor, out failure))
                    {
                        // The core stays up: it started cleanly, and tearing it
                        // down here would destroy the evidence of what stopped
                        // Grasshopper. State stays Running so Stop still works.
                        _state = HostState.Running;
                        return new StartOutcome(
                            StartOutcomeKind.GrasshopperFailed,
                            "Rhino started but Grasshopper did not load. " + failure);
                    }

                    Log.Write("Grasshopper loaded.");
                }

                _state = HostState.Running;
                return new StartOutcome(
                    StartOutcomeKind.Started,
                    "Rhino " + RhinoBoot.DescribeVersion() + " hosted from " + systemDirectory
                    + (loadGrasshopper ? ", Grasshopper loaded." : ", Grasshopper not requested.")
                    + (ArchicadJsonPort != 0
                        ? " Archicad JSON port " + ArchicadJsonPort.ToString(CultureInfo.InvariantCulture) + "."
                        : " Archicad JSON port unavailable."));
            }
            catch (Exception exception)
            {
                _state = HostState.Failed;
                return new StartOutcome(StartOutcomeKind.RhinoInitFailed, Flatten(exception));
            }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string Stop()
        {
            _state = HostState.Stopping;
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
                // worth failing over: the caller is on Archicad's quit path and
                // the only thing that matters from here is that nothing of ours
                // is left pointing into the add-on.
                report = "RhinoCore did not dispose cleanly: " + Flatten(exception);
            }
            finally
            {
                _core = null;
                _state = HostState.Stopped;
            }

            Log.Write(report);
            Log.Close();
            return report;
        }

        private static string Flatten(Exception exception)
        {
            string text = exception.GetType().Name + ": " + exception.Message;
            Exception inner = exception.InnerException;
            while (inner != null)
            {
                text += " -> " + inner.GetType().Name + ": " + inner.Message;
                inner = inner.InnerException;
            }

            return text;
        }
    }
}
