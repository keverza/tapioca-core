#include "RayWorker/RayWorkerWin32.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace evp::rayworker::win32 {

namespace {

constexpr DWORD kPipeBufferBytes = 1u << 16;
constexpr DWORD kConnectDeadlineMs = 10000;

std::wstring Wide (const std::string& text)
{
    if (text.empty ())
        return {};
    const int size = MultiByteToWideChar (CP_UTF8, 0, text.data (), static_cast<int> (text.size ()), nullptr, 0);
    std::wstring out (static_cast<size_t> (size), L'\0');
    MultiByteToWideChar (CP_UTF8, 0, text.data (), static_cast<int> (text.size ()), out.data (), size);
    return out;
}

std::string Narrow (const std::wstring& text)
{
    if (text.empty ())
        return {};
    const int size =
        WideCharToMultiByte (CP_UTF8, 0, text.data (), static_cast<int> (text.size ()), nullptr, 0, nullptr, nullptr);
    std::string out (static_cast<size_t> (size), '\0');
    WideCharToMultiByte (CP_UTF8, 0, text.data (), static_cast<int> (text.size ()), out.data (), size, nullptr,
                         nullptr);
    return out;
}

std::string LastError (const char* what)
{
    return std::string (what) + " failed (Win32 error " + std::to_string (GetLastError ()) + ")";
}

struct Event {
    HANDLE handle = CreateEventW (nullptr, TRUE, FALSE, nullptr);
    ~Event ()
    {
        if (handle != nullptr)
            CloseHandle (handle);
    }
};

// Read exactly `size` bytes; false when the pipe broke or `stop` was set.
bool ReadExact (HANDLE pipe, HANDLE stop, uint8_t* out, size_t size, std::string& error)
{
    Event event;
    size_t done = 0;
    while (done < size) {
        OVERLAPPED overlapped {};
        overlapped.hEvent = event.handle;
        ResetEvent (event.handle);
        const DWORD chunk = static_cast<DWORD> (std::min<size_t> (size - done, 1u << 20));
        if (!ReadFile (pipe, out + done, chunk, nullptr, &overlapped) && GetLastError () != ERROR_IO_PENDING) {
            error = GetLastError () == ERROR_BROKEN_PIPE ? "the pipe closed" : LastError ("ReadFile");
            return false;
        }
        const HANDLE waits[2] = { event.handle, stop };
        const DWORD signalled = WaitForMultipleObjects (stop != nullptr ? 2 : 1, waits, FALSE, INFINITE);
        DWORD got = 0;
        if (signalled != WAIT_OBJECT_0) {
            CancelIoEx (pipe, &overlapped);
            GetOverlappedResult (pipe, &overlapped, &got, TRUE);
            error = "stopped";
            return false;
        }
        if (!GetOverlappedResult (pipe, &overlapped, &got, FALSE) || got == 0) {
            error = GetLastError () == ERROR_BROKEN_PIPE ? "the pipe closed" : LastError ("ReadFile");
            return false;
        }
        done += got;
    }
    return true;
}

bool WriteAll (HANDLE pipe, const std::vector<uint8_t>& bytes)
{
    Event event;
    size_t done = 0;
    while (done < bytes.size ()) {
        OVERLAPPED overlapped {};
        overlapped.hEvent = event.handle;
        ResetEvent (event.handle);
        DWORD wrote = 0;
        const DWORD chunk = static_cast<DWORD> (std::min<size_t> (bytes.size () - done, 1u << 20));
        if (!WriteFile (pipe, bytes.data () + done, chunk, nullptr, &overlapped) && GetLastError () != ERROR_IO_PENDING)
            return false;
        if (!GetOverlappedResult (pipe, &overlapped, &wrote, TRUE) || wrote == 0)
            return false;
        done += wrote;
    }
    return true;
}

// One frame. A header this side cannot decode is returned on its own (so the
// client refuses it with ProtocolMismatch) and ends the stream: after it,
// nothing says where the next frame starts.
bool ReadFrame (HANDLE pipe, HANDLE stop, std::vector<uint8_t>& frame, std::string& error)
{
    frame.assign (protocol::HeaderSize, 0);
    if (!ReadExact (pipe, stop, frame.data (), frame.size (), error)) {
        frame.clear ();
        return false;
    }
    protocol::Header header;
    if (!protocol::DecodeHeader (frame.data (), frame.size (), header, error))
        return false;
    frame.resize (protocol::HeaderSize + header.payloadBytes);
    if (header.payloadBytes != 0 &&
        !ReadExact (pipe, stop, frame.data () + protocol::HeaderSize, header.payloadBytes, error)) {
        frame.clear ();
        return false;
    }
    return true;
}

} // namespace

