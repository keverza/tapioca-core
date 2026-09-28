#ifndef EVP_ARCHVIZ_PLANOVERLAYRUNTIME_HPP
#define EVP_ARCHVIZ_PLANOVERLAYRUNTIME_HPP

// ArchViz/PlanOverlayRuntime -- the floor-plan overlay's session: Archicad's own wall
// outlines, drawn into the plan's back buffer at its Present with the transform read at
// that Present. Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md.
//
// ⚠️ THE PLAN'S OWN SESSION, NEVER THE 3D ONE'S (§12). It shares the Present hook and
// state restoration with the 3D overlay -- where and when -- and nothing else: no camera
// recognition, no census, no depth, no host extraction. The controller never runs the
// two at once, and a failure of either leaves the other alone.
//
// ⚠️ IT OWNS WHAT IT INSTALLS AND NOTHING ELSE, AND STOP TAKES ALL OF IT BACK (§8). The
// Present hook is removed only if this session installed it; the layer, the timer, the
// crash-loop breadcrumb and every D3D object it built go with every Stop, and every
// Start resets what the last one left.
//
// ⚠️ CONTENT IS THE STOREY'S WALLS, READ AT START AND AGAIN WHEN THE STOREY CHANGES.
// Edits to the model are NOT followed yet: the overlay shows the walls as they were
// when it started, until it is started again -- and its log says so, rather than
// letting a stale outline read as a sync fault.
//
// MAIN THREAD, every entry point.

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace planruntime {

// The plan's own 2D geometry for the storey being drawn: closed rings of model metres
// (x, y, x, y, ...) and one signed arc angle per vertex. MAIN THREAD, ACAPI. Registered
// by the add-on at init: the reader lives with the plan geometry commands, which this
// module may not include (a sideways feature include).
using ContentReader = bool (*) (std::vector<std::vector<double>>& rings, std::vector<std::vector<double>>& arcs,
                                std::string& error);
void SetContentReader (ContentReader reader);

enum class StartError : uint32_t {
    None = 0,
    AlreadyRunning,
    NotFloorPlan,
    RecordRunning, // the plan frame record measures Archicad's frames, not ours
    Blocked,       // the crash-loop guard, or its breadcrumb could not be written
    NoCanvas,
    CanvasSize,
    NoContentReader,
    Content,
    PresentHook,
    Timer,
};
const char* StartErrorName (StartError error);

struct StartResult {
    bool ok = false;
    StartError code = StartError::None;
    std::string message;
};

StartResult Start ();

// Everything back, then one redraw while the plan is in front, so the overlay leaves
// the screen instead of waiting for the next pan.
void Stop (const char* reason);

// The same, with no ACAPI: project events and the add-on's unload.
void Shutdown ();

bool Running ();

struct Status {
    bool running = false;
    std::string canvasClass;
    uint32_t canvasWidth = 0;
    uint32_t canvasHeight = 0;
    double dpi = 1.0;
    int32_t storey = 0;
    uint32_t rings = 0;
    uint32_t segments = 0;
    uint64_t generation = 0;
    uint64_t canvasPresents = 0; // cumulative since Start
    uint64_t drawn = 0;
    uint64_t readsFresh = 0;
    uint64_t drawnWithLastRead = 0;
    std::string lastError;
};
Status GetStatus ();

} // namespace planruntime
} // namespace archviz
} // namespace geomsrv

#endif
