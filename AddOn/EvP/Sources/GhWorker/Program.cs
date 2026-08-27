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
        /// A canvas was asked for before Rhino was ready. Read and written only
        /// on the UI thread -- the bridge marshals every request through
        /// <see cref="OnUiThread"/>, and the payout in <c>Run</c> is on that same
        /// thread.
        /// </summary>
        private static bool _editorRequested;

        /// <summary>
        /// The counting proxy in front of Archicad's JSON port, or null when it
        /// is off. Off is the default: a proxy in front of every Tapir call is a
        /// latency cost and one more thing to fail, and it earns that only while
        /// someone is measuring.
        /// </summary>
        private static TapirProxy _tapirProxy;

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
            // ⚠️ DPI AWARENESS IS DECIDED HERE, BEFORE THE FIRST WINDOW EXISTS,
            // AND IT HAS TO BE DECIDED BY US RATHER THAN INHERITED.
            //
            // A process gets exactly one DPI awareness mode, and it is latched by
            // whoever creates the first window. In process, that was Archicad,
            // and Rhino ran inside whatever Archicad had already chosen. Out of
            // process it is this worker — and the marshalling Control below is
            // created before RhinoCore, so if this is left to the default, the
            // mode is latched from OUR runtimeconfig and Rhino never gets to set
            // the one it expects.
            //
            // Getting it wrong does not fail; it produces a canvas where some
            // elements are the right size and others are tiny, because
            // Grasshopper mixes controls that scale themselves against the
            // per-monitor DPI with ones that use the system DPI, and the two only
            // agree when the process is per-monitor aware. Rhino 8 is
            // per-monitor-v2 aware, so this matches it deliberately.
            SetDpiAwareness();
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);

            // Created BEFORE anything can raise an event: the bridge's reader
            // thread is not the UI thread, and Grasshopper's editor may only be
            // touched from the thread that owns it. This control's handle is what
            // every worker-side event marshals through.
            _marshaller = new Control();
            _marshaller.CreateControl();

            _bridge = new BridgeClient();

            // ⚠️ SUBSCRIBED BEFORE Connect, NOT AFTER, AND THIS IS NOT TIDINESS.
            // Connect starts the reader thread as its last act, and the add-on
            // sends ShowEditor the instant the handshake completes -- it spawned
            // this worker precisely because a user asked for a canvas. Wiring the
            // handlers afterwards loses that race every time: the reader raises
            // an event nobody is subscribed to, Raise returns silently, and the
            // canvas never appears -- while every later message (Shutdown,
            // arriving minutes on) works perfectly. That asymmetry is exactly
            // what the symptom looked like, and there is nothing in the log to
            // see, because a dropped event writes nothing.
            _bridge.EditorShowRequested += () => OnUiThread(ShowEditor);
            _bridge.EditorHideRequested += () => OnUiThread(HideEditor);
            _bridge.ShutdownRequested += () => OnUiThread(BeginShutdown);
            _bridge.RunRequested += () => OnUiThread(RunDefinition);
            // ⚠️ CANCEL IS NOT MARSHALLED, AND THAT IS THE ONLY WAY IT CAN WORK.
            // Every other request is queued to the UI thread; a cancel queued
            // behind the very solution it is meant to interrupt would be
            // delivered after that solution finished, which is precisely never
            // useful. RequestAbortSolution is documented safe to call from
            // another thread for exactly this reason.
            _bridge.CancelRequested += CancelRun;

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

            uint tapirPort = StartTapirProxy(arguments.ArchicadJsonPort);
            StartOutcome outcome = WorkerSession.Start(arguments.ArchicadJsonPort, tapirPort);
            // Acknowledged, not also logged: the add-on writes every Ack into
            // grasshopper.log itself, so doing both prints this paragraph twice.
            _bridge.Acknowledge(
                outcome.Kind == StartOutcomeKind.Started
                    ? BridgeProtocol.AckStatus.Ok
                    : BridgeProtocol.AckStatus.Failed,
                outcome.Message);

            if (outcome.Kind != StartOutcomeKind.Started)
            {
                // ⚠️ THE PROCESS EXITS RATHER THAN LINGERING. A worker with no
                // Rhino can serve nothing, and one that stayed up would look
                // healthy to the add-on's liveness check while being useless.
                return ExitRhinoUnavailable;
            }

            OpenFirstLedgerWindow();

            // The other half of the race above: the request may well have arrived
            // while Rhino was still coming up, which is the ordinary case rather
            // than the exception -- the add-on spawns and asks in one gesture,
            // and RhinoCore takes seconds. ShowEditor latches it; this is where
            // the latch is paid out, once there is a Grasshopper to show.
            ShowEditorIfRequested();

            // Grasshopper's editor is WinForms and needs a message loop. Running
            // it HERE, on the process's own STA thread, is the thing the whole
            // boundary buys: this loop is ours to block, and Archicad's is not.
            Application.Run();
            return ExitOk;
        }

        /// <summary>
        /// Applies the process DPI awareness mode and records what actually took
        /// effect.
        /// </summary>
        /// <remarks>
        /// <para>
        /// The mode is normally already applied from the .csproj's
        /// ApplicationHighDpiMode, in which case <c>SetHighDpiMode</c> returns
        /// false and changes nothing — which is why the EFFECTIVE mode is logged
        /// rather than the requested one. "We asked for PerMonitorV2" and "the
        /// process is PerMonitorV2" are different claims, and only the second is
        /// worth anything when a canvas is rendering at the wrong size.
        /// </para>
        /// <para>
        /// TAPIOCA_GH_DPI_MODE overrides it — <c>PerMonitorV2</c>,
        /// <c>SystemAware</c>, <c>PerMonitor</c>, <c>DpiUnaware</c> or
        /// <c>DpiUnawareGdiScaled</c>. This is a display fault that depends on
        /// the machine, the monitor mix and the user's scaling setting, none of
        /// which are visible from here; the override means the right mode can be
        /// found by trying, on the machine that has the problem, without a
        /// rebuild for each attempt.
        /// </para>
        /// </remarks>
        private static void SetDpiAwareness()
        {
            HighDpiMode requested = HighDpiMode.PerMonitorV2;
            string configured = Environment.GetEnvironmentVariable("TAPIOCA_GH_DPI_MODE");
            if (!string.IsNullOrWhiteSpace(configured))
            {
                HighDpiMode parsed;
                if (Enum.TryParse(configured.Trim(), true, out parsed))
                {
                    requested = parsed;
                }
                else
                {
                    WorkerLog.Write(
                        "TAPIOCA_GH_DPI_MODE=" + configured + " is not a mode name; using " + requested + ".");
                }
            }

            bool applied = Application.SetHighDpiMode(requested);
            WorkerLog.Write(
                "DPI awareness: requested " + requested + ", " + (applied ? "applied" : "already set")
                + ", effective " + Application.HighDpiMode + ".");
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

        /// <summary>
        /// Shows the canvas, or remembers that one was asked for when Rhino is
        /// not up yet.
        /// </summary>
        /// <remarks>
        /// ⚠️ THE LATCH IS THE NORMAL PATH, NOT THE EDGE CASE. The add-on spawns
        /// this worker and asks for a canvas in one gesture, because making a
        /// user click twice for one intention would be absurd -- and it must not
        /// block Archicad's main thread waiting for RhinoCore, which is the whole
        /// reason the worker exists. So the request routinely arrives seconds
        /// before there is anything to show. Refusing it there would report
        /// "Rhino is not running" for a worker that was two seconds from ready.
        /// </remarks>
        private static void ShowEditor()
        {
            if (!WorkerSession.IsRunning)
            {
                _editorRequested = true;
                WorkerLog.Write("editor requested while Rhino was still starting; it will be shown when it is up");
                return;
            }

            _editorRequested = false;
            string failure;
            bool shown = WorkerSession.SetEditorVisible(true, out failure);
            _bridge.Acknowledge(
                shown ? BridgeProtocol.AckStatus.Ok : BridgeProtocol.AckStatus.Failed,
                shown ? "Grasshopper editor shown. " + TapirConnectionCheck.Report : failure);
        }

        private static void ShowEditorIfRequested()
        {
            if (_editorRequested)
            {
                ShowEditor();
            }
        }

        private static void HideEditor()
        {
            // Clears the latch too: "hide it" after "show it" and before Rhino is
            // up means the user changed their mind, not that a canvas is owed.
            _editorRequested = false;
            string failure;
            bool hidden = WorkerSession.SetEditorVisible(false, out failure);
            _bridge.Acknowledge(
                hidden ? BridgeProtocol.AckStatus.Ok : BridgeProtocol.AckStatus.Failed,
                hidden ? "Grasshopper editor hidden." : failure);
        }

        /// <summary>
        /// Starts the Tapir proxy when asked, and returns the port Tapir should
        /// be pointed at — the proxy's when it is up, Archicad's own otherwise.
        /// </summary>
        /// <remarks>
        /// ⚠️ A PROXY THAT WILL NOT START MUST DEGRADE TO "NO MEASUREMENT",
        /// NEVER TO "NO TAPIR". Falling back to the real port keeps every
        /// definition working; the only thing lost is the undo ledger.
        /// </remarks>
        private static uint StartTapirProxy(uint archicadPort)
        {
            string enabled = Environment.GetEnvironmentVariable("TAPIOCA_GH_TAPIR_PROXY");
            if (string.IsNullOrEmpty(enabled) || enabled == "0")
            {
                return archicadPort;
            }

            TapirProxy proxy = new TapirProxy((int)archicadPort);
            string error;
            if (!proxy.Start(out error))
            {
                WorkerLog.Write(error + " Tapir will talk to Archicad directly and no undo ledger is kept.");
                return archicadPort;
            }

            _tapirProxy = proxy;
            WorkerLog.Write(
                "Tapir proxy listening on 127.0.0.1:" + proxy.Port + ", forwarding to " + archicadPort
                + ". Every Tapir call is counted for the undo budget.");
            return (uint)proxy.Port;
        }

        /// <summary>
        /// Opens the first ledger window, after startup has finished making its
        /// own Tapir calls.
        /// </summary>
        /// <remarks>
        /// The connection check makes half a dozen calls of its own while the
        /// worker comes up. They are reads, so they would not change a verdict,
        /// but they would sit in the first run's report as traffic the user did
        /// not cause — and a report is worth less every time it says something
        /// the reader has to learn to ignore.
        /// </remarks>
        private static void OpenFirstLedgerWindow()
        {
            TapirProxy proxy = _tapirProxy;
            if (proxy != null)
            {
                proxy.ResetLedger();
            }
        }

        private static void RunDefinition()
        {
            // ⚠️ THE WINDOW IS "SINCE THE LAST RUN", NOT "DURING THIS SOLVE", AND
            // THAT IS THE ONLY WINDOW THAT SEES ANYTHING. Tapir's write components
            // fire from their own capsule button, not from SolveInstance — so a
            // press happens BETWEEN runs. Resetting on the way in, which is what
            // this did first, wiped every one of them and reported a loop full of
            // writes as costing nothing. Measured live on 2026-08-27: four
            // CreateLineElements calls, and a run report that mentioned none of
            // them.
            //
            // So the ledger is read and THEN reset, opening the next window. A
            // Player that drives those buttons itself would make the two windows
            // the same thing; until then this is the honest one.
            TapirProxy proxy = _tapirProxy;
            RunReport report = DefinitionRunner.Run();
            if (proxy != null)
            {
                report.Ledger = proxy.Ledger();
                proxy.ResetLedger();
            }

            _bridge.SendRunResult(report);
            WorkerLog.Write(report.Headline);
        }

        private static void CancelRun()
        {
            WorkerLog.Write(DefinitionRunner.RequestCancel());
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

            TapirProxy proxy = _tapirProxy;
            _tapirProxy = null;
            if (proxy != null)
            {
                proxy.Dispose();
            }

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
