#ifndef EVP_ARCHVIZ_VIEWERSTATUS_HPP
#define EVP_ARCHVIZ_VIEWERSTATUS_HPP

// ArchViz/ViewerStatus -- the viewer's status service: what the bottom bar of
// the 3D viewer palette says, decided without DG (issue #96).
//
// Four blocks, in the issue's priority order:
//   view        -- Perspective / Parallel / Plan
//   inspection  -- what is selected
//   analysis    -- the active study, its progress and whether its results still
//                  describe the model; FLEXIBLE, and also where a multi-step
//                  workflow says what to do next ("Next: click the model ...")
//   model       -- whether the viewer's copy of the Archicad model is current
//
// ⚠️ IT SHOWS ONLY WHAT MEANS SOMETHING. An idle viewer with nothing selected and
// no study has empty inspection and analysis blocks -- not "Ready", not "0
// selected". A line that never changes is a line nobody reads.
//
// ⚠️ MODEL FRESHNESS AND ANALYSIS FRESHNESS ARE SEPARATE BLOCKS ON PURPOSE. The
// geometry can be re-extracted and current while a sun study drawn over it still
// describes the building before the edit; one "up to date" flag would have to lie
// about one of them.
//
// ⚠️ PERCENTAGES ARE QUANTISED (kPercentStep). Every change of the composed text
// is a DG SetText, i.e. a paint into the palette that hosts the render surface --
// the flicker ArchVizPanel::RefreshStatus documents. A per-step percentage would
// repaint as often as the study advances.
//
// Two halves:
//   - Compose: Inputs -> Blocks. Pure, no locks, tested offline.
//   - The publish channel: any thread posts a hint or the view facts it alone
//     knows (the render thread owns the HUD's projection); the palette reads them
//     back on idle. Workflows publish ONCE here, whichever surface displays it.

#include <cstddef>
#include <cstdint>
#include <string>

namespace geomsrv::archviz::viewerstatus {

constexpr int kPercentStep = 5;

// The sun follower's state, mirrored so this header needs nothing from
// SunStudy/. Same order as evp::sunstudy::SunStudyFollowState.
enum class FollowState : uint8_t { NoStudy, Current, DirtyVisible, Starting, UpdatingVisible, Failed };

// What the render thread knows and the palette cannot ask: the HUD's projection.
struct ViewFacts {
    bool known = false; // false until the first frame publishes
    bool orthographic = false;
    bool planMode = false;
};

struct Inputs {
    // ---- the renderer ----
    bool drawing = false; // device up and the scene built: the four blocks apply
    ViewFacts view;

    // ---- inspection ----
    uint64_t selectedCount = 0;

    // ---- analysis: the HUD's "Run sun study" (sunstudyhud::Status) ----
    bool runRunning = false;
    std::string runStage; // waiting, snapshot, starting, calculating, showing, done, cancelled, failed
    size_t runResolved = 0;
    size_t runTotal = 0;
    std::string runError;
    // A period run (#152): its overall progress, 0..1, replaces the current
    // day's steps, which restart every day.
    bool runPeriod = false;
    bool runWholeYear = false;
    double runPeriodProgress = 0.0;
    // ---- analysis: the follower that keeps an adopted study current ----
    FollowState follow = FollowState::NoStudy;
    std::string followDirtyReason; // "geometry", "sun", "sampling" or empty
    size_t followResolved = 0;
    size_t followTotal = 0;
    std::string followError;
    // A study is on screen (SunStudyOverlayStatus::studyId non-empty).
    bool studyShown = false;

    // ---- model ----
    bool extractionRunning = false;
    bool extractionGaveUp = false;
    uint32_t extractionDone = 0; // elements visited: extracted + empty
    uint32_t extractionTotal = 0;
    std::string watchError;

    // ---- a multi-step workflow's next step (PublishHint) ----
    std::string hint;
};

struct Blocks {
    std::string view;
    std::string inspection;
    std::string analysis;
    std::string model;
    // The analysis block reports something the user must act on: a failure, or
    // results that no longer describe the model.
    bool analysisAlert = false;
    bool modelAlert = false;
};

// Pure.
Blocks Compose (const Inputs& in);
// The four blocks as one line, empty blocks dropped -- the log's and the tests'
// form, and the fallback for a surface with one text item.
std::string Join (const Blocks& blocks);
// `done` of `total` as a percentage rounded DOWN to kPercentStep; 0 when total is 0.
int QuantisedPercent (size_t done, size_t total);

// ---- the publish channel (any thread) ---------------------------------------
// One hint per `source`, so two workflows cannot erase each other's; empty text
// clears that source. The most recently set hint wins when several are live.
void PublishHint (const std::string& source, const std::string& text);
std::string CurrentHint ();
void PublishView (const ViewFacts& view);
ViewFacts CurrentView ();
// Viewer teardown: a hint must not outlive the workflow that posted it.
void ResetPublished ();

} // namespace geomsrv::archviz::viewerstatus

#endif
