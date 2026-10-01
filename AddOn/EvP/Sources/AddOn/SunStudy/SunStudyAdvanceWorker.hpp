#ifndef EVP_SUNSTUDY_SUNSTUDYADVANCEWORKER_HPP
#define EVP_SUNSTUDY_SUNSTUDYADVANCEWORKER_HPP

#include "SunStudy/SunStudySession.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace evp::sunstudy {

struct AdvanceRequest {
    std::string studyId;
    uint64_t sessionGeneration = 0;
    uint64_t runGeneration = 0;
    uint64_t recordRevision = 0; // filled by Submit from the store
    size_t maxSteps = 8;
    size_t maxParallel = 1;
    double tmin = 0.001;
    double tmax = 0.0;
    double maxMilliseconds = 0.0;   // checked between complete timesteps, zero = unlimited
    std::function<void ()> onReady; // nonblocking notification after mailbox publication, outside its lock
};

struct AdvanceCompletion {
    AdvanceRequest request;
    StudyProgress progress;
    size_t advanced = 0;
    bool succeeded = false;
    std::string error;
    std::thread::id threadId;
    double wallMilliseconds = 0.0;
    bool priorityLowered = false;
    std::chrono::steady_clock::time_point readyAt;
};

// Single-flight mailbox. Submit/Poll/Cancel never trace or join on their caller.
// Calculation only touches owned study data, never ACAPI. A ready notification
// may post a nonblocking host wake, but must never wait on the main thread.
// Cancellation drains at a timestep boundary and discards the completion.
class SunStudyAdvanceWorker final {
  public:
    SunStudyAdvanceWorker () = default;
    ~SunStudyAdvanceWorker ();
    SunStudyAdvanceWorker (const SunStudyAdvanceWorker&) = delete;
    SunStudyAdvanceWorker& operator= (const SunStudyAdvanceWorker&) = delete;

    bool Submit (AdvanceRequest request, std::string& error);
    bool Poll (AdvanceCompletion& completion);
    bool Busy () const;
    void Cancel ();

    // Join only at add-on quit/unload, never on viewer close. Idempotent;
    // submissions after shutdown are refused, not executed inline.
    void Shutdown ();

  private:
    struct Ticket {
        AdvanceRequest request;
        std::atomic<bool> cancelled { false };
    };
    void ThreadMain ();
    static AdvanceCompletion Execute (const std::shared_ptr<Ticket>& ticket);

    mutable std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::thread thread_;
    std::shared_ptr<Ticket> active_;
    std::shared_ptr<Ticket> pending_;
    AdvanceCompletion completion_;
    bool ready_ = false;
    bool stopping_ = false;
};

} // namespace evp::sunstudy

#endif
