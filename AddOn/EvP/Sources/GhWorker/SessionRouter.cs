using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// Decodes session messages, marshals the work that needs a document to the
    /// engine thread, and answers.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ DECODING HAPPENS ON THE READER THREAD AND THE WORK HAPPENS ON THE
    /// ENGINE THREAD, AND THE SPLIT IS THE POINT. A malformed message must be
    /// refused without ever reaching the engine — a queue entry that only turns
    /// out to be nonsense once a solve ahead of it has finished is a refusal
    /// delivered minutes late. Cancellation is the deliberate exception: it is
    /// answered on the reader thread precisely because the engine is busy with
    /// the thing being cancelled.
    /// </para>
    /// <para>
    /// ⚠️ THE HOST GENERATION IS CHECKED BEFORE ANYTHING ELSE IS DONE WITH A
    /// MESSAGE. A request stamped with a generation that is not this worker's is
    /// addressed to a process that no longer exists (§7); serving it would let a
    /// host that has not noticed a restart drive a session it never opened.
    /// </para>
    /// <para>
    /// No Grasshopper type is named in this file. Everything that needs one goes
    /// through <see cref="DefinitionHost"/>, <see cref="SolutionCollector"/> or
    /// <see cref="WorkflowFacade"/>, whose methods are non-inlinable.
    /// </para>
    /// </remarks>
    internal static class SessionRouter
    {
        private static readonly object _sync = new object();

        /// <summary>
        /// The sessions this worker holds. §13 opens exactly one for the first
        /// milestone; the map is here because the protocol addresses sessions by
        /// id and a map that refuses a second one is a policy, whereas a single
        /// field would be an assumption baked into every method.
        /// </summary>
        private static readonly Dictionary<uint, WorkflowSession> _sessions = new Dictionary<uint, WorkflowSession>();

        private static BridgeClient _bridge;
        private static GhEngineThread _engine;
        private static uint _generation;

        internal static void Bind(BridgeClient bridge, GhEngineThread engine, uint generation)
        {
            _bridge = bridge;
            _engine = engine;
            _generation = generation;
        }

        internal static void Unbind()
        {
            _bridge = null;
            _engine = null;
        }

        /// <summary>
        /// Closes every session. Called during shutdown, on the engine thread.
        /// </summary>
        internal static void CloseAll()
        {
            List<WorkflowSession> sessions;
            lock (_sync)
            {
                sessions = new List<WorkflowSession>(_sessions.Values);
                _sessions.Clear();
            }

            foreach (WorkflowSession session in sessions)
            {
                session.Close();
            }
        }

        /// <summary>
        /// Handles one session message. Called on the reader thread; never
        /// throws.
        /// </summary>
        internal static void Handle(BridgeProtocol.Header header, byte[] payload)
        {
            try
            {
                Route(header, payload);
            }
            catch (Exception exception)
            {
                WorkerLog.Write(
                    "a " + header.Type + " message could not be handled: " + WorkerLog.Describe(exception));
            }
        }

        private static void Route(BridgeProtocol.Header header, byte[] payload)
        {
            SessionProtocol.Envelope envelope;
            string error;

            switch (header.Type)
            {
                case BridgeProtocol.MessageType.OpenSession:
                {
                    SessionProtocol.SessionMode mode;
                    if (!SessionProtocol.DecodeOpenSession(payload, out envelope, out mode, out error))
                    {
                        Refuse(header, error);
                        return;
                    }

                    if (!AcceptGeneration(header, envelope))
                    {
                        return;
                    }

                    OpenSession(header, envelope, mode);
                    return;
                }

                case BridgeProtocol.MessageType.CloseSession:
                {
                    if (!SessionProtocol.DecodeEnvelopeOnly(
                            payload, "close-session request", out envelope, out error))
                    {
                        Refuse(header, error);
                        return;
                    }

                    if (!AcceptGeneration(header, envelope))
                    {
                        return;
                    }

                    WorkflowSession session = Take(envelope.SessionId);
                    if (session == null)
                    {
                        // Not an error. A close for a session the worker has
                        // already dropped is what a restart race looks like from
                        // the host's side, and the state it wanted is the state
                        // it has.
                        Answer(header, envelope, SessionProtocol.SessionState.Empty,
                               SessionProtocol.FailureCode.None, string.Empty);
                        return;
                    }

                    Post(() =>
                    {
                        session.Close();
                        Answer(header, envelope, SessionProtocol.SessionState.Empty,
                               SessionProtocol.FailureCode.None, string.Empty);
                    });
                    return;
                }

                case BridgeProtocol.MessageType.SetSessionMode:
                {
                    SessionProtocol.SessionMode mode;
                    if (!SessionProtocol.DecodeSetSessionMode(payload, out envelope, out mode, out error))
                    {
                        Refuse(header, error);
                        return;
                    }

                    WorkflowSession session = Resolve(header, envelope);
                    if (session == null)
                    {
                        return;
                    }

                    SetMode(header, envelope, session, mode);
                    return;
                }

                case BridgeProtocol.MessageType.LoadDefinition:
                {
                    string path;
                    if (!SessionProtocol.DecodeLoadDefinition(payload, out envelope, out path, out error))
                    {
                        Refuse(header, error);
                        return;
                    }

                    WorkflowSession session = Resolve(header, envelope);
                    if (session == null)
                    {
                        return;
                    }

                    session.SetState(SessionProtocol.SessionState.Loading);
                    Post(() => LoadOnEngine(header, envelope, session, path, false));
                    return;
                }

                case BridgeProtocol.MessageType.ReloadDefinition:
                {
                    if (!SessionProtocol.DecodeEnvelopeOnly(payload, "reload request", out envelope, out error))
                    {
                        Refuse(header, error);
                        return;
                    }

                    WorkflowSession session = Resolve(header, envelope);
                    if (session == null)
                    {
                        return;
                    }

                    session.SetState(SessionProtocol.SessionState.Reloading);
                    Post(() => LoadOnEngine(header, envelope, session, null, true));
                    return;
                }

                case BridgeProtocol.MessageType.GetSchema:
                {
                    if (!SessionProtocol.DecodeEnvelopeOnly(payload, "schema request", out envelope, out error))
                    {
                        Refuse(header, error);
                        return;
                    }

                    WorkflowSession session = Resolve(header, envelope);
                    if (session == null)
                    {
                        return;
                    }

                    Post(() => SchemaOnEngine(header, envelope, session));
                    return;
                }

                case BridgeProtocol.MessageType.SetInputs:
                {
                    IList<SessionProtocol.InputValue> inputs;
                    if (!SessionProtocol.DecodeSetInputs(payload, out envelope, out inputs, out error))
                    {
                        Refuse(header, error);
                        return;
                    }

                    WorkflowSession session = Resolve(header, envelope);
                    if (session == null)
                    {
                        return;
                    }

                    // ⚠️ RECORDED ON THIS THREAD, APPLIED ON THE ENGINE. The
                    // snapshot is what the next solve reads; recording it here
                    // means a drag's newest values are already in place when the
                    // one pending solve finally runs, instead of each queued
                    // application overwriting the next.
                    session.SetInputs(inputs);
                    Post(() => ApplyInputsOnEngine(header, envelope, session));
                    return;
                }

                case BridgeProtocol.MessageType.Solve:
                {
                    uint wants;
                    if (!SessionProtocol.DecodeSolve(payload, out envelope, out wants, out error))
                    {
                        Refuse(header, error);
                        return;
                    }

                    WorkflowSession session = Resolve(header, envelope);
                    if (session == null)
                    {
                        return;
                    }

                    if (session.RequestSolve(envelope.RequestRevision, wants))
                    {
                        Post(() => SolveOnEngine(header, envelope, session));
                    }

                    return;
                }

                case BridgeProtocol.MessageType.CancelSolve:
                {
                    if (!SessionProtocol.DecodeEnvelopeOnly(payload, "cancel request", out envelope, out error))
                    {
                        Refuse(header, error);
                        return;
                    }

                    WorkflowSession session = Resolve(header, envelope);
                    if (session == null)
                    {
                        return;
                    }

                    // ⚠️ NOT MARSHALLED, AND THAT IS THE ONLY WAY IT CAN WORK.
                    // A cancel queued behind the solution it is meant to
                    // interrupt is delivered after that solution has finished.
                    session.SetState(SessionProtocol.SessionState.Cancelling);
                    string report = SolutionCollector.RequestCancel();
                    WorkerLog.Write(report);
                    Answer(header, envelope, SessionProtocol.SessionState.Cancelling,
                           SessionProtocol.FailureCode.None, report);
                    return;
                }

                case BridgeProtocol.MessageType.GetDiagnostics:
                {
                    if (!SessionProtocol.DecodeEnvelopeOnly(payload, "diagnostics request", out envelope, out error))
                    {
                        Refuse(header, error);
                        return;
                    }

                    WorkflowSession session = Resolve(header, envelope);
                    if (session == null)
                    {
                        return;
                    }

                    // Answered off the engine on purpose: the question a
                    // diagnostics request is usually asked to settle is "why is
                    // this session not answering", and routing it through the
                    // queue that is stuck would make it unanswerable exactly
                    // then.
                    Send(BridgeProtocol.MessageType.DiagnosticsResult, header.RequestId,
                         SessionProtocol.EncodeDiagnosticsResult(envelope, Describe(session)));
                    return;
                }

                default:
                    WorkerLog.Write("ignored a " + header.Type + " message sent in the wrong direction");
                    return;
            }
        }

        // ---- engine-thread work -------------------------------------------

        private static void LoadOnEngine(
            BridgeProtocol.Header header, SessionProtocol.Envelope envelope, WorkflowSession session, string path,
            bool reload)
        {
            string message;
            SessionProtocol.FailureCode failure = reload
                ? session.Definition.Reload(out message)
                : session.Definition.Load(path, out message);

            if (failure != SessionProtocol.FailureCode.None)
            {
                session.SetState(SessionProtocol.SessionState.Invalid);
                Answer(header, envelope, SessionProtocol.SessionState.Invalid, failure, message);
                return;
            }

            session.SetState(SessionProtocol.SessionState.Loaded);
            Answer(header, envelope, SessionProtocol.SessionState.Loaded, SessionProtocol.FailureCode.None,
                   "Loaded " + session.Definition.Path + ".");

            // The schema follows the load unasked. The host needs it before it
            // can set a single input, so making it ask would be one more round
            // trip on the one path where the panel is empty and waiting.
            SchemaOnEngine(header, envelope, session);
        }

        private static void SchemaOnEngine(
            BridgeProtocol.Header header, SessionProtocol.Envelope envelope, WorkflowSession session)
        {
            if (!session.Definition.IsLoaded)
            {
                Answer(header, envelope, session.State, SessionProtocol.FailureCode.DefinitionInvalid,
                       "This session has no definition loaded, so it has no schema.");
                return;
            }

            string workflowId = session.Definition.ContentHash;
            string json = WorkflowFacade.DescribeSchema(
                session.Definition.Document, workflowId, System.IO.Path.GetFileName(session.Definition.Path));
            Send(BridgeProtocol.MessageType.SchemaResult, header.RequestId,
                 SessionProtocol.EncodeSchemaResult(envelope, session.Definition.Identity, json));
        }

        private static void ApplyInputsOnEngine(
            BridgeProtocol.Header header, SessionProtocol.Envelope envelope, WorkflowSession session)
        {
            if (!session.Definition.IsLoaded)
            {
                Answer(header, envelope, session.State, SessionProtocol.FailureCode.DefinitionInvalid,
                       "This session has no definition loaded, so its inputs cannot be set.");
                return;
            }

            string[] ids;
            string[] values;
            string hash;
            session.SnapshotFor(out ids, out values, out hash);

            string rejected = WorkflowFacade.ApplyInputs(session.Definition.Document, ids, values);
            if (string.IsNullOrEmpty(rejected))
            {
                Answer(header, envelope, session.State, SessionProtocol.FailureCode.None, string.Empty);
                return;
            }

            // ⚠️ A REJECTED INPUT IS REPORTED AND THE SESSION IS NOT INVALIDATED.
            // The usual cause is a panel still showing the previous definition's
            // controls, which the next schema fixes; tearing the session down
            // over it would lose the loaded document to a transient mismatch.
            Answer(header, envelope, session.State, SessionProtocol.FailureCode.InputInvalid, rejected);
        }

        private static void SolveOnEngine(
            BridgeProtocol.Header header, SessionProtocol.Envelope envelope, WorkflowSession session)
        {
            uint requestRevision;
            uint wants;
            session.TakePendingSolve(out requestRevision, out wants);

            SessionProtocol.Envelope current = new SessionProtocol.Envelope(
                envelope.HostGeneration, envelope.SessionId, requestRevision);

            if (!session.Definition.IsLoaded)
            {
                session.SetState(SessionProtocol.SessionState.Invalid);
                Fail(header, current, SessionProtocol.FailureCode.DefinitionInvalid,
                     "This session has no definition loaded, so there is nothing to solve.", 0, null);
                return;
            }

            session.SetState(SessionProtocol.SessionState.Solving);
            Send(BridgeProtocol.MessageType.SolutionStarted, header.RequestId,
                 SessionProtocol.EncodeEnvelopeOnly(current));

            SolutionCollection collected = SolutionCollector.Solve(session.Definition.Document, wants);

            if (!collected.Ok)
            {
                session.SetState(SessionProtocol.SessionState.Loaded);
                Fail(header, current, collected.Failure, collected.Message, collected.ElapsedMs,
                     collected.Diagnostics);
                return;
            }

            // ⚠️ THE STALENESS CHECK IS AFTER THE SOLVE, NOT INSTEAD OF IT, AND
            // THE RESULT IS STILL SENT. §7: "a completed stale solve is collected
            // for diagnostics but never published". The host is the thing that
            // decides not to publish, and it can only do that if it is told; a
            // worker that dropped the message would leave the host waiting for a
            // result that is never coming.
            bool stale = session.IsStale(requestRevision);
            string[] ids;
            string[] values;
            string hash;
            session.SnapshotFor(out ids, out values, out hash);

            uint solutionRevision = session.NextSolutionRevision();
            session.SetState(stale ? SessionProtocol.SessionState.Loaded : SessionProtocol.SessionState.Published);

            Send(
                BridgeProtocol.MessageType.SolutionResult,
                header.RequestId,
                SessionProtocol.EncodeSolutionResult(
                    current,
                    solutionRevision,
                    collected.ElapsedMs,
                    0,
                    0,
                    session.Definition.Identity,
                    hash,
                    collected.Outputs,
                    collected.Diagnostics));

            if (stale)
            {
                WorkerLog.Write(
                    "session " + session.SessionId.ToString(CultureInfo.InvariantCulture) + " solved request "
                    + requestRevision.ToString(CultureInfo.InvariantCulture)
                    + " after a newer one arrived; it is diagnostics, not a publication.");
            }
        }

        // ---- plumbing ------------------------------------------------------

        private static void OpenSession(
            BridgeProtocol.Header header, SessionProtocol.Envelope envelope, SessionProtocol.SessionMode mode)
        {
            WorkflowSession existing;
            lock (_sync)
            {
                if (!_sessions.TryGetValue(envelope.SessionId, out existing))
                {
                    _sessions[envelope.SessionId] = new WorkflowSession(envelope.SessionId, mode);
                    existing = null;
                }
            }

            if (existing != null)
            {
                // Idempotent, like every other start in this bridge: a host that
                // reopens a session it already has is a host recovering, and
                // building a second one under the same id would leave the first
                // document resident with nothing able to reach it.
                existing.SetMode(mode);
                Answer(header, envelope, existing.State, SessionProtocol.FailureCode.None, string.Empty);
                return;
            }

            Answer(header, envelope, SessionProtocol.SessionState.Empty, SessionProtocol.FailureCode.None,
                   "Session " + envelope.SessionId.ToString(CultureInfo.InvariantCulture) + " open in "
                   + (mode == SessionProtocol.SessionMode.Authoring ? "authoring" : "headless") + " mode.");
        }

        private static void SetMode(
            BridgeProtocol.Header header, SessionProtocol.Envelope envelope, WorkflowSession session,
            SessionProtocol.SessionMode mode)
        {
            if (session.Mode == mode)
            {
                Answer(header, envelope, session.State, SessionProtocol.FailureCode.None, string.Empty);
                return;
            }

            // ⚠️ THE HEADLESS -> AUTHORING TRANSITION IS AN OPEN QUESTION, AND
            // IT IS ANSWERED HERE BY REFUSING IT. §5 lists "whether a document
            // initialized under RunHeadless can safely become the editor's live
            // document" as NOT VERIFIED, with a documented restart fallback. A
            // silent switch would be a claim this codebase has no evidence for;
            // the restart is the fallback until an acceptance test says
            // otherwise.
            if (session.Definition.IsLoaded && mode == SessionProtocol.SessionMode.Authoring)
            {
                Answer(header, envelope, session.State, SessionProtocol.FailureCode.AccessDenied,
                       "A definition loaded headlessly cannot be handed to the Grasshopper editor in this build. "
                       + "Close this session and reopen it in authoring mode.");
                return;
            }

            session.SetMode(mode);
            Answer(header, envelope, session.State, SessionProtocol.FailureCode.None, string.Empty);
        }

        /// <summary>
        /// The session this envelope names, or null after answering why not.
        /// </summary>
        private static WorkflowSession Resolve(BridgeProtocol.Header header, SessionProtocol.Envelope envelope)
        {
            if (!AcceptGeneration(header, envelope))
            {
                return null;
            }

            WorkflowSession session;
            lock (_sync)
            {
                _sessions.TryGetValue(envelope.SessionId, out session);
            }

            if (session == null)
            {
                Answer(header, envelope, SessionProtocol.SessionState.Empty,
                       SessionProtocol.FailureCode.SessionUnknown,
                       "This worker has no session "
                       + envelope.SessionId.ToString(CultureInfo.InvariantCulture) + ".");
            }

            return session;
        }

        private static WorkflowSession Take(uint sessionId)
        {
            WorkflowSession session;
            lock (_sync)
            {
                if (_sessions.TryGetValue(sessionId, out session))
                {
                    _sessions.Remove(sessionId);
                }
            }

            return session;
        }

        private static bool AcceptGeneration(BridgeProtocol.Header header, SessionProtocol.Envelope envelope)
        {
            if (envelope.HostGeneration == _generation)
            {
                return true;
            }

            // Answered rather than dropped, and with the envelope AS SENT, so
            // the host can match it to whatever it was waiting for. StaleRevision
            // is the category: nothing failed, the request is simply addressed to
            // a worker that has been replaced.
            Answer(header, envelope, SessionProtocol.SessionState.HostRestartRequired,
                   SessionProtocol.FailureCode.StaleRevision,
                   "This request is for Grasshopper host generation "
                   + envelope.HostGeneration.ToString(CultureInfo.InvariantCulture) + " and this worker is "
                   + _generation.ToString(CultureInfo.InvariantCulture) + ".");
            return false;
        }

        private static void Refuse(BridgeProtocol.Header header, string error)
        {
            // A message that would not decode has no trustworthy envelope, so
            // there is nothing to correlate a session answer to. It goes to the
            // log, where a malformed frame belongs.
            WorkerLog.Write("refused a " + header.Type + " message: " + error);
        }

        private static void Answer(
            BridgeProtocol.Header header, SessionProtocol.Envelope envelope, SessionProtocol.SessionState state,
            SessionProtocol.FailureCode failure, string message)
        {
            Send(BridgeProtocol.MessageType.SessionEvent, header.RequestId,
                 SessionProtocol.EncodeSessionAck(envelope, state, failure, message));
        }

        private static void Fail(
            BridgeProtocol.Header header, SessionProtocol.Envelope envelope, SessionProtocol.FailureCode failure,
            string message, uint elapsedMs, IList<SessionProtocol.Diagnostic> diagnostics)
        {
            Send(BridgeProtocol.MessageType.SolutionFailed, header.RequestId,
                 SessionProtocol.EncodeSolutionFailed(envelope, failure, elapsedMs, message, diagnostics));
        }

        private static void Send(BridgeProtocol.MessageType type, uint correlationId, byte[] payload)
        {
            BridgeClient bridge = _bridge;
            if (bridge == null)
            {
                return;
            }

            bridge.SendSession(type, correlationId, payload);
        }

        private static void Post(Action action)
        {
            GhEngineThread engine = _engine;
            if (engine == null || !engine.Post(action))
            {
                WorkerLog.Write("a session request arrived after the Grasshopper engine stopped accepting work.");
            }
        }

        /// <summary>
        /// The developer diagnostics block of §11.
        /// </summary>
        private static string Describe(WorkflowSession session)
        {
            StringBuilder text = new StringBuilder();
            text.Append("Grasshopper host generation ")
                .Append(_generation.ToString(CultureInfo.InvariantCulture))
                .Append(", pid ")
                .Append(System.Diagnostics.Process.GetCurrentProcess().Id.ToString(CultureInfo.InvariantCulture))
                .Append('.');
            text.Append("\nSession ").Append(session.SessionId.ToString(CultureInfo.InvariantCulture))
                .Append(": ").Append(session.State).Append(", ").Append(session.Mode).Append(" mode.");
            text.Append("\nDefinition: ")
                .Append(session.Definition.IsLoaded ? session.Definition.Identity : "(none loaded)");
            text.Append("\nSolving: ").Append(SolutionCollector.IsSolving ? "yes" : "no");
            text.Append("\nTapioca package: ").Append(WorkflowFacade.IsAvailable ? "loaded" : "NOT LOADED");
            return text.ToString();
        }
    }
}