// ---- segments ---------------------------------------------------------------

MappedSegmentStore::MappedSegmentStore (std::string prefix, size_t retained)
    : prefix_ (std::move (prefix)), retained_ (std::max<size_t> (retained, 1))
{
}

MappedSegmentStore::~MappedSegmentStore ()
{
    for (auto& owned : owned_)
        CloseHandle (owned.second);
}

std::wstring MappedSegmentStore::ObjectName (const std::string& name) const
{
    return L"Local\\" + Wide (prefix_ + "." + name);
}

bool MappedSegmentStore::Put (const std::string& name, std::vector<uint8_t> bytes)
{
    std::lock_guard<std::mutex> lock (mutex_);
    // A re-Put (the same scene epoch after a worker restart) replaces ours.
    for (auto it = owned_.begin (); it != owned_.end (); ++it)
        if (it->first == name) {
            CloseHandle (it->second);
            owned_.erase (it);
            break;
        }
    const uint64_t total = bytes.size () + 8;
    HANDLE mapping =
        CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD> (total >> 32),
                            static_cast<DWORD> (total), ObjectName (name).c_str ());
    if (mapping == nullptr)
        return false;
    uint8_t* view = static_cast<uint8_t*> (MapViewOfFile (mapping, FILE_MAP_WRITE, 0, 0, 0));
    MEMORY_BASIC_INFORMATION info {};
    if (view == nullptr || VirtualQuery (view, &info, sizeof (info)) == 0 || info.RegionSize < total) {
        // Someone else still holds a smaller mapping of that name.
        if (view != nullptr)
            UnmapViewOfFile (view);
        CloseHandle (mapping);
        return false;
    }
    const uint64_t length = bytes.size ();
    for (int i = 0; i < 8; ++i)
        view[i] = static_cast<uint8_t> (length >> (8 * i));
    if (!bytes.empty ())
        std::memcpy (view + 8, bytes.data (), bytes.size ());
    UnmapViewOfFile (view);
    owned_.emplace_back (name, mapping);
    while (owned_.size () > retained_) {
        CloseHandle (owned_.front ().second);
        owned_.pop_front ();
    }
    return true;
}

bool MappedSegmentStore::Get (const std::string& name, std::vector<uint8_t>& bytes) const
{
    HANDLE mapping = OpenFileMappingW (FILE_MAP_READ, FALSE, ObjectName (name).c_str ());
    if (mapping == nullptr)
        return false;
    bool ok = false;
    const uint8_t* view = static_cast<const uint8_t*> (MapViewOfFile (mapping, FILE_MAP_READ, 0, 0, 0));
    MEMORY_BASIC_INFORMATION info {};
    if (view != nullptr && VirtualQuery (view, &info, sizeof (info)) != 0 && info.RegionSize >= 8) {
        uint64_t length = 0;
        for (int i = 0; i < 8; ++i)
            length |= static_cast<uint64_t> (view[i]) << (8 * i);
        // ⚠️ THE LENGTH IS DATA FROM ANOTHER PROCESS: bound it by the view.
        if (length <= info.RegionSize - 8) {
            bytes.assign (view + 8, view + 8 + length);
            ok = true;
        }
    }
    if (view != nullptr)
        UnmapViewOfFile (view);
    CloseHandle (mapping);
    return ok;
}

std::string NewSegmentPrefix ()
{
    static std::atomic<uint32_t> counter { 0 };
    const auto ticks = std::chrono::steady_clock::now ().time_since_epoch ().count ();
    return "Tapioca.RayWorker." + std::to_string (GetCurrentProcessId ()) + "." + std::to_string (ticks) + "." +
           std::to_string (++counter);
}

std::wstring PipeNameFor (const std::string& segmentPrefix)
{
    return L"\\\\.\\pipe\\" + Wide (segmentPrefix);
}

