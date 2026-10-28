#include "ArchViz/AnalysisTabs.hpp"

namespace geomsrv::archviz::analysistabs {

int TabOf (uint32_t analysisKind, int visibilityOrigin)
{
    if (analysisKind != 1)
        return kSunTab;
    return visibilityOrigin == 1 ? kPointVisibilityTab : kSurfaceVisibilityTab;
}

void FollowShownStudy (Focus& focus, const std::string& shownStudy, uint32_t analysisKind, uint64_t claim,
                       int visibilityOrigin)
{
    const bool arrived = !shownStudy.empty () && shownStudy != focus.seenStudy;
    focus.seenStudy = shownStudy;
    if (!arrived)
        return;
    if (analysisKind == 1)
        focus.shownOrigin = visibilityOrigin;
    if (claim == focus.seenClaim)
        return;
    focus.seenClaim = claim;
    Select (focus, TabOf (analysisKind, focus.shownOrigin));
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

bool ShowsTint (const Focus& focus, const std::string& shownStudy, uint32_t analysisKind)
{
    return shownStudy.empty () || TabOf (analysisKind, focus.shownOrigin) == focus.tab;
}

} // namespace geomsrv::archviz::analysistabs
