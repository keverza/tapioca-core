#ifndef TAPIOCA_PIPE_TRANSFER_HPP
#define TAPIOCA_PIPE_TRANSFER_HPP

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>

namespace tapioca::transport {

// A wake services outgoing work without cancelling or losing a partial incoming
// frame. The callback runs on the reader's IO thread, never the publishing thread.
inline bool TransferExact (HANDLE pipe, void* buffer, DWORD size, bool write, const std::atomic<bool>& stopping,
                           ULONGLONG timeoutMs, HANDLE wake = nullptr, const std::function<void ()>& onWake = {})
{
    auto* bytes = static_cast<uint8_t*> (buffer);
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
                if (stopping.load () || (deadline != 0 && GetTickCount64 () >= deadline)) {
                    // Only this operation: concurrent writes own their OVERLAPPED.
                    CancelIoEx (pipe, &operation);
                    WaitForSingleObject (event, INFINITE);
                    CloseHandle (event);
                    return false;
                }
                HANDLE events[] = { event, wake };
                const DWORD waited = WaitForMultipleObjects (wake == nullptr ? 1 : 2, events, FALSE, 50);
                if (waited == WAIT_OBJECT_0)
                    break;
                if (waited == WAIT_OBJECT_0 + 1 && onWake)
                    onWake ();
                if (waited == WAIT_FAILED) {
                    CancelIoEx (pipe, &operation);
                    WaitForSingleObject (event, INFINITE);
                    CloseHandle (event);
                    return false;
                }
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

} // namespace tapioca::transport
#endif
