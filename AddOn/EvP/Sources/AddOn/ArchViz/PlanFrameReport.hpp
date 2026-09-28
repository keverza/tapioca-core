#ifndef EVP_ARCHVIZ_PLANFRAMEREPORT_HPP
#define EVP_ARCHVIZ_PLANFRAMEREPORT_HPP

// ArchViz/PlanFrameReport -- the end of a floor-plan frame record: consecutive
// frames registered against each other (ArchViz/PlanFrameRegistration) and the
// whole record written raw to logs\plan_frames\ as JSON plus one binary file of
// frames. See ArchViz/PlanFrameSession.hpp for what the record is for.
//
// ⚠️ RAW, SO THE VERDICT CAN CHANGE WITHOUT A REBUILD. Nothing here decides which
// ACAPI read describes a frame; the diagnostic does, from these rows, and the same
// rows replay offline against a changed verdict (OVERLAY-INVARIANTS.md §7).
//
// ⚠️ A WORKER THREAD RUNS THIS: no ACAPI, no D3D, no window. Everything it reads
// is handed to it, owned, in `ReportInput`.

#include "ArchViz/Dxgi/PlanFrameRecord.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace planframes {

enum class SampleSource : uint32_t { Entry = 0, Exit = 1, Timer = 2, Present = 3 };

// One ACAPI read of the plan's model-to-pixel map, in the canvas's LOGICAL pixels:
//     x = xx * modelX + xy * modelY + ox,   y = yx * modelX + yy * modelY + oy
struct TransformSample {
    uint64_t serial = 0;
    int64_t qpc = 0;
    SampleSource source = SampleSource::Timer;
    uint32_t message = 0;       // the canvas message it was taken in, 0 for the timer
    uint32_t depth = 0;         // how deeply that message was nested
    uint64_t messageSerial = 0; // which one, by the serial of its entry
    bool valid = false;
    bool torn = false; // the view moved while it was being asked
    uint32_t costUs = 0;
    int32_t error = 0; // ACAPI's error when a read was refused
    double xx = 0.0, xy = 0.0, yx = 0.0, yy = 0.0, ox = 0.0, oy = 0.0;
};

struct ChainRow {
    uint64_t chain = 0;
    uint64_t window = 0;
    uint64_t presents = 0;
    uint32_t width = 0, height = 0, format = 0, swapEffect = 0, bufferCount = 0, flags = 0;
    bool ours = false;
    std::string windowClass;
    std::string relation; // to the plan canvas
};

struct ReportInput {
    std::string reason;
    int64_t qpcFrequency = 0;
    int64_t startQpc = 0;
    uint32_t mainThread = 0;
    double dpi = 1.0;
    uint64_t canvasWindow = 0;
    std::string canvasClass;
    uint32_t canvasWidth = 0, canvasHeight = 0;   // physical
    uint32_t logicalWidth = 0, logicalHeight = 0; // what ACAPI was asked in
    // Where the canvas's client area begins in the target's back buffer, physical.
    bool placementKnown = false;
    int32_t offsetX = 0, offsetY = 0;
    uint32_t targetClientWidth = 0, targetClientHeight = 0;
    ChainRow target; // chain 0: never identified
    std::vector<ChainRow> chains;
    dxgi::planframes::Stats stats;
    uint64_t samplesDropped = 0;
    uint64_t canvasMessages = 0;
    uint64_t retrievedMessages = 0;
    uint32_t idleRedraws = 0;
    std::vector<dxgi::planframes::PresentRecord> presents;
    std::vector<TransformSample> samples;
    std::vector<dxgi::planframes::FrameInfo> frames;
    std::vector<uint8_t> framePixels; // kFrameWidth x kFrameHeight per frame, in order
    std::wstring directory;           // empty: nothing is written
    std::string stamp;
};

struct ReportResult {
    uint32_t pairs = 0;
    uint32_t pairsValid = 0;
    uint64_t analysisMs = 0;
    std::string jsonPath;
    std::string framesPath;
};

ReportResult WriteReport (const ReportInput& input);

} // namespace planframes
} // namespace archviz
} // namespace geomsrv

#endif