bool ParseWorkerArgs (int argc, wchar_t** argv, std::wstring& pipe, std::string& segments)
{
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::wstring key = argv[i];
        if (key == L"--pipe")
            pipe = argv[i + 1];
        else if (key == L"--segments")
            segments = Narrow (argv[i + 1]);
        else
            return false;
    }
    return !pipe.empty () && !segments.empty ();
}

// ---- worker side ------------------------------------------------------------

int RunWorker (const std::wstring& pipeName, const std::string& segments, std::unique_ptr<RayBackend> backend,
               uint32_t heartbeatMs)
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (kConnectDeadlineMs);
    while (pipe == INVALID_HANDLE_VALUE) {
        pipe = CreateFileW (pipeName.c_str (), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
            break;
        if (std::chrono::steady_clock::now () > deadline)
            return 2;
        if (GetLastError () == ERROR_PIPE_BUSY)
            WaitNamedPipeW (pipeName.c_str (), 500);
        else
            Sleep (20);
    }

    MappedSegmentStore store (segments);
    RayWorkerService service (std::move (backend), store);
    std::mutex writeMutex;
    const auto send = [&] (const std::vector<uint8_t>& frame) {
        std::lock_guard<std::mutex> lock (writeMutex);
        return WriteAll (pipe, frame);
    };

    int code = 0;
    Event stop;
    std::thread heartbeat;
    if (!send (service.Hello ()))
        code = 3;
    else
        heartbeat = std::thread ([&] {
            while (WaitForSingleObject (stop.handle, heartbeatMs) == WAIT_TIMEOUT)
                if (!send (protocol::EncodeFrame (protocol::MessageType::Heartbeat, 0, 0, {})))
                    break;
        });
    while (code == 0 && !service.ShutdownRequested ()) {
        std::vector<uint8_t> frame;
        std::string error;
        if (!ReadFrame (pipe, nullptr, frame, error)) {
            code = 4; // the host went away: nothing left to serve
            break;
        }
        for (const auto& reply : service.OnFrame (frame))
            if (!send (reply))
                code = 4;
    }
    SetEvent (stop.handle);
    if (heartbeat.joinable ())
        heartbeat.join ();
    FlushFileBuffers (pipe);
    CloseHandle (pipe);
    return code;
}

// ---- host side --------------------------------------------------------------

WorkerProcess::WorkerProcess (std::wstring executable, InProcessWorker inProcess)
    : executable_ (std::move (executable)), inProcess_ (std::move (inProcess)), prefix_ (NewSegmentPrefix ()),
      segments_ (std::make_unique<MappedSegmentStore> (prefix_))
{
}

WorkerProcess::~WorkerProcess ()
{
    Kill ();
}

