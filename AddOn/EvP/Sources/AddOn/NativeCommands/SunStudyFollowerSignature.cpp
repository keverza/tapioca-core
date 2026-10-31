#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SunStudyFollowerSignature.hpp"

#include "NativeCommands/SunStudyCommandsSupport.hpp" // sunstudysupport::PlaceInputHash
#include "SunStudy/SunStudyRoles.hpp"                 // CanonicalGuid
#include "Geometry/MeshStore.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace geomsrv {
namespace sunfollow {

using evp::sunstudy::SunStudyDependencySignature;

namespace {

void Mix (uint64_t& hash, uint64_t value)
{
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xffull;
        hash *= 1099511628211ull;
    }
}

// A picked element list, order- and spelling-independent: the same elements
// picked in another order, or braced differently, are the same study.
void MixGuids (uint64_t& hash, const std::vector<std::string>& guids)
{
    std::vector<std::string> canonical;
    canonical.reserve (guids.size ());
    for (const std::string& guid : guids)
        canonical.push_back (evp::sunstudy::CanonicalGuid (guid));
    std::sort (canonical.begin (), canonical.end ());
    canonical.erase (std::unique (canonical.begin (), canonical.end ()), canonical.end ());
    Mix (hash, canonical.size ());
    for (const std::string& guid : canonical) {
        for (const char c : guid)
            Mix (hash, static_cast<uint8_t> (c));
        Mix (hash, 0xffull); // separator: {AB,C} is not {A,BC}
    }
}

void MixDouble (uint64_t& hash, double value)
{
    uint64_t bits = 0;
    static_assert (sizeof (bits) == sizeof (value), "a double is eight bytes");
    std::memcpy (&bits, &value, sizeof (bits));
    Mix (hash, bits);
}

// The sun the study would resolve, as a hash of the INPUTS that decide it.
//
// ⚠️ THE INPUTS, NOT SunSeries::Version(). That version hashes the resolved step
// LIST, and resolving it costs one ACAPI_GeoLocation_CalcSunOnPlace per timestep
// -- forty-nine host calls to answer "has the sun changed?" five times a second.
// These are exactly the values StartSunStudy feeds into that loop, taken from
// the same API_PlaceInfo it reads, so the two cannot disagree about what a
// different sun is.
//
// ⚠️ AND NOTHING FROM THE 3D WINDOW'S PROJECTION IS IN IT. Archicad raises its
// environment notification on every orbit; a signature that included the
// viewer's sun vector would make navigation restart the analysis, which is the
// 37-rebuild bug wearing a different hat.
uint64_t SunInputHash (const API_PlaceInfo& place, const ActiveSunStudyConfig& config)
{
    uint64_t hash = sunstudysupport::PlaceInputHash (place);
    Mix (hash, static_cast<uint64_t> (config.year));
    Mix (hash, static_cast<uint64_t> (config.month));
    Mix (hash, static_cast<uint64_t> (config.day));
    Mix (hash, static_cast<uint64_t> (config.timestep));
    Mix (hash, static_cast<uint64_t> (config.hourFrom));
    Mix (hash, static_cast<uint64_t> (config.hourTo));
    MixDouble (hash, config.minAltitudeDeg);
    return hash;
}

uint64_t SamplingHash (const ActiveSunStudyConfig& config)
{
    uint64_t hash = 1469598103934665603ull;
    MixDouble (hash, config.grid);
    // The domain dices the surfaces differently, so it IS a sampling input.
    Mix (hash, config.patchDomain ? 1ull : 0ull);
    Mix (hash, config.preset == "late" ? 1ull : 0ull);
    MixDouble (hash, config.glassThreshold);
    Mix (hash, config.analysisRestricted ? 1ull : 0ull);
    MixDouble (hash, config.clipX);
    MixDouble (hash, config.clipY);
    MixDouble (hash, config.clipRadius);
    // Which elements are measured and which only cast shadow are sampling
    // inputs too: re-picking them must re-measure.
    MixGuids (hash, config.analysisElements);
    MixGuids (hash, config.contextElements);
    MixGuids (hash, config.ignoredElements);
    // ⚠️ THE DISPLAY MODE IS DELIBERATELY ABSENT. Switching from `hours` to
    // `cell checker` changes no measurement; including it would recompute a
    // whole study to change a colour, which is precisely what the atlas design
    // exists to avoid.
    return hash;
}

} // namespace

SunStudyDependencySignature BuildCurrentSignature (const ActiveSunStudyConfig& config)
{
    SunStudyDependencySignature signature;

    const std::shared_ptr<const Snapshot> snapshot = MeshStore::Get ().Current ();
    signature.geometry = snapshot != nullptr ? snapshot->id : 0;

    API_PlaceInfo place = {};
    if (ACAPI_GeoLocation_GetPlaceSets (&place) == NoError)
        signature.sun = SunInputHash (place, config);
    signature.sampling = SamplingHash (config);
    return signature;
}

} // namespace sunfollow
} // namespace geomsrv
