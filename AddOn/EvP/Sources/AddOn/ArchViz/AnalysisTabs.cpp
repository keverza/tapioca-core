#include "ArchViz/AnalysisTabs.hpp"

#include <algorithm>

namespace geomsrv::archviz::analysistabs {

int TabOf (uint32_t analysisKind, int visibilityOrigin)
{
    if (analysisKind != 1)
        return kSunTab;
    return visibilityOrigin == 1 ? kPointVisibilityTab : kSurfaceVisibilityTab;
}

void MarkPlanting (Focus& focus, const std::string& studyId)
{
    if (studyId.empty ())
        return;
    auto& ids = focus.plantingStudies;
    if (std::find (ids.begin (), ids.end (), studyId) != ids.end ())
        return;
    ids.push_back (studyId);
    if (ids.size () > kPlantingKept)
        ids.erase (ids.begin ());
}

int TabOfStudy (const Focus& focus, const std::string& studyId, uint32_t analysisKind)
{
    if (analysisKind != 1 && !studyId.empty () &&
        std::find (focus.plantingStudies.begin (), focus.plantingStudies.end (), studyId) !=
            focus.plantingStudies.end ())
        return kPlantingTab;
    return TabOf (analysisKind, focus.shownOrigin);
}

void FollowShownStudy (Focus& focus, const std::string& shownStudy, uint32_t analysisKind, uint64_t claim,
                       int visibilityOrigin)
{
    const bool arrived = !shownStudy.empty () && shownStudy != focus.seenStudy;
    focus.seenStudy = shownStudy;
    if (!arrived)
        return;
    // A visibility study put back by its tab is that tab's, whatever ran last.
    if (analysisKind == 1)
        focus.shownOrigin = shownStudy == focus.keptStudy[kPointVisibilityTab]     ? 1
                            : shownStudy == focus.keptStudy[kSurfaceVisibilityTab] ? 0
                                                                                   : visibilityOrigin;
    if (claim == focus.seenClaim)
        return;
    focus.seenClaim = claim;
    Select (focus, TabOfStudy (focus, shownStudy, analysisKind));
}

void Opened (Focus& focus, int tab)
{
    if (focus.selectTab >= 0 && focus.selectTab != tab)
        return;
    focus.tab = tab;
    focus.selectTab = -1;
}

void Select (Focus& focus, int tab)
{
    focus.tab = tab;
    focus.selectTab = tab;
}

int KeepShownStudy (Focus& focus, const std::string& shownStudy, uint32_t analysisKind)
{
    const int shownTab = shownStudy.empty () ? -1 : TabOfStudy (focus, shownStudy, analysisKind);
    if (shownTab >= 0)
        focus.keptStudy[shownTab] = shownStudy;
    else if (focus.lastShownTab >= 0 && focus.lastShownTab == focus.tab)
        focus.keptStudy[focus.tab].clear (); // cleared on its own tab: gone
    focus.lastShownTab = shownTab;
    if (focus.restoreTried != focus.tab)
        focus.restoreTried = -1;
    const int tab = focus.tab;
    if (!IsTintTab (tab) || focus.keptStudy[tab].empty () || shownTab == tab || focus.restoreTried == tab)
        return -1;
    focus.restoreTried = tab;
    return tab;
}

bool ShowsTint (const Focus& focus, const std::string& shownStudy, uint32_t analysisKind)
{
    return shownStudy.empty () || TabOfStudy (focus, shownStudy, analysisKind) == focus.tab;
}

} // namespace geomsrv::archviz::analysistabs