bool WorkerProcess::Launch (std::string& error)
{
    Kill ();
    static std::atomic<uint32_t> launches { 0 };
    const std::string channel = prefix_ + ".pipe" + std::to_string (++launches);
    const std::wstring pipeName = PipeNameFor (channel);
    pipe_ =
        CreateNamedPipeW (pipeName.c_str (), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                          kPipeBufferBytes, kPipeBufferBytes, 0, nullptr);
    if (pipe_ == INVALID_HANDLE_VALUE) {
        pipe_ = nullptr;
        error = LastError ("CreateNamedPipe");
        return false;
    }
    stop_ = CreateEventW (nullptr, TRUE, FALSE, nullptr);
    {
        std::lock_guard<std::mutex> lock (eventsMutex_);
        events_ = {};
    }

    if (!executable_.empty ()) {
        if (GetFileAttributesW (executable_.c_str ()) == INVALID_FILE_ATTRIBUTES) {
            error = "the worker executable is missing: " + Narrow (executable_);
            Kill ();
            return false;
        }
        // ⚠️ A KILL-ON-CLOSE JOB: if Archicad dies, the worker dies with it
        // instead of holding the GPU and the pipe name.
        job_ = CreateJobObjectW (nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        if (job_ != nullptr)
            SetInformationJobObject (job_, JobObjectExtendedLimitInformation, &limits, sizeof (limits));
        std::wstring command = L"\"" + executable_ + L"\" --pipe " + pipeName + L" --segments " + Wide (prefix_);
        STARTUPINFOW startup {};
        startup.cb = sizeof (startup);
        PROCESS_INFORMATION info {};
        if (!CreateProcessW (executable_.c_str (), command.data (), nullptr, nullptr, FALSE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
            error = LastError ("CreateProcess");
            Kill ();
            return false;
        }
        if (job_ != nullptr)
            AssignProcessToJobObject (job_, info.hProcess);
        ResumeThread (info.hThread);
        CloseHandle (info.hThread);
        process_ = info.hProcess;
    }
    else if (inProcess_) {
        worker_ = std::thread ([worker = inProcess_, pipeName, prefix = prefix_] { worker (pipeName, prefix); });
    }
    else {
        error = "no worker to launch";
        Kill ();
        return false;
    }
    running_ = true;
    reader_ = std::thread ([this] { ReadLoop (); });
    return true;
}

void WorkerProcess::Exited (const std::string& reason)
{
    std::lock_guard<std::mutex> lock (eventsMutex_);
    if (!events_.exited) {
        events_.exited = true;
        events_.exitReason = reason;
    }
}

void WorkerProcess::ReadLoop ()
{
    Event connected;
    OVERLAPPED overlapped {};
    overlapped.hEvent = connected.handle;
    if (!ConnectNamedPipe (pipe_, &overlapped)) {
        const DWORD status = GetLastError ();
        if (status == ERROR_IO_PENDING) {
            const HANDLE waits[3] = { connected.handle, stop_, process_ };
            const DWORD signalled = WaitForMultipleObjects (process_ != nullptr ? 3 : 2, waits, FALSE, INFINITE);
            if (signalled != WAIT_OBJECT_0) {
                CancelIoEx (pipe_, &overlapped);
                DWORD ignored = 0;
                GetOverlappedResult (pipe_, &overlapped, &ignored, TRUE);
                if (signalled == WAIT_OBJECT_0 + 2)
                    Exited ("the worker exited before it connected");
                return;
            }
        }
        else if (status != ERROR_PIPE_CONNECTED) {
            Exited (LastError ("ConnectNamedPipe"));
            return;
        }
    }
    for (;;) {
        std::vector<uint8_t> frame;
        std::string error;
        const bool ok = ReadFrame (pipe_, stop_, frame, error);
        if (WaitForSingleObject (stop_, 0) == WAIT_OBJECT_0)
            return; // our own Kill, not the worker's death
        std::lock_guard<std::mutex> lock (eventsMutex_);
        if (ok || frame.size () == protocol::HeaderSize)
            events_.frames.push_back (std::move (frame));
        if (!ok) {
            events_.exited = true;
            events_.exitReason = error;
            return;
        }
    }
}

void WorkerProcess::Kill ()
{
    if (stop_ != nullptr)
        SetEvent (stop_);
    if (job_ != nullptr)
        TerminateJobObject (job_, 1);
    else if (process_ != nullptr)
        TerminateProcess (process_, 1);
    if (process_ != nullptr)
        WaitForSingleObject (process_, 5000);
    if (reader_.joinable ())
        reader_.join ();
    if (pipe_ != nullptr) {
        DisconnectNamedPipe (pipe_); // an in-process worker sees a broken pipe and returns
        CloseHandle (pipe_);
        pipe_ = nullptr;
    }
    if (worker_.joinable ())
        worker_.join ();
    for (void** handle : { &process_, &job_, &stop_ })
        if (*handle != nullptr) {
            CloseHandle (*handle);
            *handle = nullptr;
        }
    running_ = false;
}

bool WorkerProcess::Send (const std::vector<uint8_t>& frame)
{
    std::lock_guard<std::mutex> lock (writeMutex_);
    return running_ && pipe_ != nullptr && WriteAll (pipe_, frame);
}

WorkerProcess::Events WorkerProcess::Take ()
{
    std::lock_guard<std::mutex> lock (eventsMutex_);
    Events out;
    std::swap (out, events_);
    return out;
}

RayWorkerHooks WorkerProcess::Hooks ()
{
    RayWorkerHooks hooks;
    hooks.launch = [this] {
        std::string error;
        return Launch (error);
    };
    hooks.kill = [this] { Kill (); };
    hooks.send = [this] (std::vector<uint8_t> frame) { Send (frame); };
    return hooks;
}

} // namespace evp::rayworker::win32
