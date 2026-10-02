#ifndef EVP_SUNSTUDY_SUNSTUDYSELECTIONBINDING_HPP
#define EVP_SUNSTUDY_SUNSTUDYSELECTIONBINDING_HPP

#include "SunStudy/SunStudyRoles.hpp"
#include <cstdint>
#include <set>

namespace evp::sunstudy {

enum class SelectionBindingRefresh { Inactive, Unchanged, Changed, Detached };

// Optional live role source. The palette's declaration generation prevents a
// later command (even with identically named sets) from changing this study.
struct SunStudySelectionBinding {
    std::string contextSet, ignoredSet;
    uint64_t generation = 0;
    uint64_t revision = 0;

    SelectionBindingRefresh Refresh (uint64_t currentGeneration, const std::vector<std::string>& context,
                                     const std::vector<std::string>& ignored, std::vector<std::string>& studyContext,
                                     std::vector<std::string>& studyIgnored)
    {
        if (generation == 0)
            return SelectionBindingRefresh::Inactive;
        if (generation != currentGeneration) {
            *this = {};
            return SelectionBindingRefresh::Detached; // retain the last explicit exclusions
        }
        const auto canonical = [] (const std::vector<std::string>& guids) {
            std::set<std::string> out;
            for (const auto& guid : guids)
                out.insert (CanonicalGuid (guid));
            return out;
        };
        const bool contextChanged = !contextSet.empty () && canonical (context) != canonical (studyContext);
        const bool ignoredChanged = !ignoredSet.empty () && canonical (ignored) != canonical (studyIgnored);
        if (contextChanged)
            studyContext = context; // an empty live set is an intentional removal, not a missing source
        if (ignoredChanged)
            studyIgnored = ignored;
        return contextChanged || ignoredChanged ? SelectionBindingRefresh::Changed : SelectionBindingRefresh::Unchanged;
    }
};

} // namespace evp::sunstudy
#endif
