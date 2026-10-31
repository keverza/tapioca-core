#ifndef EVP_RAYWORKER_RAYWORKERWIN32_HPP
#define EVP_RAYWORKER_RAYWORKERWIN32_HPP

// RayWorker/RayWorkerWin32 -- the platform half of the GPU worker (G1, #43):
// named-pipe framing for the control wire, file-mapping segments for the
// packets, the worker executable's serve loop and the add-on's process host.
//
// Everything that decides what a frame MEANS stays in RayWorkerService and
// RayWorkerClient; this file only moves bytes and reports a dead peer. Both
// executables compile it.
//
// ⚠️ BOTH PIPE ENDS ARE OVERLAPPED. A synchronous handle serialises every I/O
// on it, so the worker's heartbeat thread would block behind the serve loop's
// pending ReadFile and a busy worker would look dead. Each read waits on its
// own event and on a stop event, which is also how a host shutdown unblocks a
// reader without closing a handle under it.

#include "RayWorker/RayWorkerClient.hpp"
#include "RayWorker/RayWorkerService.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace evp::rayworker::win32 {

// Packets in named file mappings, "Local\<prefix>.<name>", each an 8-byte
// little-endian length followed by the bytes. Put keeps the mapping alive by
// holding its handle; the protocol is one run at a time, so only the most
// recent `retained` segments are held (older ones are already consumed).
class MappedSegmentStore final : public SegmentStore {
  public:
    explicit MappedSegmentStore (std::string prefix, size_t retained = 8);
    ~MappedSegmentStore () override;
    MappedSegmentStore (const MappedSegmentStore&) = delete;
    MappedSegmentStore& operator= (const MappedSegmentStore&) = delete;

    bool Put (const std::string& name, std::vector<uint8_t> bytes) override;
    bool Get (const std::string& name, std::vector<uint8_t>& bytes) const override;

  private:
    std::wstring ObjectName (const std::string& name) const;

    std::string prefix_;
    size_t retained_;
    mutable std::mutex mutex_;
    std::deque<std::pair<std::string, void*>> owned_;
};

// A unique segment prefix and pipe name for one host session.
std::string NewSegmentPrefix ();
std::wstring PipeNameFor (const std::string& segmentPrefix);

// Worker executable: --pipe <name> --segments <prefix>.
bool ParseWorkerArgs (int argc, wchar_t** argv, std::wstring& pipe, std::string& segments);

// The worker side: connect to `pipe`, say Hello, serve frames with `backend`
// until Shutdown, a refused handshake or a broken pipe. Heartbeats go out on
// their own thread every `heartbeatMs`, and frames are read on another, so a
// Cancel reaches a Trace in progress. Returns the process exit code.
int RunWorker (const std::wstring& pipe, const std::string& segments, std::unique_ptr<RayBackend> backend,
               uint32_t heartbeatMs = 1000);

// The add-on side: a pipe server, the worker process (in a kill-on-close job
// object) and a reader thread. Frames and the peer's death are queued; the
// owner drains them on its own thread and feeds RayWorkerClient.
class WorkerProcess final {
  public:
    // Starts the worker that will connect to `pipe`: the product spawns an
    // executable; tests run RunWorker on a thread.
    using InProcessWorker = std::function<void (const std::wstring& pipe, const std::string& segments)>;

    // `executable` empty with `inProcess` set runs the worker in this process.
    WorkerProcess (std::wstring executable, InProcessWorker inProcess = {});
    ~WorkerProcess ();
    WorkerProcess (const WorkerProcess&) = delete;
    WorkerProcess& operator= (const WorkerProcess&) = delete;

    bool Launch (std::string& error);
    // Terminate (or disconnect an in-process worker) and join; idempotent.
    void Kill ();
    bool Send (const std::vector<uint8_t>& frame);

    struct Events {
        std::vector<std::vector<uint8_t>> frames;
        bool exited = false; // the peer is gone (process ended or pipe broke)
        std::string exitReason;
    };
    Events Take ();
    // Block until a frame or the peer's death is queued, or `timeoutMs`
    // passes; true when there is something to Take.
    // ⚠️ WAIT HERE, DO NOT SLEEP-POLL. A sleep on Windows lasts a timer tick
    // (15.6 ms unless someone raised the resolution), and the sun study asks
    // once per step: a 4 ms run measured 15 ms of wall time.
    bool Wait (int64_t timeoutMs);

    SegmentStore& Segments ()
    {
        return *segments_;
    }
    // launch/kill/send bound to this host, for RayWorkerClient.
    RayWorkerHooks Hooks ();

  private:
    void ReadLoop ();
    void Exited (const std::string& reason);

    std::wstring executable_;
    InProcessWorker inProcess_;
    std::string prefix_;
    std::unique_ptr<MappedSegmentStore> segments_;
    void* pipe_ = nullptr;
    void* stop_ = nullptr;
    void* process_ = nullptr;
    void* job_ = nullptr;
    std::thread reader_;
    std::thread worker_; // in-process only
    std::mutex writeMutex_;
    std::mutex eventsMutex_;
    std::condition_variable eventsQueued_;
    Events events_;
    bool running_ = false;
};

} // namespace evp::rayworker::win32

#endif
