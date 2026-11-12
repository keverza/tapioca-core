// Native inspector service, separated from the shared sun-study presentation.
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/SunStudyOverlay.hpp"
#include "ArchViz/SunStudyObstructionController.hpp"
#include "Geometry/MeshStore.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include <algorithm>

namespace geomsrv::archviz {
namespace {
void ServiceSolarCutReading (HudState& state, const DiligentScene& scene, const float origin[3],
                             const float direction[3])
{
    static uint64_t generation = 0;
    static float lastRay[6] = {};
    if (state.sunInspect == 0 || state.analysisFocus.tab != analysistabs::kSunTab ||
        state.sunView != kSunObstructionView || state.sunObstructionMode != 3 || state.sunShowsPeriod) {
        state.sunCutReadingMinute = -1;
        state.sunCutReadingMinutes.clear ();
        generation = 0;
        return;
    }
    const auto frame = SunObstructionTasks ().Latest ();
    if (!SunObstructionFrameCurrent (frame, scene.ShownSunStudyId ()) || frame->mode != SunObstructionMode::SolarCut ||
        frame->cutStage != SunSolarCutStage::Done || !frame->error.empty () || !frame->exposure.error.empty ()) {
        state.sunCutReadingMinute = -1;
        state.sunCutReadingMinutes.clear ();
        generation = 0;
        return;
    }
    const float ray[6] = { origin[0], origin[1], origin[2], direction[0], direction[1], direction[2] };
    if (generation == frame->generation && std::equal (ray, ray + 6, lastRay))
        return;
    generation = frame->generation;
    std::copy_n (ray, 6, lastRay);
    const double o[3] = { origin[0], origin[1], origin[2] }, d[3] = { direction[0], direction[1], direction[2] };
    state.sunCutReadingMinutes = evp::sunstudy::ReadSolarExposureSteps (frame->exposure, o, d);
    state.sunCutReadingMinute = state.sunCutReadingMinutes.empty () ? -1 : state.sunCutReadingMinutes.front ();
    state.sunCutReadingStepMinutes = frame->timestepMinutes;
}
} // namespace

void ServiceSunStudyInspector (HudState& state, const DiligentScene& scene, const float origin[3],
                               const float direction[3])
{
    ServiceSolarCutReading (state, scene, origin, direction);
    // Re-ask only when the ray, picked object, snapshot or DISPLAYED RESULT
    // changes. A stationary cursor over a converged study used to raycast again
    // every 250 ms forever; a converging study still updates when its new atlas
    // version arrives, without polling the CPU between publications.
    static float lastRay[6] = { 0, 0, 0, 0, 0, 0 };
    static std::string lastStudy;
    static std::string lastPicked;
    static uint64_t lastSnapshot = 0;
    static uint64_t lastResultVersion = 0;
    if (state.sunInspect == 0 || !state.hover.valid) {
        state.sunReadingState = 0;
        state.sunReadingPointValid = false;
        lastStudy.clear (); // re-enabling/re-entering the identical ray must restore the reading
        return;
    }
    const std::string id = scene.ShownSunStudyId ();
    const std::shared_ptr<const Snapshot> snapshot = MeshStore::Get ().Current ();
    // A tint another Analysis tab owns is not drawn, so it has nothing to report.
    if (id.empty () || snapshot == nullptr ||
        !analysistabs::ShowsTint (state.analysisFocus, id, scene.ShownAnalysisKind ())) {
        state.sunReadingState = 0;
        lastStudy.clear ();
        return;
    }
    const std::string picked = evp::sunstudy::CanonicalGuid (state.hover.guid);
    const auto payload = scene.ShownAnalysisPayload ();
    const uint64_t resultVersion = payload != nullptr ? payload->version : 0;
    const float ray[6] = { origin[0], origin[1], origin[2], direction[0], direction[1], direction[2] };
    if (id == lastStudy && picked == lastPicked && snapshot->id == lastSnapshot && resultVersion == lastResultVersion &&
        std::equal (ray, ray + 6, lastRay))
        return;
    std::copy (ray, ray + 6, lastRay);
    lastStudy = id;
    lastPicked = picked;
    lastSnapshot = snapshot->id;
    lastResultVersion = resultVersion;

    state.sunReadingState = 0;
    state.sunReadingPointValid = false;

    // THE GPU PICK DECIDES WHICH ELEMENT IS UNDER THE CURSOR, NOT THE RAY.
    // The snapshot's BVH holds elements the viewer never draws (zone volumes,
    // hidden layers), and the first thing a ray meets can be one of them -- live,
    // the inspector "did not track the model". The pick renders what is ON
    // SCREEN, so the reading is taken from the first hit on the element it
    // names, walking past anything invisible in front.
    const double org[3] = { origin[0], origin[1], origin[2] };
    const double dir[3] = { direction[0], direction[1], direction[2] };
    // Role-partition preparation may never populate QueryIndexCache. Query the
    // picked mesh directly: no whole-model BVH build or capped invisible hits.
    QueryEngine::RayHit hit;
    for (size_t mesh = 0; mesh < snapshot->meshes.size (); ++mesh) {
        if (evp::sunstudy::CanonicalGuid (snapshot->meshes[mesh].guid) == picked) {
            const auto candidate = QueryEngine::RaycastMesh (*snapshot, mesh, org, dir, hit.hit ? hit.t : 1.0e6);
            if (candidate.hit)
                hit = candidate;
        }
    }
    if (!hit.hit) {
        // Said, not hidden: the ray and the picture disagree about this pixel.
        state.sunReadingState = 4;
        return;
    }

    evp::sunstudy::SunStudyReading reading;
    uint8_t role = 0xff;
    double daylight = 0.0;
    std::string error;
    // The sample's lit flags too: the shadow views' hover says WHEN (#116).
    if (!evp::sunstudy::SunStudyStore::Get ().ReadAt (id, snapshot->id, hit.tri, hit.meshIndex, hit.point, reading,
                                                      role, daylight, error, &state.sunReadingLit)) {
        state.sunReadingState = error == "computing" ? 3 : 0;
        lastStudy.clear (); // a transient store refusal must be retried after Advance/cancellation ends
        return;
    }
    state.sunReadingRole = role == 0xff ? -1 : int (role);
    state.sunReadingDaylight = daylight;
    state.sunReadingHours = reading.hours;
    state.sunReadingSample = reading.measured ? reading.sample : SIZE_MAX;
    state.sunReadingStudy = reading.measured ? id : "";
    state.sunReadingNearest = reading.nearest;
    state.sunReadingState = reading.measured ? 1 : 2;
    // Where, for the study point's right-click menu (#152).
    state.sunReadingPointValid = reading.measured;
    std::copy (hit.point, hit.point + 3, state.sunReadingPoint);
}
} // namespace geomsrv::archviz

namespace geomsrv::archviz::viewerhud {
void ServiceAnalysisInteractions (HudState& state, const DiligentScene& scene, const InputSnapshot& input,
                                  const float origin[3], const float direction[3])
{
    ServiceSunStudyInspector (state, scene, origin, direction);
    ServiceSunStudyCentrePick (state, input, origin, direction);
}
} // namespace geomsrv::archviz::viewerhud
