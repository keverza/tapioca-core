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
                WriteMessage(BridgeProtocol.MessageType.Hello, 0, 0, BridgeProtocol.EncodeHelloPayload(processId));

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

                // A non-empty hello-ack payload is the add-on's refusal reason,
                // spelled out on the wire so a worker can report it in its own
                // window rather than merely being disconnected.
                string refusal = BridgeProtocol.DecodeTextPayload(payload);
                if (!string.IsNullOrEmpty(refusal))
                {
                    error = refusal;
                    return false;
                }
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

                default:
                    // Worker-to-host messages arriving the wrong way. The
                    // direction is part of the contract; a peer that gets it
                    // wrong is a peer whose build does not match this one.
                    WorkerLog.Write("ignored a " + header.Type + " message sent in the wrong direction");
                    break;
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
