#ifndef EVP_ARCHVIZ_ANALYSISTABS_HPP
#define EVP_ARCHVIZ_ANALYSISTABS_HPP

// ArchViz/AnalysisTabs -- which Analysis tab is in focus, and so which result the
// viewer shows. The sun study and both visibility studies share ONE tint channel
// (SceneCmdQueue's analysis display); the viewpoint isovist is drawn by the HUD.
// Each is shown only while its own tab has the focus, so a sun study never sits
// under a visibility gradient and two visibility tabs never show one result.
//
// Pure: no ImGui, no scene. The HUD feeds it what the scene shows each frame.

#include <cstdint>
#include <string>

namespace geomsrv::archviz::analysistabs {

enum Tab : int {
    kSunTab = 0,
    kSurfaceVisibilityTab = 1,
    kPointVisibilityTab = 2,
    kViewpointTab = 3,
};

// The tab whose result a resident study is: kind 0 the sun study, kind 1 a
// visibility study from surfaces (origin 0) or from a placed point (origin 1).
int TabOf (uint32_t analysisKind, int visibilityOrigin);

struct Focus {
    int tab = kSunTab;  // the tab in focus; only its result is shown
    int selectTab = -1; // a tab the tab bar must select, or -1
    std::string seenStudy;
    uint64_t seenClaim = 0;
    int shownOrigin = 0; // the visibility origin of the resident kind-1 study
    // Per tint tab (sun, surface, point): the last study it showed, which it puts
    // back when it takes the focus again. Empty once cleared on its own tab.
    std::string keptStudy[3];
    int restoreTried = -1; // the tab a put-back was asked for since it took the focus
    int lastShownTab = -1; // the tab whose study was shown last frame, or -1
};

// Once a frame, with the resident study (empty for none), its kind, the analysis
// display's claim generation and -- for kind 1 -- the origin it was run with.
// A study that ARRIVES through an explicit display claim takes the focus to its
// tab. A clear never moves the focus (a role edit clears, and following that sent
// the page to its first tab), and neither does a background sun refresh, which
// replaces the atlas without claiming the display.
void FollowShownStudy (Focus& focus, const std::string& shownStudy, uint32_t analysisKind, uint64_t claim,
                       int visibilityOrigin);

// The tab bar drew `tab` open this frame. A pending selection wins until the bar
// has actually switched to it, so the frame between the request and the switch
// cannot hand the focus back.
void Opened (Focus& focus, int tab);

// Request a tab from outside the tab bar (a command, or a run started elsewhere).
void Select (Focus& focus, int tab);

// Once a frame, after FollowShownStudy: record the shown study as its tab's, and
// forget a tab's study cleared while that tab had the focus. Returns the tab in
// focus when its kept study should be put back on screen now -- it is not the one
// shown, and no put-back was asked for since the tab took the focus -- else -1.
// ⚠️ ONE ASK PER VISIT. A put-back the scene refuses (a stale visibility capture)
// leaves the other study up; asking again every frame would never stop.
int KeepShownStudy (Focus& focus, const std::string& shownStudy, uint32_t analysisKind);

// Whether the resident study's tint draws: only on the tab that owns it.
bool ShowsTint (const Focus& focus, const std::string& shownStudy, uint32_t analysisKind);

} // namespace geomsrv::archviz::analysistabs

#endif
