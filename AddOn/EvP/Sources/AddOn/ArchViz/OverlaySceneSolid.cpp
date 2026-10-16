// ArchViz/OverlaySceneSolid -- a solid layer's items, marked for the guest's solid phase.
#include "ArchViz/OverlaySceneBuilder.hpp"

namespace geomsrv::archviz::overlayscene::build {
// A solid layer's items to the solid phase (OverlayLayers.hpp `Layer::solid`): its fills write a
// depth of their own; its lines and mesh edges are dashed and faint behind them -- "dash" where
// they named "always", a hidden pattern where they named none (user, 2026-10-16: "Hiden wireframe
// edges must be less visible and dashed"); its texts follow its fills. Screen items stay as they are.
void Builder::MarkSolid (size_t fills, size_t lines, size_t glyphs)
{
    for (size_t i = fills; i < draft_.fills.size (); ++i)
        if (!draft_.fills[i].draw.screen)
            draft_.fills[i].draw.solid = true;
    for (size_t i = lines; i < draft_.lines.size (); ++i) {
        DraftLine& line = draft_.lines[i];
        if (line.behind == kBehindShow) {
            line.behind = kBehindDash;
            if ((line.dashes >> 8) == kSolidPattern)
                line.dashes = (line.dashes & 0xFFu) | (DashOf (layers::kDefaultHiddenDash, 2) << 8);
        }
        line.behind |= kSolidPhase;
    }
    for (size_t i = glyphs; i < draft_.glyphs.size (); ++i)
        if ((draft_.glyphs[i].flags & kScreenAnchored) == 0)
            draft_.glyphs[i].behind |= kSolidPhase;
}
} // namespace geomsrv::archviz::overlayscene::build
