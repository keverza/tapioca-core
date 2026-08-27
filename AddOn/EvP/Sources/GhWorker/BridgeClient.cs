using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Threading;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// The worker's end of the Archicad bridge: one duplex named pipe, one
    /// reader thread, one heartbeat thread.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ THE READER THREAD IS NOT THE UI THREAD, AND THAT IS THE WHOLE POINT.
    /// Grasshopper solves on this process's UI thread; if the transport were
    /// served there, a definition that never returns would also stop the worker
    /// answering the add-on — and the add-on's only evidence that a worker is
    /// wedged is the heartbeat. Reading and heartbeating from their own threads
    /// is what makes "the definition hung" reportable instead of silent.
    /// </para>
    /// <para>
    /// ⚠️ <see cref="Call"/> IS CALLED FROM A GRASSHOPPER SOLVE AND BLOCKS.
    /// That is legal here and was not legal in process: the thread it blocks
    /// belongs to this worker, and Archicad's main thread stays free to answer.
    /// It is the same reason the unmodified Tapir .gha works out of process
    /// (HANDOFF §"Tapir needs no change and no fork").
    /// </para>
    /// <para>
    /// Requests are correlated by id: the reply carries the request's id in its
    /// correlationId, so several components may have calls outstanding without
    /// any of them reading another's answer.
    /// </para>
    /// </remarks>
    internal sealed class BridgeClient : IDisposable
    {
        private const int HeartbeatIntervalMs = 3000;

        /// <summary>
        /// Long enough for an Archicad round trip that has to queue behind the
        /// user's own work, short enough that a component reports rather than
        /// hangs. MainThreadGate's own default is 30 s and this must outlast it,
        /// or a gate timeout would surface here as a bridge timeout.
        /// </summary>
        private static readonly TimeSpan CallTimeout = TimeSpan.FromSeconds(45);

        private readonly object _writeSync = new object();
        private readonly object _pendingSync = new object();
        private readonly Dictionary<uint, PendingCall> _pending = new Dictionary<uint, PendingCall>();

        private NamedPipeClientStream _pipe;
        private Thread _reader;
        private Thread _heartbeat;
        private volatile bool _stopping;
        private int _nextRequestId;
        private volatile BridgeProtocol.Capabilities _granted = BridgeProtocol.Capabilities.None;

        /// <summary>
        /// The batches Archicad has not finished with. Owned here because the
        /// ack that releases one arrives on the reader thread.
        /// </summary>
        private readonly PreviewSegments _segments = new PreviewSegments();

        internal event Action EditorShowRequested;

        internal event Action EditorHideRequested;

        internal event Action ShutdownRequested;

        internal event Action RunRequested;

        /// <summary>
        /// ⚠️ RAISED ON THE READER THREAD, NOT MARSHALLED. See the note where
        /// <see cref="Program"/> subscribes: a cancel queued behind the solution
        /// it is meant to interrupt arrives after that solution has finished.
        /// </summary>
        internal event Action CancelRequested;

        /// <summary>
        /// Archicad cannot trust its preview mirror any more and the next batch
        /// must be a FULL one rather than a delta against it. Raised on the
        /// reader thread with the reason.
        /// </summary>
        internal event Action<string> PreviewResyncRequested;

        /// <summary>
        /// Someone picked a preview primitive in Archicad's viewport. Metadata
        /// only: it must never cause a re-solve, a retessellation or a geometry
        /// transfer.
        /// </summary>
        internal event Action<ulong> PreviewPicked;

        /// <summary>
        /// True when the add-on GRANTED preview at the handshake. A component
        /// that reads false must not convert geometry at all -- the whole point
        /// of the gate is that off costs nothing.
        /// </summary>
        internal bool PreviewGranted
        {
            get { return (_granted & BridgeProtocol.Capabilities.Preview) != 0; }
        }

        internal bool IsConnected
        {
            get { return _pipe != null && _pipe.IsConnected && !_stopping; }
        }

        private sealed class PendingCall
        {
            internal readonly ManualResetEventSlim Done = new ManualResetEventSlim(false);

            internal string Envelope;
        }

        /// <summary>
        /// Connects, says hello and waits for the add-on's answer. Returns false
        /// with a reason rather than throwing: every failure here is something a
        /// user has to read, and a worker whose only output is a stack trace in a
        /// process with no console is a worker that failed silently.
        /// </summary>
        internal bool Connect(string pipeName, int connectTimeoutMs, out string error)
        {
            error = string.Empty;
            try
            {
                _pipe = new NamedPipeClientStream(
                    ".",
                    pipeName,
                    PipeDirection.InOut,
                    PipeOptions.Asynchronous);
                _pipe.Connect(connectTimeoutMs);
            }
            catch (Exception exception)
            {
                error = "Could not connect to the Tapioca bridge pipe " + pipeName + ": " + Describe(exception);
                return false;
            }

            try
            {
                int processId = Process.GetCurrentProcess().Id;
                WriteMessage(
                    BridgeProtocol.MessageType.Hello,
                    0,
                    0,
                    BridgeProtocol.EncodeHelloPayload(processId, BridgeProtocol.Capabilities.Preview));

                byte[] headerBytes = ReadExact(BridgeProtocol.HeaderSize);
                if (headerBytes == null)
                {
                    error = "The add-on closed the bridge without answering the handshake.";
                    return false;
                }

                BridgeProtocol.Header header;
                string protocolError;
                if (!BridgeProtocol.DecodeHeader(headerBytes, out header, out protocolError))
                {
                    error = protocolError;
                    return false;
                }

                byte[] payload = header.PayloadBytes == 0 ? new byte[0] : ReadExact((int)header.PayloadBytes);
                if (header.Type != BridgeProtocol.MessageType.HelloAck)
                {
                    error = "The add-on answered the handshake with a " + header.Type + " message.";
                    return false;
                }

                // The add-on answers with the capabilities it GRANTED and, when
                // it is refusing, the reason -- spelled out on the wire so a
                // worker can report it in its own window rather than merely
                // being disconnected.
                BridgeProtocol.Capabilities grantedBits;
                string refusal;
                string ackError;
                if (!BridgeProtocol.DecodeHelloAckPayload(payload, out grantedBits, out refusal, out ackError))
                {
                    error = ackError;
                    return false;
                }

                if (!string.IsNullOrEmpty(refusal))
                {
                    error = refusal;
                    return false;
                }

                // ⚠️ THE GRANTED WORD, NOT THE OFFERED ONE. A worker that acted
                // on its own offer would collect, convert and send preview into
                // an add-on that has it switched off -- the exact cost "preview
                // off costs nothing" promises never to pay.
                _granted = grantedBits;
            }
            catch (Exception exception)
            {
                error = "The bridge handshake failed: " + Describe(exception);
                return false;
            }

            _reader = new Thread(ReadLoop);
            _reader.IsBackground = true;
            _reader.Name = "Tapioca bridge reader";
            _reader.Start();

            _heartbeat = new Thread(HeartbeatLoop);
            _heartbeat.IsBackground = true;
            _heartbeat.Name = "Tapioca bridge heartbeat";
            _heartbeat.Start();
            return true;
        }

        /// <summary>
        /// Runs one Tapioca command in Archicad and returns its JSON envelope:
        /// <c>{"ok":true,"data":{...}}</c> or <c>{"ok":false,"error":"..."}</c>.
        /// Never throws; a transport failure becomes an error envelope, because
        /// the caller is a Grasshopper component that must show a message on the
        /// canvas rather than abort a solve.
        /// </summary>
        internal string Call(string commandName, string parametersJson)
        {
            if (!IsConnected)
            {
                return ErrorEnvelope(
                    "This Grasshopper is not connected to Archicad. Open Tapioca > Grasshopper Editor from "
                    + "Archicad's menu; a Grasshopper started any other way has no add-on to talk to.");
            }

            if (string.IsNullOrWhiteSpace(commandName))
            {
                return ErrorEnvelope("No command name was given.");
            }

            uint requestId = (uint)Interlocked.Increment(ref _nextRequestId);
            PendingCall pending = new PendingCall();
            lock (_pendingSync)
            {
                _pending[requestId] = pending;
            }

            try
            {
                WriteMessage(
                    BridgeProtocol.MessageType.ApiRequest,
                    requestId,
                    0,
                    BridgeProtocol.EncodeApiRequestPayload(commandName, parametersJson));

                if (!pending.Done.Wait(CallTimeout))
                {
                    return ErrorEnvelope(
                        "Archicad did not answer '" + commandName + "' within "
                        + (int)CallTimeout.TotalSeconds + " seconds.");
                }

                return pending.Envelope ?? ErrorEnvelope("Archicad returned an empty answer.");
            }
            catch (Exception exception)
            {
                return ErrorEnvelope("The Archicad bridge failed: " + Describe(exception));
            }
            finally
            {
                lock (_pendingSync)
                {
                    _pending.Remove(requestId);
                }

                pending.Done.Dispose();
            }
        }

        /// <summary>
        /// Sends one line to the add-on's <c>logs\grasshopper.log</c>. The add-on
        /// stamps it with this worker's pid and restart generation — see
        /// GhLog.hpp for why the worker does not write that file itself.
        /// </summary>
        internal void Log(string line)
        {
            if (string.IsNullOrEmpty(line) || !IsConnected)
            {
                return;
            }

            try
            {
                WriteMessage(BridgeProtocol.MessageType.Log, 0, 0, BridgeProtocol.EncodeTextPayload(line));
            }
            catch (Exception)
            {
                // A log line that cannot be sent must never be the reason a solve
                // fails.
            }
        }

        /// <summary>
        /// Sends the report from one Run. Separate from <see cref="Acknowledge"/>
        /// because the add-on shows this to the user and merely logs the other.
        /// </summary>
        internal void SendRunResult(RunReport report)
        {
            if (!IsConnected)
            {
                return;
            }

            try
            {
                WriteMessage(
                    BridgeProtocol.MessageType.RunResult,
                    0,
                    0,
                    BridgeProtocol.EncodeRunReportPayload(report));
            }
            catch (Exception exception)
            {
                WorkerLog.Write("the run result could not be sent: " + Describe(exception));
            }
        }

        /// <summary>
        /// Sends one preview batch: the segment first, then the frames the GHA
        /// framed. Returns an empty string on success and a reason otherwise;
        /// never throws, because the caller is a Grasshopper solve.
        /// </summary>
        /// <remarks>
        /// <para>
        /// ⚠️ THE SEGMENT IS PUBLISHED BEFORE THE FIRST FRAME AND RELEASED
        /// BY THE HOST'S ACK. A frame referencing a segment that does not exist
        /// yet is a batch the host must refuse; a segment freed before the ack is
        /// memory the host is still reading. The order here is the whole
        /// contract.
        /// </para>
        /// <para>
        /// ⚠️ THE FRAMES ARE OPAQUE HERE ON PURPOSE. The wire format is the
        /// GHA's (Sources/GrasshopperComponents/PreviewChannel.cs); this worker
        /// relays bytes and owns the transport, so exactly one place knows how a
        /// primitive is laid out and it is the one beside the conversion that
        /// produced it.
        /// </para>
        /// <para>
        /// ⚠️ A FAILED SEND MEANS THE WORKER'S MIRROR HAS ALREADY ADVANCED.
        /// The diff was computed against it, so the two sides now disagree and no
        /// retry against that mirror can fix it. The caller must drop its mirror
        /// and send a full batch next -- which is what a resync request from the
        /// other direction also asks for.
        /// </para>
        /// </remarks>
        internal string SendPreviewBatch(
            uint epoch, uint revision, uint[] messageTypes, byte[][] payloads, byte[] segment, string segmentName)
        {
            if (!IsConnected)
            {
                return "This Grasshopper is not connected to Archicad.";
            }

            if (!PreviewGranted)
            {
                return "Archicad did not grant the preview capability for this session.";
            }

            if (messageTypes == null || payloads == null || messageTypes.Length != payloads.Length)
            {
                return "The preview batch's frames and message types did not match.";
            }

            if (segment != null && segment.Length > 0)
            {
                string segmentError;
                if (!_segments.Publish(segmentName, epoch, revision, segment, out segmentError))
                {
                    return segmentError;
                }
            }

            try
            {
                for (int index = 0; index < messageTypes.Length; index++)
                {
                    WriteMessage(
                        (BridgeProtocol.MessageType)messageTypes[index], 0, 0, payloads[index] ?? new byte[0]);
                }
            }
            catch (Exception exception)
            {
                // The batch is half-written and the host will refuse it at the
                // footer that never arrives; the segment goes now rather than
                // waiting for an ack that cannot come.
                _segments.Release(epoch, revision);
                return "The preview batch could not be sent: " + Describe(exception);
            }

            return string.Empty;
        }

        /// <summary>
        /// Tells Archicad to forget everything this worker previewed. Sent when a
        /// definition closes and when a send failed, since both leave the two
        /// mirrors disagreeing.
        /// </summary>
        internal void SendPreviewDropAll(uint epoch, byte[] payload)
        {
            _segments.ReleaseAll();
            if (!IsConnected || !PreviewGranted)
            {
                return;
            }

            try
            {
                WriteMessage(BridgeProtocol.MessageType.PreviewDropAll, 0, 0, payload ?? new byte[0]);
            }
            catch (Exception exception)
            {
                WorkerLog.Write("a preview drop could not be sent: " + Describe(exception));
            }
        }

        internal void Acknowledge(BridgeProtocol.AckStatus status, string message)
        {
            if (!IsConnected)
            {
                return;
            }

            try
            {
                WriteMessage(
                    BridgeProtocol.MessageType.Ack,
                    0,
                    0,
                    BridgeProtocol.EncodeAckPayload(status, message));
            }
            catch (Exception)
            {
            }
        }

        public void Dispose()
        {
            _stopping = true;
            // Nothing outstanding will ever be acknowledged now, so every segment
            // goes with the connection rather than waiting for an ack from a host
            // that is no longer listening.
            _segments.Dispose();
            try
            {
                if (_pipe != null)
                {
                    _pipe.Dispose();
                }
            }
            catch (Exception)
            {
            }

            // Every waiting caller is released rather than left on a pipe that
            // will never answer: a Grasshopper solve blocked forever on a dead
            // bridge is exactly the hang this process exists to prevent.
            lock (_pendingSync)
            {
                foreach (KeyValuePair<uint, PendingCall> entry in _pending)
                {
                    entry.Value.Envelope = ErrorEnvelope("The Archicad bridge closed.");
                    entry.Value.Done.Set();
                }
            }
        }

        private void ReadLoop()
        {
            try
            {
                while (!_stopping)
                {
                    byte[] headerBytes = ReadExact(BridgeProtocol.HeaderSize);
                    if (headerBytes == null)
                    {
                        break;
                    }

                    BridgeProtocol.Header header;
                    string protocolError;
                    if (!BridgeProtocol.DecodeHeader(headerBytes, out header, out protocolError))
                    {
                        // A framing error leaves the stream position unknown, so
                        // there is nothing to resynchronise to. Drop the
                        // connection and let the supervisor decide.
                        WorkerLog.Write("bridge dropped: " + protocolError);
                        break;
                    }

                    byte[] payload = header.PayloadBytes == 0 ? new byte[0] : ReadExact((int)header.PayloadBytes);
                    if (payload == null)
                    {
                        break;
                    }

                    Dispatch(header, payload);
                }
            }
            catch (Exception exception)
            {
                if (!_stopping)
                {
                    WorkerLog.Write("bridge reader stopped: " + Describe(exception));
                }
            }
            finally
            {
                Dispose();
            }
        }

        private void Dispatch(BridgeProtocol.Header header, byte[] payload)
        {
            switch (header.Type)
            {
                case BridgeProtocol.MessageType.ApiResponse:
                {
                    PendingCall pending;
                    lock (_pendingSync)
                    {
                        _pending.TryGetValue(header.CorrelationId, out pending);
                    }

                    if (pending != null)
                    {
                        pending.Envelope = BridgeProtocol.DecodeTextPayload(payload);
                        pending.Done.Set();
                    }

                    break;
                }

                case BridgeProtocol.MessageType.ShowEditor:
                    Raise(EditorShowRequested);
                    break;

                case BridgeProtocol.MessageType.HideEditor:
                    Raise(EditorHideRequested);
                    break;

                case BridgeProtocol.MessageType.Shutdown:
                    Raise(ShutdownRequested);
                    break;

                case BridgeProtocol.MessageType.RunDefinition:
                    Raise(RunRequested);
                    break;

                case BridgeProtocol.MessageType.CancelRun:
                    Raise(CancelRequested);
                    break;

                case BridgeProtocol.MessageType.PreviewBatchAck:
                {
                    uint epoch;
                    uint revision;
                    bool accepted;
                    string reason;
                    string ackError;
                    if (!BridgeProtocol.DecodePreviewBatchAckPayload(
                            payload, out epoch, out revision, out accepted, out reason, out ackError))
                    {
                        WorkerLog.Write("a preview batch ack could not be read: " + ackError);
                        break;
                    }

                    // ⚠️ RELEASED WHETHER OR NOT THE BATCH WAS ACCEPTED. The
                    // ack means Archicad is finished with the memory, not that it
                    // liked what was in it; holding a refused batch's segment
                    // leaks exactly the same bytes as holding an accepted one.
                    _segments.Release(epoch, revision);
                    if (!accepted)
                    {
                        WorkerLog.Write(
                            "Archicad refused preview batch " + epoch + "." + revision + ": " + reason);
                    }

                    break;
                }

                case BridgeProtocol.MessageType.PreviewResyncRequest:
                {
                    uint epoch;
                    string reason;
                    string resyncError;
                    if (!BridgeProtocol.DecodePreviewResyncPayload(payload, out epoch, out reason, out resyncError))
                    {
                        WorkerLog.Write("a preview resync request could not be read: " + resyncError);
                        break;
                    }

                    WorkerLog.Write("Archicad asked for a full preview resync: " + reason);
                    RaiseWith(PreviewResyncRequested, reason);
                    break;
                }

                case BridgeProtocol.MessageType.PreviewPicked:
                {
                    ulong primitiveId;
                    string pickError;
                    if (!BridgeProtocol.DecodePreviewPickedPayload(payload, out primitiveId, out pickError))
                    {
                        WorkerLog.Write("a preview pick could not be read: " + pickError);
                        break;
                    }

                    RaiseWith(PreviewPicked, primitiveId);
                    break;
                }

                default:
                    // Worker-to-host messages arriving the wrong way. The
                    // direction is part of the contract; a peer that gets it
                    // wrong is a peer whose build does not match this one.
                    WorkerLog.Write("ignored a " + header.Type + " message sent in the wrong direction");
                    break;
            }
        }

        private static void RaiseWith<T>(Action<T> handler, T value)
        {
            if (handler == null)
            {
                return;
            }

            try
            {
                handler(value);
            }
            catch (Exception exception)
            {
                WorkerLog.Write("a bridge handler threw: " + Describe(exception));
            }
        }

        private static void Raise(Action handler)
        {
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
                WorkerLog.Write("a bridge handler threw: " + Describe(exception));
            }
        }

        private void HeartbeatLoop()
        {
            try
            {
                while (!_stopping)
                {
                    Thread.Sleep(HeartbeatIntervalMs);
                    if (_stopping || !IsConnected)
                    {
                        return;
                    }

                    WriteMessage(BridgeProtocol.MessageType.Heartbeat, 0, 0, null);
                }
            }
            catch (Exception)
            {
                // A heartbeat that cannot be sent IS the signal. Stopping here
                // lets the add-on's liveness deadline do its job rather than
                // masking a dead pipe with retries.
            }
        }

        private void WriteMessage(BridgeProtocol.MessageType type, uint requestId, uint correlationId, byte[] payload)
        {
            byte[] header = BridgeProtocol.EncodeHeader(type, requestId, correlationId,
                payload == null ? 0 : payload.Length);

            // One lock around header AND payload: two writers interleaving a
            // header with another message's body is a framing error the reader
            // cannot recover from.
            lock (_writeSync)
            {
                Stream pipe = _pipe;
                if (pipe == null)
                {
                    throw new IOException("The bridge pipe is closed.");
                }

                pipe.Write(header, 0, header.Length);
                if (payload != null && payload.Length > 0)
                {
                    pipe.Write(payload, 0, payload.Length);
                }

                pipe.Flush();
            }
        }

        /// <summary>Reads exactly <paramref name="count"/> bytes, or null at end of stream.</summary>
        private byte[] ReadExact(int count)
        {
            byte[] buffer = new byte[count];
            int offset = 0;
            while (offset < count)
            {
                Stream pipe = _pipe;
                if (pipe == null)
                {
                    return null;
                }

                int read = pipe.Read(buffer, offset, count - offset);
                if (read <= 0)
                {
                    return null;
                }

                offset += read;
            }

            return buffer;
        }

        private static string ErrorEnvelope(string message)
        {
            return "{\"ok\":false,\"error\":\""
                   + (message ?? string.Empty).Replace("\\", "\\\\").Replace("\"", "\\\"")
                   + "\"}";
        }

        private static string Describe(Exception exception)
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
