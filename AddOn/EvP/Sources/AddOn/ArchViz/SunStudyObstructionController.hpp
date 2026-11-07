#ifndef EVP_ARCHVIZ_SUNSTUDYOBSTRUCTIONCONTROLLER_HPP
#define EVP_ARCHVIZ_SUNSTUDYOBSTRUCTIONCONTROLLER_HPP

// Single-day exact obstruction requests. Latest selection wins; stale work is
// cancelled and never published. CPU raycasts and drawable construction run on
// the study task worker, while the returned immutable frame is render-ready.

#include "ArchViz/GhPreviewGeometry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
struct Snapshot;
}

namespace geomsrv::archviz {

enum class SunObstructionMode {
    Off,
    TimeStep,
    WholeDay,
};

struct SunObstructionRequest {
    std::string studyId;
    std::vector<size_t> samples; // ordered study-point cells
    SunObstructionMode mode = SunObstructionMode::Off;
    int step = -1; // TimeStep only; WholeDay traces every measured step
};

struct SunObstructionFrame {
    uint64_t generation = 0;
    std::string studyId;
    uint64_t snapshotId = 0;
    uint64_t captureStamp = 0;
    SunObstructionMode mode = SunObstructionMode::Off;
    int minute = -1; // WholeDay: -1; TimeStep: the measured clock minute
    GhPreviewDrawables drawables;
    size_t blockerObjects = 0, timedParts = 0;
    std::string error;
};

class SunObstructionController final {
  public:
    SunObstructionController ();
    ~SunObstructionController ();
    SunObstructionController (const SunObstructionController&) = delete;
    SunObstructionController& operator= (const SunObstructionController&) = delete;

    void Request (const SunObstructionRequest& request);
    std::shared_ptr<const SunObstructionFrame> Latest ();
    bool Busy () const;
    void Cancel (); // project/document transition; the controller remains reusable
    void Shutdown ();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

SunObstructionController& SunObstructionTasks ();
void ShutdownSunObstructionTasks ();
void ResetSunObstructionTasks ();
bool SunObstructionSnapshotCurrent (const SunObstructionFrame& frame,
                                    const std::shared_ptr<const geomsrv::Snapshot>& shared, uint64_t captureStamp);
// A historical study's exact parts must not be drawn over an edited scene.
bool SunObstructionFrameCurrent (const std::shared_ptr<const SunObstructionFrame>& frame,
                                 const std::string& shownStudyId);

} // namespace geomsrv::archviz

#endif
