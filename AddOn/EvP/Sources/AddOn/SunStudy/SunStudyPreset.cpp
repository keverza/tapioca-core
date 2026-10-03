#include "SunStudy/SunStudyPreset.hpp"
#include <cmath>

namespace evp::sunstudy {

std::string SunStudyPresetAdvice (size_t triangleCount, const std::string& selectedPreset)
{
    // Conservative bands anchored by the October live runs: 44K triangles
    // stayed light; 818K/1.59M samples took eight minutes on manual CPU.
    if (triangleCount >= 250000 && selectedPreset == "early")
        return "Large model: consider switching to Late model (glass only).";
    if (triangleCount > 0 && triangleCount <= 50000 && selectedPreset == "late")
        return "Small model: consider switching to Early model (all surfaces).";
    return {};
}

SunStudyReceivers BuildSunStudyReceivers (const geomsrv::Snapshot& snapshot, const ElementRoles& roles, bool glassOnly,
                                          const std::map<int32_t, double>& transparency, double threshold)
{
    SunStudyReceivers result;
    result.roles = roles;
    if (roles.roles.size () != snapshot.meshes.size ())
        return result;
    result.faces.resize (snapshot.meshes.size ());
    for (size_t mesh = 0; mesh < snapshot.meshes.size (); ++mesh) {
        const auto& item = snapshot.meshes[mesh];
        if (roles.roles[mesh] == ElementRole::Ignored)
            continue;
        if (roles.roles[mesh] == ElementRole::Context) {
            result.contextFaces += item.TriangleCount ();
            continue;
        }
        if (!glassOnly) {
            result.analysisFaces += item.TriangleCount ();
            continue;
        }
        auto& mask = result.faces[mesh];
        mask.assign (item.TriangleCount (), 0);
        size_t measured = 0;
        for (size_t face = 0; face < item.TriangleCount (); ++face) {
            const auto found =
                face < item.triMaterial.size () ? transparency.find (item.triMaterial[face]) : transparency.end ();
            if (found == transparency.end () || !std::isfinite (found->second)) {
                ++result.unknownMaterialFaces;
                continue; // unknown is NOT glass and must not widen the study
            }
            if (found->second >= threshold) {
                mask[face] = 1;
                ++measured;
            }
        }
        result.analysisFaces += measured;
        result.contextFaces += item.TriangleCount () - measured;
        if (measured == 0) {
            result.roles.roles[mesh] = ElementRole::Context;
            --result.roles.analysis;
            ++result.roles.context;
            mask.clear ();
        }
        else if (measured == item.TriangleCount ())
            mask.clear ();
    }
    return result;
}

} // namespace evp::sunstudy
