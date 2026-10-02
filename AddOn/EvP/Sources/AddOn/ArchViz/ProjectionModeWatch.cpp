// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.
// ArchViz/ProjectionModeWatch -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/ProjectionModeWatch.hpp"

#include "ArchViz/Dxgi/CameraCensus.hpp"
#include "ArchViz/Dxgi/CameraFreshness.hpp"
#include "ArchViz/Dxgi/CameraRecognizer.hpp"
#include "ArchViz/Dxgi/CameraShape.hpp"
#include "ArchViz/OverlayRuntimeReport.hpp"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace geomsrv {
namespace archviz {
namespace projectionmodewatch {

namespace {

namespace cen = dxgi::census;
namespace freshness = dxgi::injection::freshness;
namespace camerashape = dxgi::camerashape;
namespace report = overlayruntime::report;

// How long after a change the cameras are followed, and how many of each are told.
constexpr uint64_t kFollowMs = 8000;
constexpr uint32_t kCamerasTold = 6;

// `PinTerm` in CameraRecognizer.cpp, in its order: `BindingStats::pinMissed` is indexed by it.
const char* const kPinTerms[] = { "occurrence", "render target", "depth", "viewport", "windows" };
constexpr uint32_t kPinTermCount = sizeof (kPinTerms) / sizeof (kPinTerms[0]);

struct Mode {
    bool read = false;
    bool perspective = false;
    bool twoPoint = false;

