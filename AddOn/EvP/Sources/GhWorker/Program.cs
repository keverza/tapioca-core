using System;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Windows.Forms;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// Tapioca.GhWorker.exe — the process Grasshopper actually solves in.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ THIS PROCESS IS EXPENDABLE, AND EVERY DESIGN CHOICE BELOW FOLLOWS FROM
    /// THAT. It exists because two findings, recorded in
    /// docs/architecture/api/HANDOFF-GrasshopperInsideArchicad.md, make hosting
    /// Grasshopper inside Archicad.exe the wrong trade: a Grasshopper solve holds
    /// the main thread Archicad needs in order to answer, and — the one with no
    /// in-process fix — an infinite loop in a script component, a hung
    /// third-party .gha, a blocking dialog or an access violation inside Rhino
    /// takes ARCHICAD down, unsaved model included. Out here, all of that costs
    /// this process and nothing else. The add-on can kill it at any moment; it
    /// must never hold anything Archicad cannot lose.
    /// </para>
    /// <para>
    /// ⚠️ NOTHING IN THIS PROCESS CALLS ACAPI, AND IT CANNOT BE GIVEN THE SDK.
    /// Every Archicad operation is a request over the bridge, answered by
    /// EvP.apx on Archicad's own main thread. See <see cref="BridgeClient"/>.
    /// </para>
    /// <para>
    /// ⚠️ NO RHINO TYPE IS NAMED IN THIS FILE. Touching one would make the JIT
    /// load RhinoCommon while compiling the method that is supposed to install
    /// the resolver first. That is why the session lives behind
    /// <see cref="MethodImplOptions.NoInlining"/> in its own type: the standard
    /// Rhino.Inside arrangement, and the one thing that reliably breaks when it
    /// is "simplified" into one class.
    /// </para>
    /// </remarks>
    internal static class Program
    {
        // Exit codes. The add-on reports these verbatim when a worker dies during
        // startup, so each one has to mean exactly one thing.
        private const int ExitOk = 0;
        private const int ExitBadArguments = 2;
        private const int ExitProtocolMismatch = 3;
        private const int ExitBridgeUnavailable = 4;
        private const int ExitRhinoUnavailable = 5;
        private const int ExitFaulted = 6;

        /// <summary>
        /// How long to wait for the add-on's pipe. The add-on creates it before
        /// spawning this process, so anything beyond a moment means it went away.
        /// </summary>
        private const int ConnectTimeoutMs = 30000;

        private static BridgeClient _bridge;
        private static Control _marshaller;

        /// <summary>
        /// ⚠️ STA, AND IT IS NOT DECORATION. RhinoCore is thread-affine and
        /// expects a single-threaded apartment; constructing it on an MTA thread
        /// is not a supported configuration and misbehaves later rather than
        /// failing here.
        /// </summary>
        [STAThread]
        internal static int Main(string[] args)
        {
            Arguments arguments;
            string failure;
            if (!Arguments.TryParse(args, out arguments, out failure))
            {
                // Nowhere to write it yet — this process has no console and no
                // log path — so the exit code carries it. The add-on names the
                // boot log in the message it shows.
                Console.Error.WriteLine(failure);
                return ExitBadArguments;
            }

            WorkerLog.OpenBootLog(arguments.BootLogPath);
            WorkerLog.Write(
                "Tapioca.GhWorker starting: generation " + arguments.Generation
                + ", pipe " + arguments.PipeName
                + ", Archicad JSON port " + arguments.ArchicadJsonPort);

            if (arguments.ProtocolVersion != BridgeProtocol.Version)
            {
                WorkerLog.Write(
                    "The add-on asked for bridge protocol " + arguments.ProtocolVersion
                    + " and this worker speaks " + BridgeProtocol.Version
                    + ". Rebuild and redeploy both halves together.");
                return ExitProtocolMismatch;
            }

            try
            {
                return Run(arguments);
            }
            catch (Exception exception)
            {
                WorkerLog.Write("the worker faulted: " + WorkerLog.Describe(exception));
                return ExitFaulted;
            }
            finally
            {
                Shutdown();
            }
        }

        private static int Run(Arguments arguments)
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);

            // Created BEFORE anything can raise an event: the bridge's reader
            // thread is not the UI thread, and Grasshopper's editor may only be
            // touched from the thread that owns it. This control's handle is what
            // every worker-side event marshals through.
            _marshaller = new Control();
            _marshaller.CreateControl();

            _bridge = new BridgeClient();
            string error;
            if (!_bridge.Connect(arguments.PipeName, ConnectTimeoutMs, out error))
            {
                WorkerLog.Write(error);
                return ExitBridgeUnavailable;
            }

            // From here the boot log stops being the sink: everything goes to the
            // add-on, which stamps it into logs\grasshopper.log with this pid.
            WorkerLog.AttachBridge(_bridge);
            TapiocaBridgeApi.Bind(_bridge);
            WorkerLog.Write("bridge connected");

            _bridge.EditorShowRequested += () => OnUiThread(ShowEditor);
            _bridge.EditorHideRequested += () => OnUiThread(HideEditor);
            _bridge.ShutdownRequested += () => OnUiThread(BeginShutdown);

            StartOutcome outcome = WorkerSession.Start(arguments.ArchicadJsonPort);
            _bridge.Acknowledge(
                outcome.Kind == StartOutcomeKind.Started
                    ? BridgeProtocol.AckStatus.Ok
                    : BridgeProtocol.AckStatus.Failed,
                outcome.Message);
            WorkerLog.Write(outcome.Message);

            if (outcome.Kind != StartOutcomeKind.Started)
            {
                // ⚠️ THE PROCESS EXITS RATHER THAN LINGERING. A worker with no
                // Rhino can serve nothing, and one that stayed up would look
                // healthy to the add-on's liveness check while being useless.
                return ExitRhinoUnavailable;
            }

            // Grasshopper's editor is WinForms and needs a message loop. Running
            // it HERE, on the process's own STA thread, is the thing the whole
            // boundary buys: this loop is ours to block, and Archicad's is not.
            Application.Run();
            return ExitOk;
        }

        private static void OnUiThread(Action action)
        {
            Control marshaller = _marshaller;
            if (marshaller == null || marshaller.IsDisposed)
            {
                return;
            }

            try
            {
                // BeginInvoke, not Invoke: the caller is the bridge's reader
                // thread, and a reader that waited on the UI thread would stop
                // reading exactly when a wedged solve made reading matter most.
                marshaller.BeginInvoke(action);
            }
            catch (Exception exception)
            {
                WorkerLog.Write("could not marshal to the UI thread: " + WorkerLog.Describe(exception));
            }
        }

        private static void ShowEditor()
        {
            string failure;
            bool shown = WorkerSession.SetEditorVisible(true, out failure);
            _bridge.Acknowledge(
                shown ? BridgeProtocol.AckStatus.Ok : BridgeProtocol.AckStatus.Failed,
                shown ? "Grasshopper editor shown. " + TapirConnectionCheck.Report : failure);
        }

        private static void HideEditor()
        {
            string failure;
            bool hidden = WorkerSession.SetEditorVisible(false, out failure);
            _bridge.Acknowledge(
                hidden ? BridgeProtocol.AckStatus.Ok : BridgeProtocol.AckStatus.Failed,
                hidden ? "Grasshopper editor hidden." : failure);
        }

        private static void BeginShutdown()
        {
            WorkerLog.Write("shutdown requested by the add-on");
            Application.ExitThread();
        }

        private static void Shutdown()
        {
            // The cooperative half. The add-on's TerminateProcess is the
            // guarantee and needs nothing from here; this exists so that an
            // orderly quit releases Rhino's licence lease and temporary files
            // rather than leaving them to a kill.
            try
            {
                WorkerLog.Write(WorkerSession.Stop());
            }
            catch (Exception exception)
            {
                WorkerLog.Write("the session did not stop cleanly: " + WorkerLog.Describe(exception));
            }

            // Dropped before the pipe closes, so a component that is still
            // mid-solve gets an unavailable bridge rather than one that is going
            // away underneath it.
            TapiocaBridgeApi.Unbind();

            BridgeClient bridge = _bridge;
            _bridge = null;
            if (bridge != null)
            {
                bridge.Dispose();
            }
        }

        /// <summary>The command line the add-on spawns this worker with.</summary>
        private sealed class Arguments
        {
            internal string PipeName { get; private set; }

            internal uint ProtocolVersion { get; private set; }

            internal uint Generation { get; private set; }

            internal uint ArchicadJsonPort { get; private set; }

            internal string BootLogPath { get; private set; }

            internal static bool TryParse(string[] args, out Arguments arguments, out string failure)
            {
                arguments = new Arguments();
                failure = string.Empty;
                if (args == null)
                {
                    failure = "Tapioca.GhWorker.exe takes --pipe <name> and is started by the Tapioca add-on.";
                    return false;
                }

                for (int index = 0; index < args.Length; index++)
                {
                    string name = args[index];
                    string value = index + 1 < args.Length ? args[index + 1] : null;
                    switch (name)
                    {
                        case "--pipe":
                            arguments.PipeName = value;
                            index++;
                            break;
                        case "--protocol":
                            arguments.ProtocolVersion = ParseUInt(value);
                            index++;
                            break;
                        case "--generation":
                            arguments.Generation = ParseUInt(value);
                            index++;
                            break;
                        case "--archicad-port":
                            arguments.ArchicadJsonPort = ParseUInt(value);
                            index++;
                            break;
                        case "--boot-log":
                            arguments.BootLogPath = value;
                            index++;
                            break;
                        default:
                            failure = "Unrecognised argument '" + name + "'.";
                            return false;
                    }
                }

                if (string.IsNullOrWhiteSpace(arguments.PipeName))
                {
                    // ⚠️ THERE IS NO STANDALONE MODE, AND THAT IS DELIBERATE. A
                    // worker with no bridge cannot reach Archicad, so it would be
                    // a Rhino with a Tapioca name on it and a Grasshopper whose
                    // Tapioca components all report that they are unavailable.
                    failure = "Tapioca.GhWorker.exe is started by the Tapioca Archicad add-on and needs "
                              + "--pipe <name>. Open Tapioca > Grasshopper Editor from Archicad instead.";
                    return false;
                }

                return true;
            }

            private static uint ParseUInt(string text)
            {
                uint value;
                return uint.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out value) ? value : 0u;
            }
        }
    }
}
