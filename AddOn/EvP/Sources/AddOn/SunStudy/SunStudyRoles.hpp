#ifndef EVP_SUNSTUDY_SUNSTUDYROLES_HPP
#define EVP_SUNSTUDY_SUNSTUDYROLES_HPP

// SunStudy/SunStudyRoles -- which elements a study MEASURES and which only cast
// shadow.
//
// Every element is one material to the analysis; what distinguishes them is the
// role the person gave it:
//
//   * ANALYSIS -- receives samples and casts shadow;
//   * CONTEXT  -- casts shadow, is not measured;
//   * IGNORED  -- neither: absent from the study.
//
// ⚠️ THE DEFAULTS KEEP EVERY STUDY THAT NAMES NOTHING EXACTLY AS IT WAS.
//
//   analysis | context | analysed            | context              | ignored
//   ---------+---------+---------------------+----------------------+--------
//   empty    | empty   | everything          | --                   | --
//   empty    | named   | all but the context | the named ones       | --
//   named    | empty   | the named ones      | everything else      | --
//   named    | named   | the named ones      | the named ones       | the rest
//
// A building is not made transparent by being left out of a picker. Only when
// BOTH lists are named has the person said what the whole study is, and only
// then is anything ignored.
//
// ⚠️ AN ELEMENT NAMED IN BOTH LISTS IS ANALYSIS. It is measured and it casts
// shadow, which is everything context would have given it as well.
//
// ⚠️ AN EXPLICIT IGNORED LIST WINS OVER BOTH, and is applied BEFORE the table.
// It exists to ask "what would this study look like without these?" without
// deleting anything from the model -- so an element named there neither casts
// shadow nor is measured, whatever else it was picked as, and the table above
// then decides the roles of everything that is left.

#include "Geometry/Mesh.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace evp::sunstudy {

enum class ElementRole : uint8_t {
    Analysis = 0,
    Context = 1,
    Ignored = 2,
};

struct ElementRoles {
    // One per element, in the order the caller's element list was given.
    std::vector<ElementRole> roles;

    size_t analysis = 0;
    size_t context = 0;
    size_t ignored = 0;

    // Picked GUIDs that match no element of the snapshot. ⚠️ REPORTED, NOT
    // DROPPED IN SILENCE: a picker holding a deleted wall, or a selection made
    // in another view, would otherwise produce a study of less than was asked
    // for and say nothing.
    size_t unmatchedAnalysis = 0;
    size_t unmatchedContext = 0;
    size_t unmatchedIgnored = 0;

    // True when a caller named an analysis list and none of it is in the
    // snapshot: there is nothing to measure, and a study of nothing is refused
    // rather than reported as "zero hours everywhere".
    bool analysisNamedButAbsent = false;

    // Whether anything is ignored -- i.e. whether the occluders are a SUBSET of
    // the snapshot and need their own traversal.
    bool RestrictsOccluders () const
    {
        return ignored > 0;
    }

    // Per element: 1 when its faces are sampled. The shape the samplers take.
    std::vector<uint8_t> SampleMask () const;
};

// Archicad GUIDs arrive braced or bare and in either case, depending on which
// API produced them. Compared in one canonical form: no braces, upper case.
std::string CanonicalGuid (const std::string& guid);

ElementRoles ResolveElementRoles (const std::vector<std::string>& elements,
                                  const std::vector<std::string>& analysisPicked,
                                  const std::vector<std::string>& contextPicked,
                                  const std::vector<std::string>& ignoredPicked = {});

// The same, over a snapshot's meshes -- roles[m] is meshes[m]'s role.
ElementRoles ResolveElementRoles (const geomsrv::Snapshot& snapshot, const std::vector<std::string>& analysisPicked,
                                  const std::vector<std::string>& contextPicked,
                                  const std::vector<std::string>& ignoredPicked = {});

// The OCCLUDERS: the snapshot without its ignored meshes, or null when nothing
// is ignored (use the snapshot's own cached BVH then).
//
// ⚠️ IT KEEPS THE SNAPSHOT'S ID. It is the same model state, and a study's
// geometry version must not claim to be of a different model.
std::shared_ptr<const geomsrv::Snapshot> OccluderSnapshot (const geomsrv::Snapshot& snapshot,
                                                           const ElementRoles& roles);

} // namespace evp::sunstudy

#endif // EVP_SUNSTUDY_SUNSTUDYROLES_HPP