    bool operator== (const Mode& other) const
    {
        return read == other.read && perspective == other.perspective && twoPoint == other.twoPoint;
    }
};

const char* NameOf (const Mode& mode)
{
    if (!mode.read)
        return "unread";
    if (!mode.perspective)
        return "parallel";
    return mode.twoPoint ? "two-point perspective" : "perspective";
}

struct Baseline {
    cen::Stats census;
    cen::BindingStats binding;
    cen::FingerprintDiagnosis diagnosis;
    freshness::Report sync;
    uint32_t groupId = 0;
};

Mode g_mode;
API_3DProjectionInfo g_settings = {};
uint32_t g_changes = 0;
uint64_t g_changedAtMs = 0; // 0: not following
Baseline g_before;
// Static: the table is too large for the stack, and only the main thread reads it.
cen::Group g_groupsBefore[cen::kGroupCapacity];
size_t g_groupsBeforeCount = 0;
cen::Group g_groupsNow[cen::kGroupCapacity];
uint64_t g_selectedSerial = 0;
uint64_t g_learningSerial = 0;
uint32_t g_selectedTold = 0;
uint32_t g_learningTold = 0;
double g_readMsMax = 0.0;

std::string Format (const char* format, ...)
{
    char line[512] = {};
    va_list args;
    va_start (args, format);
    _vsnprintf_s (line, sizeof (line), _TRUNCATE, format, args);
    va_end (args);
    return line;
}

std::string SettingsOf (const API_3DProjectionInfo& info)
{
    if (!info.isPersp)
        return "a parallel projection (no camera position)";
    const API_PerspPars& p = info.u.persp;
    const double dx = p.target.x - p.pos.x;
    const double dy = p.target.y - p.pos.y;
    const double dz = p.targetZ - p.cameraZ;
    const double pitch = std::atan2 (dz, std::sqrt (dx * dx + dy * dy)) * 57.29577951308232;
    return Format ("camera (%.3f, %.3f, %.3f) target (%.3f, %.3f, %.3f) -- %.2f deg above the horizon; "
                   "viewCone %.3f rollAngle %.4f distance %.3f azimuth %.4f",
                   p.pos.x, p.pos.y, p.cameraZ, p.target.x, p.target.y, p.targetZ, pitch, p.viewCone, p.rollAngle,
                   p.distance, p.azimuth);
}

std::string Floats (const float m[16])
{
    std::string text;
    for (int i = 0; i < 16; ++i)
        text += Format ("%s%.6g", i == 0 ? "" : (i % 4 == 0 ? " | " : " "), double (m[i]));
    return text;
}

// One camera the census read, as `camerashape::Of` describes it -- and where Archicad's own
// target lands through it, which for any perspective it draws is the middle of the image.
void TellCamera (const char* whose, const freshness::CameraCopy& copy, bool raw)
{
    const camerashape::Shape s = camerashape::Of (copy.view, copy.projection);
    std::string line =
        Format ("%s camera #%llu: %s (b1 a view %s, b0 rotation x projection %s, axes apart %.2e) | "
                "looks %.3f deg above the horizon by b0, %.3f by b1 | off axis x %.5f y %.5f NDC | "
                "field %.3f x %.3f deg | eye (%.3f, %.3f, %.3f) | viewport %.0fx%.0f",
                whose, (unsigned long long) copy.serial, s.decodes ? "DECODES" : "REFUSED", s.view ? "yes" : "NO",
                s.rotationProjection ? "yes" : "NO", s.mismatch, s.pitchB0, s.pitchB1, s.shiftX, s.shiftY, s.fovX,
                s.fovY, s.eye[0], s.eye[1], s.eye[2], double (copy.viewport[2]), double (copy.viewport[3]));
    if (g_settings.isPersp) {
        const API_PerspPars& p = g_settings.u.persp;
        const double target[3] = { p.target.x, p.target.y, p.targetZ };
        double ndc[2] = {};
        line += camerashape::ToNdc (copy.view, copy.projection, target, ndc)
                    ? Format (" | Archicad's target at NDC (%.4f, %.4f)", ndc[0], ndc[1])
                    : std::string (" | Archicad's target is behind this camera");
    }
    report::Say ("PROJECTION", line);
    if (raw) {
        report::Say ("PROJECTION", std::string (whose) + " b1 " + Floats (copy.view));
        report::Say ("PROJECTION", std::string (whose) + " b0 " + Floats (copy.projection));
    }
}

void FollowCameras ()
{
    freshness::CameraCopy copy;
    if (g_selectedTold < kCamerasTold && freshness::LatestCamera (copy) && copy.serial != g_selectedSerial) {
        g_selectedSerial = copy.serial;
        TellCamera ("selected group's", copy, g_selectedTold == 0);
        ++g_selectedTold;
    }
    if (g_learningTold < kCamerasTold && freshness::LearningCamera (copy) && copy.serial != g_learningSerial) {
        g_learningSerial = copy.serial;
        TellCamera ("any group's", copy, g_learningTold == 0);
        ++g_learningTold;
    }
}

Baseline Take ()
{
    Baseline now;
    now.census = cen::GetStats ();
    now.binding = cen::GetBindingStats ();
    now.diagnosis = cen::GetFingerprintDiagnosis ();
    now.sync = freshness::Snapshot ();
    now.groupId = cen::GetSelection ().groupId;
    return now;
}

unsigned long long Delta (uint64_t now, uint64_t before)
{
    return (unsigned long long) (now >= before ? now - before : 0);
}

// ⚠️ DELTAS OVER THE WINDOW, NOT TOTALS (§7): what moved because the mode changed.
void Conclude (uint64_t nowMs)
{
    const Baseline now = Take ();
    const Baseline& was = g_before;
    const double seconds = double (nowMs - g_changedAtMs) / 1000.0;
    report::Say (
        "PROJECTION",
        Format (
            "%.1f s after the change: Archicad drew %llu model frames; the overlay took %llu camera "
            "snapshots; the census read the selected camera %llu times (%llu changed); composition "
            "adopted %llu, and kept an older one while a newer was read on %llu presents; group g%u -> g%u",
            seconds, Delta (now.census.modelFramesSeen, was.census.modelFramesSeen),
            Delta (now.sync.snapshots, was.sync.snapshots), Delta (now.sync.contentDecodes, was.sync.contentDecodes),
            Delta (now.sync.contentChanges, was.sync.contentChanges), Delta (now.sync.camAdopted, was.sync.camAdopted),
            Delta (now.sync.camFreshNotAdopted, was.sync.camFreshNotAdopted), was.groupId, now.groupId));

    std::string pin = Format ("the pin: matched %llu, fingerprint matched %llu, rebound %llu, rebind refused %llu, "
                              "the pinned occurrence held another draw %llu; missed by",
                              Delta (now.binding.selectionMatches, was.binding.selectionMatches),
                              Delta (now.binding.logicalMatches, was.binding.logicalMatches),
                              Delta (now.binding.rebinds, was.binding.rebinds),
                              Delta (now.binding.rebindsRefused, was.binding.rebindsRefused),
                              Delta (now.census.snapshotsOtherDraw, was.census.snapshotsOtherDraw));
    for (uint32_t term = 0; term < kPinTermCount; ++term)
        pin += Format ("%s %s %llu", term == 0 ? "" : ",", kPinTerms[term],
                       Delta (now.binding.pinMissed[term], was.binding.pinMissed[term]));
    std::string sole;
    for (uint32_t term = 0; term < cen::kFingerprintTermCount; ++term) {
        const unsigned long long more = Delta (now.diagnosis.soleMiss[term], was.diagnosis.soleMiss[term]);
        if (more == 0)
            continue;
        const uint32_t* seen = now.diagnosis.observed[term];
        sole += Format (" %s x%llu (the session's first: %u %u %u %u)", cen::FingerprintTermName (term), more, seen[0],
                        seen[1], seen[2], seen[3]);
    }
    pin += "; a draw agreeing on every fingerprint term but one:" + (sole.empty () ? std::string (" none") : sole);
    report::Say ("PROJECTION", pin);

    // The groups that drew meanwhile: did the camera's draw move, or stop being read?
    const size_t count = cen::CopyGroups (g_groupsNow, cen::kGroupCapacity);
    uint32_t drew = 0;
    for (size_t i = 0; i < count; ++i) {
        const cen::Group& group = g_groupsNow[i];
        const cen::Group* before = nullptr;
        for (size_t j = 0; j < g_groupsBeforeCount && before == nullptr; ++j)
            before = g_groupsBefore[j].groupId == group.groupId ? &g_groupsBefore[j] : nullptr;
        const uint64_t frames = before != nullptr ? Delta (group.modelFramesObserved, before->modelFramesObserved)
                                                  : group.modelFramesObserved;
        if (frames == 0)
            continue;
        ++drew;
        const auto was32 = [before] (uint32_t cen::Group::* field) { return before != nullptr ? before->*field : 0u; };
        report::Say ("PROJECTION",
                     Format ("drew: g%u%s occ%u idx%u (camera draw idx%u) frames +%llu | read +%u decoded +%u "
                             "skipped +%u | target %ux%u fmt%u, depth %s%ux%u fmt%u, viewport %.0fx%.0f",
                             group.groupId, group.groupId == now.groupId ? " SELECTED" : "", group.occurrenceIndex,
                             group.lastIndexCount, group.cameraIndexCount, (unsigned long long) frames,
                             group.projectionSamples - was32 (&cen::Group::projectionSamples),
                             group.relativeSamples - was32 (&cen::Group::relativeSamples),
                             group.samplesSkipped - was32 (&cen::Group::samplesSkipped), group.renderTargetWidth,
                             group.renderTargetHeight, group.renderTargetFormat, group.depthPresent ? "" : "none ",
                             group.depthWidth, group.depthHeight, group.depthFormat, double (group.viewportWidth),
                             double (group.viewportHeight)));
    }
    if (drew == 0)
        report::Say ("PROJECTION", Format ("drew: no census group recorded a model frame (%zu groups)", count));
    report::Say ("PROJECTION", Format ("cameras told: selected group's %u, any group's %u (at most %u each); reading "
                                       "the settings cost at most %.3f ms",
                                       g_selectedTold, g_learningTold, kCamerasTold, g_readMsMax));
    g_changedAtMs = 0;
}

void Begin (uint64_t nowMs)
{
    g_changedAtMs = nowMs;
    g_before = Take ();
    g_groupsBeforeCount = cen::CopyGroups (g_groupsBefore, cen::kGroupCapacity);
    g_selectedTold = 0;
    g_learningTold = 0;
    // The cameras read so far are the old mode's; only a newer one is told.
    freshness::CameraCopy copy;
    g_selectedSerial = freshness::LatestCamera (copy) ? copy.serial : 0;
    g_learningSerial = freshness::LearningCamera (copy) ? copy.serial : 0;
}

} // namespace

void Reset ()
{
    g_mode = Mode {};
    g_settings = {};
    g_changes = 0;
    g_changedAtMs = 0;
    g_groupsBeforeCount = 0;
    g_selectedSerial = 0;
    g_learningSerial = 0;
    g_selectedTold = 0;
    g_learningTold = 0;
    g_readMsMax = 0.0;
}

void Tick (bool threeDInFront)
{
    if (!threeDInFront)
        return;
    const uint64_t nowMs = ::GetTickCount64 ();
    API_3DProjectionInfo info = {};
    const auto started = std::chrono::steady_clock::now ();
    const GSErrCode err = ACAPI_View_Get3DProjectionSets (&info);
    const double readMs =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
    g_readMsMax = readMs > g_readMsMax ? readMs : g_readMsMax;
    Mode mode;
    mode.read = err == NoError;
    mode.perspective = mode.read && info.isPersp;
    mode.twoPoint = mode.perspective && info.u.persp.isTwoPointPersp;
    if (mode.read)
        g_settings = info;

    if (!(mode == g_mode)) {
        const bool first = !g_mode.read && g_changes == 0;
        report::Say ("PROJECTION",
                     Format ("%s%s%s (%.3f ms to read)", first ? "the 3D window is in " : "changed: ",
                             first ? "" : (std::string (NameOf (g_mode)) + " -> ").c_str (), NameOf (mode), readMs) +
                         (mode.read ? " -- " + SettingsOf (info) : Format (" -- the read failed (%d)", int (err))));
        g_mode = mode;
        if (!first) {
            ++g_changes;
            if (g_changedAtMs != 0)
                Conclude (nowMs); // the previous change's window, cut short by this one
            Begin (nowMs);
        }
    }
    if (g_changedAtMs == 0)
        return;
    FollowCameras ();
    if (nowMs - g_changedAtMs >= kFollowMs) {
        report::Say ("PROJECTION", "settings now: " + std::string (NameOf (mode)) + " -- " +
                                       (mode.read ? SettingsOf (info) : std::string ("unread")));
        Conclude (nowMs);
    }
}

} // namespace projectionmodewatch
} // namespace archviz
} // namespace geomsrv
