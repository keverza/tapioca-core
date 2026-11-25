// Kept planting layers (#278): the finished planting studies the Planting tab
// can show again without re-tracing. Declared in SunStudyHudRunner.hpp (the
// HUD's plain calls) and SunStudyHudPeriod.hpp (the runner's side).
//
// ⚠️ THE REGISTRY OWNS THESE RECORDS. A period release spares a kept carrier
// (SunStudyHudPeriod.cpp), and a planting day is never followed, so nothing
// else erases them; eviction and ForgetPlantingLayers do, by revision.

#include "NativeCommands/SunStudyHudPeriod.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <algorithm>
#include <mutex>
#include <vector>

namespace geomsrv::sunstudyhud {

namespace {

struct KeptLayer {
    std::string id;
    uint64_t revision = 0;
    double hoursMax = 0.0;
};

std::mutex gLayersMutex;
std::vector<KeptLayer> gLayers; // oldest first

} // namespace

void plantinglayers::Keep (const std::string& studyId, uint64_t revision, double hoursMax)
{
    if (studyId.empty ())
        return;
    std::vector<KeptLayer> evicted;
    {
        std::lock_guard<std::mutex> lock (gLayersMutex);
        gLayers.erase (std::remove_if (gLayers.begin (), gLayers.end (),
                                       [&] (const KeptLayer& layer) { return layer.id == studyId; }),
                       gLayers.end ());
        gLayers.push_back ({ studyId, revision, hoursMax });
        while (gLayers.size () > kPlantingLayersKept) {
            evicted.push_back (gLayers.front ());
            gLayers.erase (gLayers.begin ());
        }
    }
    // Outside the registry's lock: the store has its own.
    for (const KeptLayer& layer : evicted)
        evp::sunstudy::SunStudyStore::Get ().Erase (layer.id, layer.revision);
}

bool plantinglayers::Find (const std::string& studyId, uint64_t& revision, double& hoursMax)
{
    std::lock_guard<std::mutex> lock (gLayersMutex);
    for (const KeptLayer& layer : gLayers)
        if (layer.id == studyId) {
            revision = layer.revision;
            hoursMax = layer.hoursMax;
            return true;
        }
    return false;
}

bool IsPlantingLayerKept (const std::string& studyId)
{
    uint64_t revision = 0;
    double hoursMax = 0.0;
    return plantinglayers::Find (studyId, revision, hoursMax);
}

void ForgetPlantingLayers ()
{
    std::vector<KeptLayer> forgotten;
    {
        std::lock_guard<std::mutex> lock (gLayersMutex);
        forgotten.swap (gLayers);
    }
    for (const KeptLayer& layer : forgotten)
        evp::sunstudy::SunStudyStore::Get ().Erase (layer.id, layer.revision);
}

} // namespace geomsrv::sunstudyhud
