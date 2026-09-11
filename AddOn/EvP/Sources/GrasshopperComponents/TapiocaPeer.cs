using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;

using Tapioca.GhWorker;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// This Grasshopper, serving Archicad as an attached peer.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ IT REPLACES A PROCESS, NOT A PROTOCOL. Everything below the transport
    /// is the worker's own session half, compiled from the same files (see the
    /// .csproj): the same framing, the same six session message types, the same
    /// facade. What this class supplies is only what the worker's
    /// <c>Program.cs</c> supplied and a peer cannot — a pipe to connect to, a
    /// generation to answer as, a thread to marshal onto, and a start
    /// acknowledgement. Nothing here parses a message.
    /// </para>
    /// <para>
    /// ⚠️ THE ADD-ON LISTENS AND THE PEER CONNECTS, WHICH IS WHY THIS IS SMALL.
    /// Tapioca's bridge has always been a named-pipe SERVER with the worker as
    /// its client, so a Grasshopper that is already running can complete the
    /// identical handshake. No new transport, no second embedded Rhino, and no
    /// seat taken from the Rhino the user is working in.
    /// </para>
    /// <para>
    /// ⚠️ THREE THINGS THIS MUST NEVER DO, all of them things the worker does.
    /// It must not construct or dispose a RhinoCore: the core here is the user's
    /// Rhino. It must not create or end an engine thread: Rhino's UI thread
    /// predates us and outlives us, so <see cref="IEngineDispatcher"/> is
    /// satisfied by marshalling onto it. And it must not act on a Shutdown
    /// message by quitting — a shutdown addressed to a worker means "end the
    /// process", and obeying it here would close somebody's Rhino from another
    /// application's panel.
    /// </para>
    /// </remarks>
    internal static class TapiocaPeer
    {
        /// <summary>
        /// Where Windows lists named pipes. Enumerating this directory is the
        /// whole of discovery: the add-on's pipe name carries everything a peer
        /// needs to know about it.
        /// </summary>
        private const string PipeDirectory = @"\\.\pipe\";

        /// <summary>
        /// Long enough to cover a pipe that exists but whose server is mid-accept,
        /// short enough that a stale name fails inside one solve.
        /// </summary>
        private const int ConnectTimeoutMs = 5000;

        private static readonly object Sync = new object();

        private static BridgeClient _bridge;
        private static string _pipeName = string.Empty;
        private static uint _generation;
        private const string NotConnected = "Not connected.";

        private static string _status = "Not connected. Press Connect in Tapioca's Grasshopper panel in Archicad, "
                                        + "then solve this component.";

        /// <summary>
        /// One Archicad bridge pipe, as advertised by its name.
        /// </summary>
        /// <remarks>
        /// The add-on builds the name as
        /// <c>Tapioca.Gh.v{protocol}.{archicadPid}.{generation}</c> — see
        /// GhBridge::Start, which says why each part is there: "the pid keeps two
        /// Archicads apart; the generation keeps this Archicad's successive
        /// workers apart". Both facts a peer needs are therefore ON THE WIRE
        /// NAME, and neither the handshake nor the protocol had to grow a field
        /// to carry them.
        /// </remarks>
        internal sealed class Candidate
        {
            internal string PipeName { get; set; }

            internal uint ArchicadProcessId { get; set; }

            internal uint Generation { get; set; }

            public override string ToString()
            {
                return PipeName;
            }
        }

        /// <summary>
        /// Raised when the connection or the status text actually changed.
        /// </summary>
        /// <remarks>
        /// <para>
        /// ⚠️ BECAUSE A DETACH IS NOT SOMETHING THIS SIDE DID. Archicad's
        /// panel can drop the bridge at any moment, and the peer learns of it on
        /// BridgeClient's reader thread -- not during a solve. Without this event
        /// a component would keep displaying "connected" until something else
        /// happened to re-solve it, which is a status output that lies.
        /// </para>
        /// <para>
        /// ⚠️ RAISED ONLY ON A REAL CHANGE, WHICH IS WHAT KEEPS IT FROM
        /// LOOPING. A subscriber's natural response is to re-solve, and a
        /// re-solve calls Connect again; if an unchanged status raised this, an
        /// unreachable Archicad would drive the canvas in a circle. Connect is
        /// idempotent and a repeated failure produces the same text, so a steady
        /// state -- connected or not -- is silent.
        /// </para>
        /// <para>Handlers run on whichever thread changed the state.</para>
        /// </remarks>
        internal static event Action StateChanged;

        internal static bool IsConnected
        {
            get
            {
                BridgeClient bridge;
                lock (Sync)
                {
                    bridge = _bridge;
                }

                return bridge != null && bridge.IsConnected;
            }
        }

        internal static string Status
        {
            get
            {
                lock (Sync)
                {
                    return _status;
                }
            }
        }

        internal static string PipeName
        {
            get
            {
                lock (Sync)
                {
                    return _pipeName;
                }
            }
        }

        /// <summary>
        /// Every Archicad on this machine that is currently WAITING for a peer,
        /// newest generation first.
        /// </summary>
        /// <remarks>
        /// ⚠️ THE PIPE'S EXISTENCE IS THE PERMISSION. The add-on creates it only
        /// when somebody presses Connect in the Tapioca panel, and closes it when
        /// they press Cancel, so a name found here is an Archicad that has ASKED
        /// for a Grasshopper. That is what makes it safe for a component to
        /// connect without a further prompt.
        /// </remarks>
        internal static IList<Candidate> Discover()
        {
            List<Candidate> found = new List<Candidate>();
            string prefix = "Tapioca.Gh.v"
                            + BridgeProtocol.Version.ToString(CultureInfo.InvariantCulture)
                            + ".";
            try
            {
                // Directory.GetFiles on the pipe filesystem is the documented way
                // to enumerate named pipes; the entries come back as full paths.
                string[] entries = Directory.GetFiles(PipeDirectory);
                for (int index = 0; index < entries.Length; index++)
                {
                    string name = Path.GetFileName(entries[index]);
                    if (name == null || !name.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                    {
                        // ⚠️ A DIFFERENT PROTOCOL VERSION IS NOT A CANDIDATE. The
                        // handshake would refuse it anyway, and refusing it here
                        // means the message names the version rather than the
                        // connection failing for no visible reason.
                        continue;
                    }

                    string[] parts = name.Split('.');
                    if (parts.Length < 5)
                    {
                        continue;
                    }

                    uint pid;
                    uint generation;
                    if (!uint.TryParse(parts[parts.Length - 2], NumberStyles.None, CultureInfo.InvariantCulture, out pid)
                        || !uint.TryParse(
                               parts[parts.Length - 1], NumberStyles.None, CultureInfo.InvariantCulture, out generation))
                    {
                        continue;
                    }

                    Candidate candidate = new Candidate();
                    candidate.PipeName = name;
                    candidate.ArchicadProcessId = pid;
                    candidate.Generation = generation;
                    found.Add(candidate);
                }
            }
            catch (Exception)
            {
                // Discovery that cannot read the pipe directory reports nothing
                // rather than failing a solve; Connect says so in its own words.
                return found;
            }

            found.Sort(delegate(Candidate left, Candidate right)
            {
                return right.Generation.CompareTo(left.Generation);
            });
            return found;
        }

        /// <summary>
        /// Connects this Grasshopper to a waiting Archicad. Idempotent; never
        /// throws.
        /// </summary>
        /// <param name="requestedPipe">
        /// A pipe name to insist on, or empty to discover one.
        /// </param>
        internal static bool Connect(string requestedPipe, out string status)
        {
            lock (Sync)
            {
                if (_bridge != null && _bridge.IsConnected)
                {
                    status = _status;
                    return true;
                }

                // A dead bridge from a previous Archicad is cleared before a new
                // one is opened, or the second connect would leak the first.
                if (_bridge != null)
                {
                    TearDownLocked("the previous connection had gone");
                }
            }

            string pipeName = requestedPipe == null ? string.Empty : requestedPipe.Trim();
            uint generation;
            if (pipeName.Length == 0)
            {
                IList<Candidate> candidates = Discover();
                if (candidates.Count == 0)
                {
                    status = "No Archicad on this machine is waiting for a Grasshopper. In Archicad, open the "
                             + "Tapioca panel's Grasshopper command and press Connect; this component finds it "
                             + "the next time it solves.";
                    return Report(status);
                }

                if (candidates.Count > 1)
                {
                    // ⚠️ NOT GUESSED, AND THE PRECEDENT IS TAPIR'S PORT. Picking
                    // one of two Archicads means driving somebody's model from a
                    // panel they are not looking at, and being wrong QUIETLY.
                    // Every name is listed so the Pipe input can name one.
                    StringBuilder text = new StringBuilder();
                    text.Append(candidates.Count.ToString(CultureInfo.InvariantCulture));
                    text.Append(" Archicad instances are waiting for a Grasshopper, so this component will not ");
                    text.Append("guess which model to drive. Name one with the Pipe input: ");
                    for (int index = 0; index < candidates.Count; index++)
                    {
                        text.Append(index == 0 ? string.Empty : ", ");
                        text.Append(candidates[index].PipeName);
                    }

                    status = text.ToString();
                    return Report(status);
                }

                pipeName = candidates[0].PipeName;
                generation = candidates[0].Generation;
            }
            else if (!GenerationOf(pipeName, out generation))
            {
                status = "\"" + pipeName + "\" is not a Tapioca bridge pipe name. Expected "
                         + "Tapioca.Gh.v" + BridgeProtocol.Version.ToString(CultureInfo.InvariantCulture)
                         + ".<archicad pid>.<generation>.";
                return Report(status);
            }

            BridgeClient bridge = new BridgeClient();
            string error;
            if (!bridge.Connect(pipeName, ConnectTimeoutMs, out error))
            {
                bridge.Dispose();
                status = error;
                return Report(status);
            }

            lock (Sync)
            {
                _bridge = bridge;
                _pipeName = pipeName;
                _generation = generation;
            }

            // ⚠️ THE GENERATION COMES FROM THE PIPE NAME, AND IT HAS TO COME FROM
            // SOMEWHERE. Every session payload carries the host generation and
            // the router refuses one that is not its own — that check is what
            // stops a request meant for a replaced worker being served. A peer is
            // told its generation by the only party that knows it, in the only
            // thing it is given: the name it connected to.
            WorkerLog.AttachBridge(bridge);
            TapiocaBridgeApi.Bind(bridge, generation);
            SessionRouter.Bind(bridge, RhinoUi.Instance, generation);

            bridge.EditorShowRequested += ShowEditor;
            bridge.EditorHideRequested += HideEditor;
            bridge.ShutdownRequested += OnShutdownRequested;
            bridge.RunRequested += OnRunRequested;
            bridge.CancelRequested += OnCancelRequested;
            bridge.Disconnected += OnDisconnected;

            // ⚠️ THE ACKNOWLEDGEMENT IS WHAT MAKES THE PANEL SAY "RUNNING", and
            // the worker sends it once Rhino has come up. Here Rhino came up
            // before Archicad did, so it is sent immediately — and it says WHOSE
            // Rhino, because "running" means something different when Stop will
            // disconnect rather than close it.
            bridge.Acknowledge(BridgeProtocol.AckStatus.Ok, Greeting());
            WorkerLog.Write("attached to " + pipeName + " as generation "
                            + generation.ToString(CultureInfo.InvariantCulture) + ".");

            status = "Connected to Archicad on " + pipeName + ". This Rhino is serving as Tapioca's Grasshopper; "
                     + "nothing was started and nothing will be shut down. " + PointPluginsAtThisArchicad();
            return Report(status);
        }

        /// <summary>
        /// Drops the connection, leaving this Rhino exactly as it was.
        /// </summary>
        internal static void Disconnect(string reason)
        {
            string status;
            bool changed;
            lock (Sync)
            {
                if (_bridge == null)
                {
                    changed = !string.Equals(_status, NotConnected, StringComparison.Ordinal);
                    _status = NotConnected;
                    status = _status;
                }
                else
                {
                    TearDownLocked(reason);
                    changed = true;
                    status = _status;
                }
            }

            if (changed)
            {
                // The one that matters: this path is also how the peer learns
                // Archicad detached, and the announcement is what lets a
                // component show it without being re-solved by hand.
                WorkerLog.Write(status);
                Raise();
            }
        }

        /// <summary>
        /// The generation encoded in a bridge pipe name.
        /// </summary>
        internal static bool GenerationOf(string pipeName, out uint generation)
        {
            generation = 0;
            if (string.IsNullOrEmpty(pipeName))
            {
                return false;
            }

            string prefix = "Tapioca.Gh.v" + BridgeProtocol.Version.ToString(CultureInfo.InvariantCulture) + ".";
            if (!pipeName.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }

            string[] parts = pipeName.Split('.');
            if (parts.Length < 5)
            {
                return false;
            }

            return uint.TryParse(
                parts[parts.Length - 1], NumberStyles.None, CultureInfo.InvariantCulture, out generation);
        }

        /// <summary>
        /// Points this Rhino's Archicad plug-ins — Tapir, and GRAPHISOFT's own
        /// Live Connection — at the Archicad we just connected to. Returns a
        /// line for the status text; never throws.
        /// </summary>
        /// <remarks>
        /// <para>
        /// ⚠️ TAPIR CANNOT FIND ARCHICAD BY ITSELF, AND IN A PEER NOTHING
        /// ELSE WAS DOING IT. Its ConnectionSettings.Port defaults to 19723 and
        /// the plug-in has no instance discovery, so with two Archicads open a
        /// definition silently drives whichever one holds the default -- which
        /// may not be the model on screen. A spawned worker is told the right
        /// port on its command line (GhWorkerHost::ArchicadJsonPort, which reads
        /// ACAPI_Command_GetHttpConnectionPort, the only authority for it). A
        /// peer was told nothing, so it ASKS, over the bridge it has just
        /// finished the handshake on.
        /// </para>
        /// <para>
        /// ⚠️ AND IT ONLY EVER NARROWS THE ANSWER. A port of 0 means
        /// Archicad could not tell us, and writing 0 into Tapir would break a
        /// setting that was working; the reading is left alone and the reason is
        /// reported instead.
        /// </para>
        /// <para>
        /// Tapir's absence is not a failure: a Rhino without it is a perfectly
        /// good peer, and the line says so rather than warning.
        /// </para>
        /// </remarks>
        private static string PointPluginsAtThisArchicad()
        {
            try
            {
                string reply = TapiocaBridgeApi.Call("Tapioca.GetStatus", string.Empty);
                if (!Envelope.IsOk(reply))
                {
                    return "Plug-in ports were left alone: Archicad would not report its JSON port ("
                           + Envelope.ErrorOf(reply) + ").";
                }

                int port = Reply.Count(Envelope.DataOf(reply), "jsonPort");
                if (port <= 0)
                {
                    return "Plug-in ports were left alone: this Archicad did not report a JSON port, so Tapir "
                           + "and Live Connection components need one set by hand.";
                }

                // ⚠️ BOTH PLUG-INS, BECAUSE BOTH HAVE THE SAME BUG-SHAPED
                // DEFAULT. Tapir and GRAPHISOFT's Live Connection each post to
                // 127.0.0.1:19723 with no way to discover which Archicad that
                // is; each exposes a settable port; and in a peer nothing else
                // was setting either. Neither is required to be installed.
                return "Tapir: " + TapirPackage.BindPort((uint)port) + " "
                       + ArchicadConnectionPackage.BindPort((uint)port);
            }
            catch (Exception exception)
            {
                return "Plug-in ports could not be set: " + WorkerLog.Describe(exception);
            }
        }

        private static string Greeting()
        {
            string version;
            try
            {
                version = PeerRhino.Version();
            }
            catch (Exception exception)
            {
                version = "(version unavailable: " + WorkerLog.Describe(exception) + ")";
            }

            return "Attached to a Grasshopper that was already running: Rhino " + version + ", pid "
                   + Process.GetCurrentProcess().Id.ToString(CultureInfo.InvariantCulture)
                   + ". This Rhino is the user's own; Stop will disconnect it, not close it.";
        }

        private static bool Report(string status)
        {
            bool changed;
            lock (Sync)
            {
                changed = !string.Equals(_status, status, StringComparison.Ordinal);
                _status = status;
            }

            // Outside the lock: a handler re-solves a Grasshopper document, and
            // holding this lock across that would invite a deadlock against any
            // solve that asks for the status.
            if (changed)
            {
                Raise();
            }

            return IsConnected;
        }

        private static void Raise()
        {
            Action handler = StateChanged;
            if (handler == null)
            {
                return;
            }

            try
            {
                handler();
            }
            catch (Exception exception)
            {
                // A subscriber that throws must not break the transport: this is
                // reached from the bridge reader thread on a disconnect.
                WorkerLog.Write("a peer state handler failed: " + WorkerLog.Describe(exception));
            }
        }

        /// <summary>
        /// Unbinds and disposes under <see cref="Sync"/>.
        /// </summary>
        /// <remarks>
        /// ⚠️ THE SESSIONS GO FIRST AND ON THE UI THREAD. A session owns a
        /// Grasshopper document that was added to this Rhino's document server;
        /// dropping the bridge without closing them would leave those documents
        /// in the user's own Grasshopper with nothing holding them. That is the
        /// same order GhEngineThread's teardown uses, minus the part that
        /// disposes the core.
        /// </remarks>
        private static void TearDownLocked(string reason)
        {
            BridgeClient bridge = _bridge;
            _bridge = null;

            RhinoUi.Instance.Post(SessionRouter.CloseAll);
            SessionRouter.Unbind();
            TapiocaBridgeApi.Unbind();
            WorkerLog.AttachBridge(null);

            if (bridge != null)
            {
                bridge.EditorShowRequested -= ShowEditor;
                bridge.EditorHideRequested -= HideEditor;
                bridge.ShutdownRequested -= OnShutdownRequested;
                bridge.RunRequested -= OnRunRequested;
                bridge.CancelRequested -= OnCancelRequested;
                bridge.Disconnected -= OnDisconnected;
                bridge.Dispose();
            }

            _status = "Disconnected from " + (_pipeName.Length == 0 ? "Archicad" : _pipeName)
                      + (string.IsNullOrEmpty(reason) ? "." : ": " + reason + ".")
                      + " This Rhino kept running.";
            _pipeName = string.Empty;
            _generation = 0;
        }

        private static void ShowEditor()
        {
            // Asked for by the panel, and here it means "bring the canvas the
            // user already has to the front" rather than "load an editor".
            RhinoUi.Instance.Post(PeerRhino.FocusEditor);
        }

        private static void HideEditor()
        {
            // ⚠️ REFUSED, POLITELY. In the worker this hides a canvas Tapioca
            // opened; here it would hide the window the user is working in,
            // because a panel in another application asked. Logged so the
            // asymmetry is visible in grasshopper.log rather than looking like a
            // dropped message.
            WorkerLog.Write("ignored a hide-editor request: this Grasshopper belongs to the user, not to Tapioca.");
        }

        private static void OnShutdownRequested()
        {
            // ⚠️ NEVER OBEYED AS WRITTEN. Shutdown means "end the process" to a
            // worker; to a peer it can only mean "we are done with you". The
            // add-on's attached path closes the bridge instead of sending this,
            // so arriving here means an older add-on or a spawned-worker code
            // path — and disconnecting is the safe reading either way.
            WorkerLog.Write("a shutdown request arrived; disconnecting rather than closing this Rhino.");
            Disconnect("Archicad asked the worker to shut down");
        }

        private static void OnRunRequested()
        {
            // ⚠️ DECLINED, AND SAID SO RATHER THAN DROPPED. RunDefinition is
            // the AUTHORING gesture -- "solve whatever is on the canvas" -- and
            // its worker implementation reaches for two things a peer has no
            // business holding: WorkerSession, to ask whether Rhino is up, and
            // TapirExecutor, which presses capsule buttons in the worker's own
            // staged Tapir package. The panel's Solve does not come this way; it
            // is a session message, and SessionRouter serves those in full.
            WorkerLog.Write(
                "declined a run-the-canvas request: an attached peer serves session solves, not the authoring run. "
                + "Use the Tapioca panel's Solve.");
        }

        private static void OnCancelRequested()
        {
            // Nothing to cancel on this path: the only solves a peer runs are
            // session solves, and CancelSolve is a session message the router
            // handles itself.
            WorkerLog.Write("a cancel arrived for the authoring run, which an attached peer does not serve.");
        }

        private static void OnDisconnected()
        {
            Disconnect("Archicad closed the bridge");
        }

        /// <summary>
        /// Rhino's UI thread as an <see cref="IEngineDispatcher"/>.
        /// </summary>
        /// <remarks>
        /// ⚠️ MARSHALS ONTO A THREAD IT DID NOT MAKE AND WILL NOT END. That is
        /// the entire difference between this and GhEngineThread, and the reason
        /// the interface has one method: there is no honest implementation of
        /// "stop the engine" when the engine is somebody's Rhino.
        /// </remarks>
        private sealed class RhinoUi : IEngineDispatcher
        {
            internal static readonly RhinoUi Instance = new RhinoUi();

            public bool Post(Action action)
            {
                if (action == null)
                {
                    return false;
                }

                try
                {
                    PeerRhino.InvokeOnUiThread(action);
                    return true;
                }
                catch (Exception exception)
                {
                    WorkerLog.Write("could not marshal onto Rhino's UI thread: " + WorkerLog.Describe(exception));
                    return false;
                }
            }
        }
    }
}
