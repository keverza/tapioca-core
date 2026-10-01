#ifndef EVP_SUNSTUDY_SUNSTUDYTASKWORKER_HPP
#define EVP_SUNSTUDY_SUNSTUDYTASKWORKER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace evp::sunstudy {

struct StudyTaskRequest {
    uint64_t sessionGeneration = 0;
    uint64_t runGeneration = 0;
    // Owned inputs only, no SDK-affine calls or main-thread rendezvous. The
    // mailbox is the publication boundary for results captured by shared value.
    std::function<void (const std::atomic<bool>&)> execute;
    std::function<void ()> discard;
    std::function<void ()> onReady; // nonblocking wake after publication, outside the mailbox lock
};

struct StudyTaskCompletion {
    uint64_t sessionGeneration = 0;
    uint64_t runGeneration = 0;
    std::string error;
    std::thread::id threadId;
    double wallMilliseconds = 0.0;
    bool priorityLowered = false;
    std::chrono::steady_clock::time_point readyAt;
};

// Single-flight pure-work mailbox. Cancellation/discard (including destruction
// of large orphaned studies) drains on the worker, never on the UI caller.
class SunStudyTaskWorker final {
  public:
    ~SunStudyTaskWorker ();
    bool Submit (StudyTaskRequest request, std::string& error);
    bool Poll (StudyTaskCompletion& completion);
    bool Busy () const;
    void Cancel ();
    void Shutdown ();

  private:
    struct Ticket {
        StudyTaskRequest request;
        std::atomic<bool> cancelled { false };
        bool discarded = false; // worker-only, cleanup runs at most once
    };
    void ThreadMain ();
    static void Discard (const std::shared_ptr<Ticket>& ticket);
    mutable std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::thread thread_;
    std::shared_ptr<Ticket> active_;
    std::shared_ptr<Ticket> pending_;
    StudyTaskCompletion completion_;
    bool ready_ = false;
    bool stopping_ = false;
};

} // namespace evp::sunstudy

#endif
