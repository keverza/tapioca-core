#include "ArchViz/ViewpointStudyController.hpp"
#include "ArchViz/VisibilityStudyCapture.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace geomsrv::archviz::viewpointstudy {
namespace {

class Worker {
  public:
    ~Worker ()
    {
        Shutdown ();
    }

    void Configure (Settings settings)
    {
        std::lock_guard<std::mutex> lock (mutex_);
        const auto previous =
            settings_->enabled && settings.enabled && settings.pointValid && settings_->context == settings.context
                ? status_.result
                : nullptr;
        settings.revision = ++generation_;
        settings_ = std::make_shared<const Settings> (std::move (settings));
        status_ = {};
        status_.result = previous;
        pending_.reset ();
        submittedRevision_ = 0;
        submittedSnapshot_ = 0;
    }

    std::shared_ptr<const Settings> GetSettings ()
    {
        std::lock_guard<std::mutex> lock (mutex_);
        return settings_;
    }

    Status GetStatus ()
    {
        std::lock_guard<std::mutex> lock (mutex_);
        return status_;
    }

    void Tick (std::shared_ptr<const Snapshot> snapshot)
    {
        std::lock_guard<std::mutex> lifecycleLock (lifecycleMutex_);
        std::lock_guard<std::mutex> lock (mutex_);
        if (!settings_->enabled || !settings_->pointValid)
            return;
        if (snapshot == nullptr) {
            if (submittedSnapshot_ != 0) {
                ++generation_;
                pending_.reset ();
                submittedSnapshot_ = 0;
            }
            status_ = {};
            status_.error = "waiting for a complete current viewer capture";
            return;
        }
        if (submittedRevision_ == settings_->revision && submittedSnapshot_ == snapshot->id)
            return;
        submittedRevision_ = settings_->revision;
        submittedSnapshot_ = snapshot->id;
        pending_ = std::move (snapshot);
        pendingGeneration_ = ++generation_;
        const auto previous = status_.result != nullptr && status_.result->snapshotId == pending_->id &&
                                      status_.result->captureStamp == pending_->captureStamp
                                  ? status_.result
                                  : nullptr;
        status_ = {};
        status_.result = previous;
        status_.calculating = true;
        if (!thread_.joinable ()) {
            stopping_ = false;
            thread_ = std::thread ([this] { Run (); });
        }
        cv_.notify_one ();
    }

    void Shutdown ()
    {
        std::lock_guard<std::mutex> lifecycleLock (lifecycleMutex_);
        {
            std::lock_guard<std::mutex> lock (mutex_);
            stopping_ = true;
            ++generation_;
            pending_.reset ();
            status_ = {};
            settings_ = std::make_shared<const Settings> ();
            submittedSnapshot_ = submittedRevision_ = 0;
            cv_.notify_one ();
        }
        if (thread_.joinable ())
            thread_.join ();
    }

  private:
    void Run ()
    {
        for (;;) {
            std::shared_ptr<const Snapshot> snapshot;
            std::shared_ptr<const Settings> settings;
            uint64_t generation;
            {
                std::unique_lock<std::mutex> lock (mutex_);
                cv_.wait (lock, [&] { return stopping_ || pending_ != nullptr; });
                if (stopping_)
                    return;
                snapshot = std::move (pending_);
                settings = settings_;
                generation = pendingGeneration_;
            }
            auto result = evp::sunstudy::RunViewpointStudy (snapshot, settings->context, settings->options,
                                                            [&] { return generation_.load () != generation; });
            std::lock_guard<std::mutex> lock (mutex_);
            if (generation_.load () != generation)
                continue;
            status_.calculating = false;
            if (!visibilitystudy::CaptureIsFresh (snapshot, snapshot->captureStamp)) {
                status_.result.reset ();
                status_.error = "the model changed during viewpoint calculation";
            }
            else if (!result.valid) {
                status_.result.reset ();
                status_.error = result.error;
            }
            else
                status_.result = std::make_shared<const evp::sunstudy::ViewpointStudyResult> (std::move (result));
        }
    }

    std::mutex mutex_, lifecycleMutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool stopping_ = false;
    std::atomic<uint64_t> generation_ { 0 };
    uint64_t submittedRevision_ = 0, submittedSnapshot_ = 0, pendingGeneration_ = 0;
    std::shared_ptr<const Settings> settings_ = std::make_shared<const Settings> ();
    std::shared_ptr<const Snapshot> pending_;
    Status status_;
};

Worker& GetWorker ()
{
    static Worker worker;
    return worker;
}
} // namespace

void Configure (Settings settings)
{
    GetWorker ().Configure (std::move (settings));
}
std::shared_ptr<const Settings> GetSettings ()
{
    return GetWorker ().GetSettings ();
}
void Tick (std::shared_ptr<const Snapshot> snapshot)
{
    GetWorker ().Tick (std::move (snapshot));
}
Status GetStatus ()
{
    return GetWorker ().GetStatus ();
}
void Shutdown ()
{
    GetWorker ().Shutdown ();
}

} // namespace geomsrv::archviz::viewpointstudy
