#include "APIEnvir.h"

#include "DynamoBridge.hpp"
#include "ApiPipeProtocol.hpp"
#include "Python/ApiDispatcher.hpp"

#include <array>
#include <string>
#include <vector>

namespace evp::dynamo {
namespace {

constexpr ULONGLONG ClientTimeoutMs = 5000;

bool TransferExact (HANDLE pipe, void* buffer, DWORD size, bool write, const std::atomic<bool>& stopping)
{
    auto* bytes = (uint8_t*) buffer;
    DWORD offset = 0;
    const ULONGLONG deadline = GetTickCount64 () + ClientTimeoutMs;
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
                if (stopping.load () || GetTickCount64 () >= deadline) {
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
    return TransferExact (pipe, destination, size, false, stopping);
}

bool WriteExact (HANDLE pipe, const void* source, DWORD size, const std::atomic<bool>& stopping)
{
    return TransferExact (pipe, const_cast<void*> (source), size, true, stopping);
}

HANDLE CreateServerPipe (const GS::UniString& fullName)
{
    return CreateNamedPipeW ((LPCWSTR) fullName.ToUStr ().Get (), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024,
                             64 * 1024, 0, nullptr);
}

void ServeClient (HANDLE pipe, const std::atomic<bool>& stopping)
{
    std::array<uint8_t, protocol::HeaderSize> header {};
    if (!ReadExact (pipe, header.data (), (DWORD) header.size (), stopping))
        return;

    protocol::RequestSizes sizes;
    std::string protocolError;
    if (!protocol::DecodeRequestHeader (header.data (), header.size (), sizes, protocolError))
        return;

    std::vector<char> command ((size_t) sizes.commandBytes + 1, '\0');
    std::vector<char> params ((size_t) sizes.paramsBytes + 1, '\0');
    if (!ReadExact (pipe, command.data (), sizes.commandBytes, stopping) ||
        (sizes.paramsBytes > 0 && !ReadExact (pipe, params.data (), sizes.paramsBytes, stopping)))
        return;
    if (std::string (command.data (), sizes.commandBytes).find ('\0') != std::string::npos ||
        std::string (params.data (), sizes.paramsBytes).find ('\0') != std::string::npos)
        return;

    if (!protocol::IsAllowedCommand (std::string_view (command.data (), sizes.commandBytes)))
        return;

    const GS::UniString envelope = evp::DispatchApiCall (GS::UniString (command.data (), CC_UTF8),
                                                         GS::UniString (params.data (), CC_UTF8), "external");
    const auto envelopeUtf8 = envelope.ToCStr (0, MaxUSize, CC_UTF8);
    const std::string envelopeBytes (envelopeUtf8.Get ());
    if (envelopeBytes.empty () || envelopeBytes.size () > protocol::MaxResponseBytes)
        return;

    const std::vector<uint8_t> response = protocol::EncodeResponse (envelopeBytes);
    if (!WriteExact (pipe, response.data (), (DWORD) response.size (), stopping))
        return;

    // DisconnectNamedPipe discards response bytes the client has not consumed.
    // Keep the connection alive until the client confirms it read the full frame;
    // ReadExact retains the same bounded, stop-aware behavior as every other hop.
    uint8_t acknowledgment = 0;
    if (!ReadExact (pipe, &acknowledgment, 1, stopping) || acknowledgment != protocol::ResponseAck)
        return;
}

} // namespace

DynamoBridge& DynamoBridge::Get ()
{
    static DynamoBridge instance;
    return instance;
}

DynamoBridge::~DynamoBridge ()
{
    Stop ();
}

bool DynamoBridge::Start (GS::UniString& error)
{
    if (worker.joinable ()) {
        if (running.load ())
            return true;
        worker.join ();
    }

    pipeName = GS::UniString::Printf ("Tapioca.Dynamo.v1.%u", (unsigned) GetCurrentProcessId ());
    const GS::UniString fullName = "\\\\.\\pipe\\" + pipeName;
    HANDLE firstPipe = CreateServerPipe (fullName);
    if (firstPipe == INVALID_HANDLE_VALUE) {
        error = GS::UniString::Printf ("Could not create the Dynamo bridge pipe (Win32 error %u).",
                                       (unsigned) GetLastError ());
        pipeName.Clear ();
        return false;
    }

    stopping.store (false);
    running.store (true);
    clientProcessId.store (0);
    clientProcessHandle.store (nullptr);
    try {
        worker = std::thread ([this, firstPipe] { Run (firstPipe); });
    }
    catch (...) {
        running.store (false);
        CloseHandle (firstPipe);
        pipeName.Clear ();
        error = "Could not create the Dynamo bridge worker thread.";
        return false;
    }
    return true;
}

void DynamoBridge::Stop ()
{
    if (!worker.joinable ())
        return;

    stopping.store (true);
    worker.join ();
    running.store (false);
    clientProcessId.store (0);
    clientProcessHandle.store (nullptr);
    pipeName.Clear ();
}

void DynamoBridge::SetClientProcess (uint32_t processId, void* processHandle)
{
    clientProcessId.store (processId);
    clientProcessHandle.store (processHandle);
}

GS::UniString DynamoBridge::PipeName () const
{
    return pipeName;
}

void DynamoBridge::Run (void* firstPipe)
{
    const GS::UniString fullName = "\\\\.\\pipe\\" + pipeName;
    HANDLE pipe = (HANDLE) firstPipe;
    while (!stopping.load () && pipe != INVALID_HANDLE_VALUE) {
        HANDLE connectEvent = CreateEventW (nullptr, TRUE, FALSE, nullptr);
        if (connectEvent == nullptr) {
            CloseHandle (pipe);
            return;
        }
        OVERLAPPED connectOperation {};
        connectOperation.hEvent = connectEvent;
        const BOOL started = ConnectNamedPipe (pipe, &connectOperation);
        const DWORD connectError = started != 0 ? ERROR_SUCCESS : GetLastError ();
        bool connected = started != 0 || connectError == ERROR_PIPE_CONNECTED;
        if (!connected && connectError == ERROR_IO_PENDING) {
            while (!stopping.load ()) {
                if (WaitForSingleObject (connectEvent, 50) != WAIT_OBJECT_0)
                    continue;
                DWORD transferred = 0;
                connected = GetOverlappedResult (pipe, &connectOperation, &transferred, FALSE) != 0;
                break;
            }
            if (!connected) {
                CancelIoEx (pipe, &connectOperation);
                WaitForSingleObject (connectEvent, INFINITE);
            }
        }
        CloseHandle (connectEvent);

        if (!stopping.load () && connected) {
            ULONG actualProcessId = 0;
            const uint32_t expectedProcessId = clientProcessId.load ();
            HANDLE expectedProcess = (HANDLE) clientProcessHandle.load ();
            if (expectedProcessId != 0 && expectedProcess != nullptr &&
                WaitForSingleObject (expectedProcess, 0) == WAIT_TIMEOUT &&
                GetNamedPipeClientProcessId (pipe, &actualProcessId) != 0 && actualProcessId == expectedProcessId)
                ServeClient (pipe, stopping);
        }

        DisconnectNamedPipe (pipe);
        CloseHandle (pipe);
        pipe = stopping.load () ? INVALID_HANDLE_VALUE : CreateServerPipe (fullName);
    }
    running.store (false);
}

} // namespace evp::dynamo
