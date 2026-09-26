#include "APIEnvir.h"

#include "GhBridge.hpp"
#include "GhLog.hpp"

#include "AddOnCommands.hpp"
#include "Preview/GhPreviewCache.hpp"
#include "Preview/GhPreviewIngest.hpp"
#include "Preview/GhPreviewSegmentView.hpp"
#include "Preview/PreviewRuntimeState.hpp"
#include "Python/ApiDispatcher.hpp"
#include "Python/MainThreadGate.hpp"

#include <array>
#include <string>
#include <vector>

namespace evp {
namespace grasshopper {

namespace {

// A read waits indefinitely — the worker is idle most of the time and an idle
// worker sends nothing but heartbeats. A WRITE does not: a worker that has
// stopped draining its end must not wedge whichever thread asked to cancel it.
constexpr ULONGLONG WriteTimeoutMs = 5000;

// How long the IO thread waits for a worker to appear on the pipe before giving
// up. Generous, because it covers a cold .NET start plus RhinoCore.
constexpr ULONGLONG ConnectTimeoutMs = 120000;

// A project with many layers can exceed the small Ping limit. The managed
// client enforces 64 KiB for Ping and 1 MiB for each allowlisted catalog read.
constexpr size_t MaxGh2ReplyBytes = 1024 * 1024;

// Long enough that no heartbeat has ever plausibly arrived.
constexpr uint64_t NeverHeardFrom = 0xFFFFFFFFFFFFull;

bool TransferExact (HANDLE pipe, void* buffer, DWORD size, bool write, const std::atomic<bool>& stopping,
                    ULONGLONG timeoutMs)
{
    auto* bytes = (uint8_t*) buffer;
    DWORD offset = 0;
    const ULONGLONG deadline = timeoutMs == 0 ? 0 : GetTickCount64 () + timeoutMs;
    HANDLE event = CreateEventW (nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr)
        return false;

    while (offset < size) {
        ResetEvent (event);
        OVERLAPPED operation {};
        operation.hEvent = event;
        DWORD count = 0;
        const BOOL started = write ? WriteFile (pipe, bytes + offset, size - offset, &count, &operation)
                                   : ReadFile (pipe, bytes + offset, size - offset, &count, &operation);
        if (started == 0) {
            if (GetLastError () != ERROR_IO_PENDING) {
                CloseHandle (event);
                return false;
            }
            while (true) {
                // ⚠️ CancelIoEx TAKES THE OVERLAPPED, NOT JUST THE HANDLE. Reads
                // and writes are in flight on this pipe from two different
                // threads at once; cancelling by handle alone would cancel the
                // other one too.
                if (stopping.load () || (deadline != 0 && GetTickCount64 () >= deadline)) {
                    CancelIoEx (pipe, &operation);
                    WaitForSingleObject (event, INFINITE);
                    CloseHandle (event);
                    return false;
                }
                if (WaitForSingleObject (event, 50) == WAIT_OBJECT_0)
                    break;
            }
            if (GetOverlappedResult (pipe, &operation, &count, FALSE) == 0) {
                CloseHandle (event);
                return false;
            }
        }
        if (count == 0) {
            CloseHandle (event);
            return false;
        }
        offset += count;
    }
    CloseHandle (event);
    return true;
}

bool ReadExact (HANDLE pipe, void* destination, DWORD size, const std::atomic<bool>& stopping)
{
    return TransferExact (pipe, destination, size, false, stopping, 0);
}

bool WriteExact (HANDLE pipe, const void* source, DWORD size, const std::atomic<bool>& stopping)
{
    return TransferExact (pipe, const_cast<void*> (source), size, true, stopping, WriteTimeoutMs);
}

// ⚠️ nMaxInstances IS 1, AND IT IS THE ACCESS CONTROL. One worker per Archicad,
// so the pipe accepts exactly one client: a second process that guesses the name
// gets ERROR_PIPE_BUSY rather than a second seat at the dispatcher.
// PIPE_REJECT_REMOTE_CLIENTS keeps it to this machine.
HANDLE CreateServerPipe (const GS::UniString& fullName)
{
    return CreateNamedPipeW ((LPCWSTR) fullName.ToUStr ().Get (), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024,
                             64 * 1024, 0, nullptr);
}

GS::UniString FromUtf8 (const std::string& text)
{
    return GS::UniString (text.c_str (), CC_UTF8);
}

std::string ToUtf8 (const GS::UniString& text)
{
    const auto utf8 = text.ToCStr (0, MaxUSize, CC_UTF8);
    return std::string (utf8.Get ());
}

std::string ErrorEnvelope (const GS::UniString& message)
{
    GS::UniString escaped = message;
    escaped.ReplaceAll ("\\", "\\\\");
    escaped.ReplaceAll ("\"", "\\\"");
    return "{\"ok\":false,\"error\":\"" + ToUtf8 (escaped) + "\"}";
}

} // namespace

GhBridge& GhBridge::Get ()
{
    static GhBridge instance;
    return instance;
}

GhBridge::~GhBridge ()
{
    Stop ();
}

bool GhBridge::Start (uint32_t startGeneration, GS::UniString& error, bool expectGh2)
{
    if (io.joinable ()) {
        error = "The Grasshopper bridge is already listening on " + pipeName + ".";
        return false;
    }

    generation.store (startGeneration);
    startupAcknowledged.store (false);
    connected.store (false);
    workerProcessId.store (0);
    lastHeartbeatTick.store (0);
    stopping.store (false);
    expectedGh2 = expectGh2;
    {
        std::lock_guard<std::mutex> lock (gh2RequestMutex);
        gh2ShutdownRequested = false;
        gh2PendingRequest.reset ();
    }
    {
        std::lock_guard<std::mutex> lock (messageMutex);
        lastWorkerMessage.Clear ();
    }

    // The pid keeps two Archicads apart; the generation keeps this Archicad's
    // successive workers apart, so a worker that was killed cannot reconnect to
    // the pipe its replacement is being given.
    pipeName = GS::UniString::Printf (expectGh2 ? "Tapioca.Gh2.v%u.%u.%u" : "Tapioca.Gh.v%u.%u.%u",
                                      (unsigned int) protocol::Version, (unsigned int) GetCurrentProcessId (),
                                      (unsigned int) startGeneration);
    const GS::UniString fullName = "\\\\.\\pipe\\" + pipeName;
    HANDLE server = CreateServerPipe (fullName);
    if (server == INVALID_HANDLE_VALUE) {
        error = GS::UniString::Printf ("Could not create the Grasshopper bridge pipe (Win32 error %u).",
                                       (unsigned int) GetLastError ());
        pipeName.Clear ();
        return false;
    }
    pipe = (void*) server;

    try {
        io = std::thread ([this] { Run (); });
    }
    catch (...) {
        CloseHandle (server);
        pipe = nullptr;
        pipeName.Clear ();
        error = "Could not create the Grasshopper bridge IO thread.";
        return false;
    }

    LogLine (startGeneration, 0, "bridge listening on \\\\.\\pipe\\" + pipeName);
    return true;
}

void GhBridge::Stop ()
{
    if (expectedGh2)
        BeginGh2Shutdown ();
    if (!io.joinable ()) {
        // A failed Start can leave a pipe with no thread behind it. Closing it
        // unconditionally is what makes Stop safe to call from the quit path.
        std::lock_guard<std::mutex> lock (writeMutex);
        if (pipe != nullptr) {
            CloseHandle ((HANDLE) pipe);
            pipe = nullptr;
        }
        return;
    }

    stopping.store (true);
    // ⚠️ THE HANDLE IS CLOSED ONLY AFTER THE THREAD IS JOINED, AND UNDER THE
    // WRITE MUTEX. Send() may be inside WriteFile on another thread right now;
    // closing under it is the difference between a cancelled write and a write
    // into a recycled handle.
    io.join ();

    std::lock_guard<std::mutex> lock (writeMutex);
    if (pipe != nullptr) {
        DisconnectNamedPipe ((HANDLE) pipe);
        CloseHandle ((HANDLE) pipe);
        pipe = nullptr;
    }
    connected.store (false);
    LogLine (generation.load (), workerProcessId.load (), "bridge stopped");
    workerProcessId.store (0);
    pipeName.Clear ();
}

bool GhBridge::BeginGh2Shutdown (bool abortBootstrap)
{
    std::lock_guard<std::mutex> lock (gh2RequestMutex);
    gh2ShutdownRequested = true;
    const bool pending = gh2PendingRequest != nullptr;
    // A bootstrap can have a Ping still in the pipe, not yet admitted. In
    // either bootstrap case cancel a blocked IO write/read before joining;
    // never hold the request lock across WriteExact. With no Ping on a Running
    // worker, preserve the pipe for Send(Shutdown) and Rhino's orderly exit.
    if (pending || abortBootstrap)
        stopping.store (true);
    gh2PendingRequest.reset ();
    return pending;
}

bool GhBridge::IsGh2RequestCurrent (const std::shared_ptr<Gh2PendingRequest>& request) const
{
    std::lock_guard<std::mutex> lock (gh2RequestMutex);
    return !gh2ShutdownRequested && gh2PendingRequest == request && generation.load () == request->generation;
}

void GhBridge::QueueGh2Reply (const std::shared_ptr<Gh2PendingRequest>& request, std::string envelope)
{
    if (envelope.empty ())
        envelope = ErrorEnvelope ("Archicad returned an empty envelope.");
    if (envelope.size () > MaxGh2ReplyBytes)
        envelope = ErrorEnvelope ("The GH2 project read exceeded 1 MiB.");
    std::lock_guard<std::mutex> lock (gh2RequestMutex);
    if (gh2ShutdownRequested || gh2PendingRequest != request || generation.load () != request->generation ||
        request->replyReady)
        return;
    request->reply.swap (envelope);
    request->queuedTick = (uint64_t) GetTickCount64 ();
    request->replyReady = true;
}

void GhBridge::FlushGh2Reply (void* server, uint32_t currentGeneration)
{
    // Called ONLY on bridge IO after a complete worker message. The worker's
    // independent 3-second heartbeat wakes an otherwise idle read, so a reply
    // queued just after one heartbeat waits up to the next. A stopped heartbeat
    // leaves startup pending until the supervisor or an explicit Stop intervenes.
    std::shared_ptr<Gh2PendingRequest> request;
    std::string envelope;
    {
        std::lock_guard<std::mutex> lock (gh2RequestMutex);
        request = gh2PendingRequest;
        if (gh2ShutdownRequested || request == nullptr || !request->replyReady ||
            request->generation != currentGeneration)
            return;
        envelope.swap (request->reply);
        request->replyReady = false;
    }

    // The request lock is NOT held for either pipe write. Stop seals the token
    // and sets stopping before joining IO; TransferExact cancels a blocked write.
    // A write already issued may finish as Stop begins, but no old-generation
    // reply can enter a replacement pipe: Start follows this IO thread's join.
    {
        std::lock_guard<std::mutex> writeLock (writeMutex);
        if (!stopping.load () && connected.load ()) {
            const std::vector<uint8_t> body = protocol::EncodeTextPayload (envelope);
            const std::vector<uint8_t> responseHeader = protocol::EncodeHeader (
                protocol::MessageType::ApiResponse, 0, request->requestId, (uint32_t) body.size ());
            if (!WriteExact ((HANDLE) server, responseHeader.data (), (DWORD) responseHeader.size (), stopping) ||
                !WriteExact ((HANDLE) server, body.data (), (DWORD) body.size (), stopping))
                stopping.store (true);
        }
    }
    if (!stopping.load ())
        LogLine (currentGeneration, workerProcessId.load (),
                         GS::UniString::Printf ("GH2 read response: bridge IO thread %u, request %u, dispatch %llu ms, "
                                        "queued %llu ms, total %llu ms",
                                        (unsigned int) GetCurrentThreadId (), (unsigned int) request->requestId,
                                        (unsigned long long) (request->queuedTick - request->receivedTick),
                                        (unsigned long long) ((uint64_t) GetTickCount64 () - request->queuedTick),
                                        (unsigned long long) ((uint64_t) GetTickCount64 () - request->receivedTick)));
    std::lock_guard<std::mutex> requestLock (gh2RequestMutex);
    if (gh2PendingRequest == request)
        gh2PendingRequest.reset ();
}

GS::UniString GhBridge::PipeName () const
{
    return pipeName;
}

bool GhBridge::IsConnected () const
{
    return connected.load ();
}

uint32_t GhBridge::WorkerProcessId () const
{
    return workerProcessId.load ();
}

uint64_t GhBridge::MillisecondsSinceHeartbeat () const
{
    const uint64_t last = lastHeartbeatTick.load ();
    if (last == 0)
        return NeverHeardFrom;
    const uint64_t now = (uint64_t) GetTickCount64 ();
    return now > last ? now - last : 0;
}

bool GhBridge::Send (protocol::MessageType type, GS::UniString& error)
{
    std::lock_guard<std::mutex> lock (writeMutex);
    if (pipe == nullptr || !connected.load ()) {
        error = "No Grasshopper worker is connected, so there was nothing to send the "
                "\"" +
                GS::UniString (protocol::DescribeMessageType (type)) + "\" message to.";
        return false;
    }

    const std::vector<uint8_t> header = protocol::EncodeHeader (type, 0, 0, 0);
    if (!WriteExact ((HANDLE) pipe, header.data (), (DWORD) header.size (), stopping)) {
        error = GS::UniString ("The Grasshopper worker did not accept the \"") +
                GS::UniString (protocol::DescribeMessageType (type)) +
                GS::UniString ("\" message. It may be wedged; restarting the worker is the recovery.");
        return false;
    }
    return true;
}

bool GhBridge::SendPayload (protocol::MessageType type, const std::vector<uint8_t>& payload, GS::UniString& error)
{
    if (payload.size () > protocol::MaxPayloadBytes) {
        // Refused before the header is written rather than after: a header
        // announcing a length the writer then cannot honour desynchronises the
        // pipe for every message after it.
        error = GS::UniString ("A Grasshopper \"") + GS::UniString (protocol::DescribeMessageType (type)) +
                GS::UniString ("\" message was too large to send.");
        return false;
    }

    std::lock_guard<std::mutex> lock (writeMutex);
    if (pipe == nullptr || !connected.load ()) {
        error = "No Grasshopper worker is connected, so there was nothing to send the "
                "\"" +
                GS::UniString (protocol::DescribeMessageType (type)) + "\" message to.";
        return false;
    }

    // ⚠️ HEADER AND PAYLOAD UNDER ONE LOCK. Two writers interleaving a header
    // with another message's body is a framing error the reader cannot recover
    // from -- the same rule BridgeClient.WriteMessage keeps on the other end.
    const std::vector<uint8_t> header = protocol::EncodeHeader (type, 0, 0, (uint32_t) payload.size ());
    if (!WriteExact ((HANDLE) pipe, header.data (), (DWORD) header.size (), stopping) ||
        (!payload.empty () && !WriteExact ((HANDLE) pipe, payload.data (), (DWORD) payload.size (), stopping))) {
        error = GS::UniString ("The Grasshopper worker did not accept the \"") +
                GS::UniString (protocol::DescribeMessageType (type)) +
                GS::UniString ("\" message. It may be wedged; restarting the worker is the recovery.");
        return false;
    }
    return true;
}

void GhBridge::SetSessionHandler (std::function<void (protocol::MessageType, const std::vector<uint8_t>&)> handler)
{
    std::lock_guard<std::mutex> lock (messageMutex);
    sessionHandler = std::move (handler);
}

void GhBridge::SetConnectedHandler (std::function<void (uint32_t)> handler)
{
    std::lock_guard<std::mutex> lock (messageMutex);
    connectedHandler = std::move (handler);
}

void GhBridge::SetStartupHandler (std::function<void (uint32_t, protocol::AckStatus, const GS::UniString&)> handler)
{
    std::lock_guard<std::mutex> lock (messageMutex);
    startupHandler = std::move (handler);
}

void GhBridge::SetDisconnectedHandler (std::function<void (uint32_t)> handler)
{
    std::lock_guard<std::mutex> lock (messageMutex);
    disconnectedHandler = std::move (handler);
}

void GhBridge::NotifyConnected ()
{
    std::function<void (uint32_t)> handler;
    {
        // Copied out and called OUTSIDE the lock: the handler talks to the host,
        // which talks back to this bridge, and holding a lock across that is how
        // a supervisor deadlocks against its own transport.
        std::lock_guard<std::mutex> lock (messageMutex);
        handler = connectedHandler;
    }
    if (handler)
        handler (generation.load ());
}

void GhBridge::SetRunResultHandler (std::function<void (const protocol::RunReportPayload&)> handler)
{
    std::lock_guard<std::mutex> lock (messageMutex);
    runResultHandler = std::move (handler);
}

GS::UniString GhBridge::LastWorkerMessage () const
{
    std::lock_guard<std::mutex> lock (messageMutex);
    return lastWorkerMessage;
}

void GhBridge::Run ()
{
    HANDLE server = (HANDLE) pipe;
    const uint32_t gen = generation.load ();

    // ---- Wait for the worker to connect -----------------------------------
    HANDLE connectEvent = CreateEventW (nullptr, TRUE, FALSE, nullptr);
    if (connectEvent == nullptr) {
        LogLine (gen, 0, "bridge could not create its connect event");
        return;
    }

    OVERLAPPED connectOperation {};
    connectOperation.hEvent = connectEvent;
    const BOOL started = ConnectNamedPipe (server, &connectOperation);
    const DWORD connectError = started != 0 ? ERROR_SUCCESS : GetLastError ();
    bool accepted = started != 0 || connectError == ERROR_PIPE_CONNECTED;
    if (!accepted && connectError == ERROR_IO_PENDING) {
        const ULONGLONG deadline = GetTickCount64 () + ConnectTimeoutMs;
        while (!stopping.load () && GetTickCount64 () < deadline) {
            if (WaitForSingleObject (connectEvent, 50) != WAIT_OBJECT_0)
                continue;
            DWORD transferred = 0;
            accepted = GetOverlappedResult (server, &connectOperation, &transferred, FALSE) != 0;
            break;
        }
        if (!accepted) {
            CancelIoEx (server, &connectOperation);
            WaitForSingleObject (connectEvent, INFINITE);
        }
    }
    CloseHandle (connectEvent);

    if (!accepted || stopping.load ()) {
        if (!stopping.load ())
            LogLine (gen, 0, "no Grasshopper worker connected to the bridge before the deadline");
        return;
    }

    // ---- Handshake, before anything else is believed ----------------------
    std::array<uint8_t, protocol::HeaderSize> headerBytes {};
    protocol::Header header;
    std::string protocolError;
    if (!ReadExact (server, headerBytes.data (), (DWORD) headerBytes.size (), stopping) ||
        !protocol::DecodeHeader (headerBytes.data (), headerBytes.size (), header, protocolError) ||
        header.messageType != protocol::MessageType::Hello) {
        const GS::UniString reason =
            protocolError.empty () ? GS::UniString ("the worker did not open with a hello") : FromUtf8 (protocolError);
        LogLine (gen, 0, "bridge handshake refused: " + reason);
        // Said out loud on the wire too. A worker told WHY it was refused can
        // report it in its own window; one that is merely disconnected cannot.
        protocol::HelloAckPayload refusalAck;
        refusalAck.refusal = ToUtf8 (reason);
        const std::vector<uint8_t> payload = protocol::EncodeHelloAckPayload (refusalAck);
        const std::vector<uint8_t> ackHeader =
            protocol::EncodeHeader (protocol::MessageType::HelloAck, 0, header.requestId, (uint32_t) payload.size ());
        std::lock_guard<std::mutex> lock (writeMutex);
        WriteExact (server, ackHeader.data (), (DWORD) ackHeader.size (), stopping);
        WriteExact (server, payload.data (), (DWORD) payload.size (), stopping);
        return;
    }

    std::vector<uint8_t> payload ((size_t) header.payloadBytes);
    if (header.payloadBytes > 0 && !ReadExact (server, payload.data (), header.payloadBytes, stopping))
        return;

    protocol::HelloPayload hello;
    if (!protocol::DecodeHelloPayload (payload.data (), payload.size (), hello, protocolError)) {
        LogLine (gen, 0, "bridge handshake refused: " + FromUtf8 (protocolError));
        return;
    }

    if (!protocol::AcceptsEngine (hello.capabilities, expectedGh2, protocolError)) {
        LogLine (gen, hello.processId, "bridge handshake refused: " + FromUtf8 (protocolError));
        protocol::HelloAckPayload refusalAck;
        refusalAck.refusal = protocolError;
        const std::vector<uint8_t> body = protocol::EncodeHelloAckPayload (refusalAck);
        const std::vector<uint8_t> frame =
            protocol::EncodeHeader (protocol::MessageType::HelloAck, 0, header.requestId, (uint32_t) body.size ());
        std::lock_guard<std::mutex> lock (writeMutex);
        WriteExact (server, frame.data (), (DWORD) frame.size (), stopping);
        WriteExact (server, body.data (), (DWORD) body.size (), stopping);
        return;
    }

    // ⚠️ THE HOST GRANTS; THE WORKER ONLY OFFERS. Preview is available only when
    // the worker asked for it AND this add-on has preview switched on. A worker
    // told no here does not collect, convert or send -- which is what makes
    // "preview off costs nothing" a fact about the handshake rather than a
    // promise about dropping messages after they have already been paid for.
    protocol::HelloAckPayload grantedAck;
    grantedAck.capabilities = hello.capabilities & protocol::CapabilityPreview;
    if (expectedGh2)
        grantedAck.capabilities |= protocol::CapabilityGh2;
    if (!evp::preview::PreviewRuntimeState::Get ().IsEnabled ())
        grantedAck.capabilities &= ~protocol::CapabilityPreview;
    previewIngest.GrantCapabilities (grantedAck.capabilities);

    {
        const std::vector<uint8_t> ackPayload = protocol::EncodeHelloAckPayload (grantedAck);
        const std::vector<uint8_t> ackHeader = protocol::EncodeHeader (protocol::MessageType::HelloAck, 0,
                                                                       header.requestId, (uint32_t) ackPayload.size ());
        std::lock_guard<std::mutex> lock (writeMutex);
        if (!WriteExact (server, ackHeader.data (), (DWORD) ackHeader.size (), stopping) ||
            !WriteExact (server, ackPayload.data (), (DWORD) ackPayload.size (), stopping))
            return;
    }

    workerProcessId.store (hello.processId);
    lastHeartbeatTick.store ((uint64_t) GetTickCount64 ());
    connected.store (true);
    LogLine (gen, hello.processId,
             GS::UniString::Printf ("worker connected, bridge protocol %u", (unsigned int) header.protocolVersion));
    NotifyConnected ();

    // ---- Serve ------------------------------------------------------------
    while (!stopping.load ()) {
        if (!ReadExact (server, headerBytes.data (), (DWORD) headerBytes.size (), stopping))
            break;
        if (!protocol::DecodeHeader (headerBytes.data (), headerBytes.size (), header, protocolError)) {
            // A framing error is unrecoverable by construction: the stream
            // position is no longer known, so there is nothing to resynchronise
            // to. Drop the connection and let the supervisor decide.
            LogLine (gen, hello.processId, "bridge dropped the worker: " + FromUtf8 (protocolError));
            break;
        }

        payload.assign ((size_t) header.payloadBytes, 0);
        if (header.payloadBytes > 0 && !ReadExact (server, payload.data (), header.payloadBytes, stopping))
            break;

        // ⚠️ REFUSED FROM THE MOMENT A STOP BEGINS, not when it finishes. The
        // same rule the lifecycle keeps: teardown is where a late request lands.
        if (stopping.load ())
            break;

        switch (header.messageType) {
            case protocol::MessageType::Heartbeat:
                lastHeartbeatTick.store ((uint64_t) GetTickCount64 ());
                break;

            case protocol::MessageType::Log: {
                std::string line;
                if (protocol::DecodeTextPayload (payload.data (), payload.size (), line, protocolError)) {
                    LogWorkerLine (gen, hello.processId, FromUtf8 (line));
                    if (expectedGh2 && line.rfind ("GH2 Ping:", 0) == 0) {
                        std::lock_guard<std::mutex> lock (messageMutex);
                        lastWorkerMessage = FromUtf8 (line);
                    }
                }
                break;
            }

            case protocol::MessageType::RunResult: {
                protocol::RunReportPayload report;
                if (!protocol::DecodeRunReportPayload (payload.data (), payload.size (), report, protocolError)) {
                    LogLine (gen, hello.processId, "bridge could not read a run report: " + FromUtf8 (protocolError));
                    break;
                }

                LogWorkerLine (gen, hello.processId,
                               GS::UniString::Printf (
                                   "run: %T (%u ms, %u error(s), %u warning(s))",
                                   FromUtf8 (report.headline).ToPrintf (), (unsigned int) report.elapsedMs,
                                   (unsigned int) report.errors.size (), (unsigned int) report.warnings.size ()));
                {
                    std::lock_guard<std::mutex> lock (messageMutex);
                    lastWorkerMessage = FromUtf8 (report.headline);
                }

                std::function<void (const protocol::RunReportPayload&)> handler;
                {
                    // Copied out and called OUTSIDE the lock, for the same reason
                    // NotifyConnected does it: the handler talks to the host,
                    // which talks back to this bridge.
                    std::lock_guard<std::mutex> lock (messageMutex);
                    handler = runResultHandler;
                }
                if (handler)
                    handler (report);
                break;
            }

            case protocol::MessageType::Ack: {
                protocol::AckPayload ack;
                if (!protocol::DecodeAckPayload (payload.data (), payload.size (), ack, protocolError))
                    break;
                const GS::UniString message = FromUtf8 (ack.message);
                {
                    std::lock_guard<std::mutex> lock (messageMutex);
                    lastWorkerMessage = message;
                }
                LogWorkerLine (gen, hello.processId, message);
                if (!startupAcknowledged.exchange (true)) {
                    std::function<void (uint32_t, protocol::AckStatus, const GS::UniString&)> handler;
                    {
                        std::lock_guard<std::mutex> lock (messageMutex);
                        handler = startupHandler;
                    }
                    if (handler)
                        handler (gen, ack.status, message);
                }
                break;
            }

            case protocol::MessageType::ApiRequest: {
                protocol::ApiRequestPayload request;
                std::string envelope;
                if (!protocol::DecodeApiRequestPayload (payload.data (), payload.size (), request, protocolError)) {
                    envelope = ErrorEnvelope (FromUtf8 (protocolError));
                }
                else if (expectedGh2) {
                    // GH2 admits bounded project/selection reads and the
                    // explicit reselect UI action, never model writes.
                    // Post (never Invoke): Stop runs on the same main thread that
                    // would service an Invoke, so IO must remain free to exit.
                    // The gate owns the lambda only until dispatch or
                    // BeginShutdown; a weak token cannot retain a stopped bridge.
                    std::shared_ptr<Gh2PendingRequest> pending;
                    GS::UniString refusal = "command or parameters are outside the GH2 allowlist";
                    {
                        std::lock_guard<std::mutex> lock (gh2RequestMutex);
                        if (gh2ShutdownRequested)
                            refusal = "bridge shutdown has begun";
                        else if (gh2PendingRequest != nullptr)
                            refusal = "another GH2 request is still pending";
                        if (!gh2ShutdownRequested && gh2PendingRequest == nullptr &&
                             ((request.command == "Tapioca.GetGhConnectionInfo" && request.parameters == "{}") ||
                              (request.command == "Tapioca.GetSelection" && request.parameters == "{}") ||
                              (request.command == "Tapioca.SetSelection" && request.parameters.size () <= 200000 &&
                               request.parameters.rfind ("{\"elements\":[", 0) == 0 &&
                               request.parameters.size () >= 13 &&
                               request.parameters.compare (request.parameters.size () - 13, 13,
                                                           ",\"add\":false}") == 0) ||
                             (request.command == "Tapioca.GetStories" && request.parameters == "{}") ||
                             (request.command == "Tapioca.ListAttributes" &&
                              (request.parameters == "{\"kind\":\"layer\"}" ||
                               request.parameters == "{\"kind\":\"lineType\"}")))) {
                            pending = std::make_shared<Gh2PendingRequest> (Gh2PendingRequest {
                                gen, header.requestId, request.command, request.parameters, {}, false,
                                (uint64_t) GetTickCount64 () });
                            gh2PendingRequest = pending;
                        }
                    }
                    if (pending != nullptr) {
                        LogLine (gen, hello.processId,
                                 GS::UniString::Printf ("GH2 read request: bridge IO thread %u, request %u (%T)",
                                                        (unsigned int) GetCurrentThreadId (),
                                                        (unsigned int) header.requestId,
                                                        FromUtf8 (pending->command).ToPrintf ()));
                        GS::UniString postError;
                        const std::weak_ptr<Gh2PendingRequest> weak = pending;
                        if (!MainThreadGate::Get ().Post (
                                [weak] () {
                                    const auto job = weak.lock ();
                                     if (job == nullptr || !GhBridge::Get ().IsGh2RequestCurrent (job))
                                         return;
                                     LogLine (job->generation, GhBridge::Get ().WorkerProcessId (),
                                              GS::UniString::Printf ("GH2 read dispatch: Archicad main thread %u, "
                                                                     "request %u, gate wait %llu ms",
                                                                     (unsigned int) GetCurrentThreadId (),
                                                                     (unsigned int) job->requestId,
                                                                     (unsigned long long) ((uint64_t) GetTickCount64 () -
                                                                                           job->receivedTick)));
                                      // The approved request is dispatched inline by the
                                    // gate's main-thread handler. No bridge state
                                    // or pipe handle is captured by this job.
                                    std::string answer;
                                    try {
                                        answer = ToUtf8 (evp::DispatchApiCall (FromUtf8 (job->command),
                                                                               FromUtf8 (job->parameters), "external"));
                                    }
                                    catch (...) {
                                        answer = ErrorEnvelope ("Archicad could not complete the GH2 Ping.");
                                    }
                                    GhBridge::Get ().QueueGh2Reply (job, std::move (answer));
                                },
                                postError)) {
                            std::lock_guard<std::mutex> lock (gh2RequestMutex);
                            if (gh2PendingRequest == pending)
                                gh2PendingRequest.reset ();
                            envelope = ErrorEnvelope (postError);
                        }
                        else {
                            break; // IO sends the reply on a subsequent worker heartbeat
                        }
                    }
                    else {
                        // No payload here: selection GUIDs and project paths do
                        // not belong in the transport log.
                        LogLine (gen, hello.processId,
                                 "GH2 request refused (" + FromUtf8 (request.command) + "): " + refusal);
                        envelope = ErrorEnvelope ("GH2 request refused: " + refusal);
                    }
                }
                else {
                    // ⚠️ READS ONLY, BY DESIGN, AND THE GATE IS HERE RATHER THAN
                    // IN THE WORKER. HANDOFF §"Supervision is the point":
                    // killing a worker cannot un-commit an Archicad change, so
                    // the first acceptance definition is read-only and writes
                    // are admitted only once execution gating proves one Run
                    // produces one deliberate executor pass. A gate on the
                    // worker's side would be a gate the worker could lose.
                    // ⚠️ THE GATE ASKED THE WRONG QUESTION AND SO NEVER FIRED
                    // ONCE. Commands arrive NAMESPACED -- "Tapioca.SetElementIds",
                    // because the dispatcher requires <Backend>.<Name> and refuses
                    // anything else -- while IsWriteCommand looks a command up by
                    // its BARE name and reports "no such command" for the
                    // namespaced form. The caller reads that as "not a write", so
                    // every write walked through the read-only gate below. The
                    // prefix is stripped before asking, which is the whole fix.
                    //
                    // An unknown bare name still answers false and still reaches
                    // the dispatcher, deliberately: refusing it here would report
                    // "read-only" for a command that is simply misspelled, and the
                    // dispatcher's BadCommand says what is actually wrong.
                    bool isWrite = false;
                    const std::string& fullCommand = request.command;
                    const size_t dot = fullCommand.find ('.');
                    const GS::String commandKey (dot == std::string::npos ? fullCommand.c_str ()
                                                                          : fullCommand.c_str () + dot + 1);
                    try {
                        if (geomsrv::IsWriteCommand (commandKey, isWrite) && isWrite) {
                            envelope = ErrorEnvelope (FromUtf8 (request.command) +
                                                      GS::UniString (" modifies the project, and the Grasshopper "
                                                                     "bridge is read-only in this version."));
                        }
                        else {
                            // ⚠️ THIS RUNS ON THE IO THREAD AND THAT IS THE POINT.
                            // DispatchApiCall marshals to the main thread through
                            // MainThreadGate itself, with a timeout and with undo
                            // ownership where core/CLAUDE.md puts it. Archicad's
                            // IO thread therefore waits on the gate, never the
                            // pipe's caller. Stop must account for this wait.
                            const GS::UniString answer = evp::DispatchApiCall (
                                FromUtf8 (request.command), FromUtf8 (request.parameters), "external");
                            envelope = ToUtf8 (answer);
                            if (envelope.empty ())
                                envelope = ErrorEnvelope ("Archicad returned an empty envelope.");
                        }
                    }
                    catch (...) {
                        LogLine (gen, hello.processId, "bridge API request failed with an exception");
                        envelope = ErrorEnvelope ("Archicad could not complete the API request.");
                    }
                }

                const std::vector<uint8_t> body = protocol::EncodeTextPayload (envelope);
                const std::vector<uint8_t> responseHeader = protocol::EncodeHeader (
                    protocol::MessageType::ApiResponse, 0, header.requestId, (uint32_t) body.size ());
                std::lock_guard<std::mutex> lock (writeMutex);
                if (!WriteExact (server, responseHeader.data (), (DWORD) responseHeader.size (), stopping) ||
                    !WriteExact (server, body.data (), (DWORD) body.size (), stopping)) {
                    stopping.store (true);
                }
                break;
            }

            case protocol::MessageType::PreviewBeginBatch:
            case protocol::MessageType::PreviewAdded:
            case protocol::MessageType::PreviewChanged:
            case protocol::MessageType::PreviewRemoved:
            case protocol::MessageType::PreviewVisibility:
            case protocol::MessageType::PreviewSelection:
            case protocol::MessageType::PreviewEndBatch:
            case protocol::MessageType::PreviewDropAll: {
                // ⚠️ NOTHING HERE TOUCHES ACAPI, THE MAIN THREAD OR THE GPU, AND
                // THAT IS THE WHOLE REASON PREVIEW CAN KEEP UP WITH A SLIDER
                // DRAG. The ingest copies the batch out of the worker's shared
                // memory into this process, publishes an immutable snapshot, and
                // returns what has to be written back. The viewport picks the
                // snapshot up on its own next frame.
                const preview::GhPreviewReply reply =
                    previewIngest.OnMessage (header.messageType, payload.data (), payload.size ());
                if (!reply.log.empty ())
                    LogWorkerLine (gen, hello.processId, FromUtf8 (reply.log));

                // ⚠️ THE ACK IS WHAT RELEASES THE WORKER'S SEGMENT. A host that
                // decided not to send one -- because the batch was refused, say
                // -- would leave the worker holding that memory for as long as it
                // lives, one leak per solve.
                if (reply.sendAck) {
                    const std::vector<uint8_t> body = protocol::EncodePreviewBatchAck (reply.ack);
                    const std::vector<uint8_t> ackHeader =
                        protocol::EncodeHeader (protocol::MessageType::PreviewBatchAck, 0, 0, (uint32_t) body.size ());
                    std::lock_guard<std::mutex> lock (writeMutex);
                    if (!WriteExact (server, ackHeader.data (), (DWORD) ackHeader.size (), stopping) ||
                        !WriteExact (server, body.data (), (DWORD) body.size (), stopping)) {
                        stopping.store (true);
                        break;
                    }
                }
                if (reply.sendResync) {
                    const std::vector<uint8_t> body = protocol::EncodePreviewResyncRequest (reply.resync);
                    const std::vector<uint8_t> resyncHeader = protocol::EncodeHeader (
                        protocol::MessageType::PreviewResyncRequest, 0, 0, (uint32_t) body.size ());
                    std::lock_guard<std::mutex> lock (writeMutex);
                    if (!WriteExact (server, resyncHeader.data (), (DWORD) resyncHeader.size (), stopping) ||
                        !WriteExact (server, body.data (), (DWORD) body.size (), stopping)) {
                        stopping.store (true);
                    }
                }
                break;
            }

            // ---- session, worker -> host ------------------------------
            // Decoded here and handed to the controller, which owns every rule
            // about what a revision means. This arm is transport only: it
            // refuses a payload that will not decode and passes on one that
            // does.
            case protocol::MessageType::SessionEvent:
            case protocol::MessageType::SchemaResult:
            case protocol::MessageType::SolutionStarted:
            case protocol::MessageType::SolutionResult:
            case protocol::MessageType::SolutionFailed:
            case protocol::MessageType::DiagnosticsResult: {
                std::function<void (protocol::MessageType, const std::vector<uint8_t>&)> handler;
                {
                    // Copied out and called OUTSIDE the lock, like every other
                    // handler here: it talks to the controller, which talks back
                    // to this bridge to send the next request.
                    std::lock_guard<std::mutex> lock (messageMutex);
                    handler = sessionHandler;
                }
                if (handler)
                    handler (header.messageType, payload);
                break;
            }

            case protocol::MessageType::Hello:
            case protocol::MessageType::HelloAck:
            case protocol::MessageType::ApiResponse:
            case protocol::MessageType::ShowEditor:
            case protocol::MessageType::HideEditor:
            case protocol::MessageType::Shutdown:
            case protocol::MessageType::RunDefinition:
            case protocol::MessageType::CancelRun:
            case protocol::MessageType::PreviewResyncRequest:
            case protocol::MessageType::PreviewBatchAck:
            case protocol::MessageType::PreviewPicked:
            case protocol::MessageType::OpenSession:
            case protocol::MessageType::CloseSession:
            case protocol::MessageType::SetSessionMode:
            case protocol::MessageType::LoadDefinition:
            case protocol::MessageType::ReloadDefinition:
            case protocol::MessageType::GetSchema:
            case protocol::MessageType::SetInputs:
            case protocol::MessageType::Solve:
            case protocol::MessageType::CancelSolve:
            case protocol::MessageType::GetDiagnostics:
                // Host-to-worker messages, arriving the wrong way. Recorded and
                // ignored rather than acted on: the direction is part of the
                // contract, and a worker that gets it wrong is a worker whose
                // build does not match this one.
                LogLine (gen, hello.processId,
                         GS::UniString ("bridge ignored a \"") +
                             GS::UniString (protocol::DescribeMessageType (header.messageType)) +
                             GS::UniString ("\" message sent in the wrong direction"));
                break;
        }
        if (expectedGh2 && !stopping.load ())
            FlushGh2Reply (server, gen);
    }

    connected.store (false);
    // ⚠️ THE SEGMENT GOES WITH THE WORKER. A mapped view of a dead process's
    // memory is a handle and an address-space reservation this add-on would hold
    // until Archicad quits, and a preview nothing can ever update, remove or
    // explain is worse than no preview at all.
    previewIngest.OnWorkerGone ("the worker disconnected from the bridge");
    LogLine (gen, hello.processId, "worker disconnected from the bridge");
    std::function<void (uint32_t)> handler;
    {
        std::lock_guard<std::mutex> lock (messageMutex);
        handler = disconnectedHandler;
    }
    if (handler)
        handler (gen);
}

} // namespace grasshopper
} // namespace evp
