#ifndef EVP_SUNSTUDY_SUNSTUDYBOUNDS_HPP
#define EVP_SUNSTUDY_SUNSTUDYBOUNDS_HPP

// SunStudy/SunStudyBounds -- the analysis cylinder, a size safeguard.
//
// A vertical cylinder as tall as the scene: elements whose plan footprint
// reaches into it are measured, everything outside only casts shadow. The
// shadow cast INTO the cylinder from outside is kept, so a bounded study is
// the unbounded one restricted to the cylinder, not a different site.
//
// TWO LEVELS. SplitByCylinder picks the elements that reach in (the rest are
// context and are never diced); the sampler then drops every cell whose centre
// is outside the circle (clipX/clipY/clipRadius on StartSunStudy), so a slab
// larger than the cylinder is measured only inside it.

#include <string>
#include <vector>

namespace geomsrv {
struct Snapshot;
}

namespace evp::sunstudy {

struct AnalysisCylinder {
    double x = 0.0, y = 0.0; // plan centre, model metres
    double radius = 0.0;
};

// The scene's axis-aligned bounds; false for a snapshot without vertices.
bool SnapshotBounds (const geomsrv::Snapshot& snapshot, double min[3], double max[3]);

// The role lists StartSunStudy takes, with BOTH named so nothing is left to the
// "everything else" defaults: inside and not picked -> analysis; outside, or
// picked as context -> context. Ignored elements are in neither (the caller
// sends its ignored list as it is). GUIDs are compared canonically and
// returned as the snapshot spells them.
struct BoundedRoles {
    std::vector<std::string> analysis;
    std::vector<std::string> context;
};
BoundedRoles SplitByCylinder (const geomsrv::Snapshot& snapshot, const AnalysisCylinder& cylinder,
                              const std::vector<std::string>& context, const std::vector<std::string>& ignored);

} // namespace evp::sunstudy

#endif
