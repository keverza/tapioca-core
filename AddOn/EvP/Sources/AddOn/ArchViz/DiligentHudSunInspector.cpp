// Native inspector service, separated from the shared sun-study presentation.
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "Geometry/MeshStore.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include <algorithm>
#include <chrono>

namespace geomsrv::archviz {
void ServiceSunStudyInspector (HudState& state, const DiligentScene& scene, const float origin[3],
                               const float direction[3])
{
    // THROTTLED TO THE CURSOR. A still cursor over a converged study asks the
    // same question every frame; the ray is compared, and re-asked at most four
    // times a second otherwise, so a study still converging updates under a
    // cursor that is not moving.
    static float lastRay[6] = { 0, 0, 0, 0, 0, 0 };
    static std::string lastStudy;
    static std::string lastPicked;
    static uint64_t lastSnapshot = 0;
    static std::chrono::steady_clock::time_point lastAsked;
    if (state.sunInspect == 0 || !state.hover.valid) {
        state.sunReadingState = 0;
        return;
    }
    const std::string id = scene.ShownSunStudyId ();
    const std::shared_ptr<const Snapshot> snapshot = MeshStore::Get ().Current ();
    // A tint another Analysis tab owns is not drawn, so it has nothing to report.
    if (id.empty () || snapshot == nullptr ||
        !analysistabs::ShowsTint (state.analysisFocus, id, scene.ShownAnalysisKind ())) {
        state.sunReadingState = 0;
        return;
    }
    const std::string picked = evp::sunstudy::CanonicalGuid (state.hover.guid);
    const auto now = std::chrono::steady_clock::now ();
    const float ray[6] = { origin[0], origin[1], origin[2], direction[0], direction[1], direction[2] };
    if (id == lastStudy && picked == lastPicked && snapshot->id == lastSnapshot && std::equal (ray, ray + 6, lastRay) &&
        now - lastAsked < std::chrono::milliseconds (250))
        return;
    std::copy (ray, ray + 6, lastRay);
    lastStudy = id;
    lastPicked = picked;
    lastSnapshot = snapshot->id;
    lastAsked = now;

    state.sunReadingState = 0;

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
    if (!evp::sunstudy::SunStudyStore::Get ().ReadAt (id, snapshot->id, hit.tri, hit.meshIndex, hit.point, reading,
                                                      role, daylight, error)) {
        state.sunReadingState = error == "computing" ? 3 : 0;
        return;
    }
    state.sunReadingRole = role == 0xff ? -1 : int (role);
    state.sunReadingDaylight = daylight;
    state.sunReadingHours = reading.hours;
    state.sunReadingNearest = reading.nearest;
    state.sunReadingState = reading.measured ? 1 : 2;
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
