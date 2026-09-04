#ifndef EVP_SUNSTUDY_SUNSTUDYSTORE_HPP
#define EVP_SUNSTUDY_SUNSTUDYSTORE_HPP

// SunStudy/SunStudyStore — the live studies, addressed by id.
//
// A study outlives the call that starts it: that is the whole point of a session
// that is advanced rather than awaited. Something has to hold it between calls,
// and this is that thing.
//
// ⚠️ IT OWNS THE SAMPLE ARRAYS, AND THAT IS NOT INCIDENTAL. SampleSet is
// non-owning by design, because the analysis must be able to run over a caller's
// buffers without copying them. But a study advanced across many separate bus
// calls has no caller stack to borrow from — the arrays would be freed the
// moment the starting call returned, and every later slice would read released
// memory. So the store copies them once and hands out a SampleSet pointing at
// its own copy.
//
// ⚠️ EVERY ACCESS IS UNDER THE LOCK BECAUSE THE CALLERS ARE ON DIFFERENT
// THREADS. Starting a study needs the host's main thread (the sun comes from
// Archicad); advancing it deliberately does not, so that the work stays off that
// thread. Those are different threads by design, not by accident.
//
// ⚠️ THE LOCK IS NOT HELD ACROSS THE ANALYSIS ITSELF. Advancing a study is the
// expensive part; holding the store's mutex for it would serialise every other
// caller behind it, including a cheap progress poll from the UI. `Advance`
// therefore takes the lock, finds the session, and releases it — see the note on
// the method.

#include "Geometry/QueryEngine.hpp"
#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudySession.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace evp::sunstudy {

// Everything one live study holds. Addressed only through the store.
struct StudyRecord {
    std::string id;

    // Owned copies; see the header note on why these are not borrowed.
    std::vector<double> positions;
    std::vector<double> normals;

    SunSeries series;
    SunStudySession session;
    std::shared_ptr<CpuTraversal> traversal;

    // Reported back so a caller can say what it studied without holding the
    // parameters itself.
    int timestepMinutes = 60;
    int year = 0, month = 0, day = 0;
    int hourFrom = 0, hourTo = 24;
    double minAltitudeDegrees = 0.0;
    double gridSpacing = 0.0;
    double groundPad = 0.0;
    size_t sourceStepCount = 0;

    // Wall-clock milliseconds spent inside Advance, summed. The measurement the
    // whole backend decision rests on, kept where a live run can read it.
    double analysisMilliseconds = 0.0;

    SampleSet Samples () const;
};

class SunStudyStore final {
  public:
    static SunStudyStore& Get ();

    // Takes ownership of `record`'s buffers and returns the id it was filed
    // under. An empty `record.id` gets a generated one.
    std::string Insert (std::unique_ptr<StudyRecord> record);

    // ⚠️ ADVANCING DOES NOT HOLD THE STORE'S LOCK. It takes the lock only to
    // resolve the id to a record, then releases it and works on the record
    // itself. Two callers advancing the SAME study concurrently is the one thing
    // that would race, and it is prevented by a per-record flag rather than by
    // serialising every unrelated caller behind the slow path.
    //
    // Returns false when the id is unknown or another thread is already
    // advancing that study; `error` says which.
    bool Advance (const std::string& id, size_t maxSteps, size_t maxParallel, double tmin, double tmax,
                  size_t& advanced, std::string& error);

    // A snapshot of one study's progress and parameters. False when unknown.
    bool Progress (const std::string& id, StudyProgress& progress, std::string& error) const;

    // Read under the lock and copy out, so a caller never holds a pointer into
    // a study another thread may erase.
    bool SunHours (const std::string& id, std::vector<double>& hours, std::vector<double>& positions,
                   std::string& error) const;

    // Everything a consumer needs to draw or diff a study, read under one lock
    // so the four arrays cannot come from different generations.
    //
    // ⚠️ `stepBits` IS THE REASON THE OTHER ENGINE CAN BE DIFFED AT ALL. Hours
    // alone answer "how much"; a per-sample-per-step bit answers "which steps",
    // which is what a sample-by-sample cross-check compares and what the
    // viewer's single-instant and AM/PM modes read. One byte per (sample, step)
    // on the wire, so it is opt-in.
    bool Results (const std::string& id, std::vector<double>& hours, std::vector<double>& positions,
                  std::vector<double>& normals, std::vector<uint8_t>* stepBits, std::string& error) const;

    bool Describe (const std::string& id, StudyRecord& copyOfMetadata, std::string& error) const;

    bool Erase (const std::string& id);
    void Clear ();

    std::vector<std::string> Ids () const;
    size_t Count () const;

  private:
    SunStudyStore () = default;

    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<StudyRecord>> studies_;
    std::map<std::string, bool> advancing_;
    uint64_t nextId_ = 1;
};

} // namespace evp::sunstudy

#endif
