#ifndef EVP_ARCHVIZ_VIEWERSTATUSBAR_HPP
#define EVP_ARCHVIZ_VIEWERSTATUSBAR_HPP

// ArchViz/ViewerStatusBar -- the 3D viewer palette's bottom edge as four blocks
// (issue #96): view | inspection | analysis | model. What they SAY is decided by
// ArchViz/ViewerStatus; this class only owns the DG items and writes into them.
//
// The ANALYSIS block is the palette's existing .grc status item, borrowed: it is
// the flexible one, and while the bar is hidden it is still the whole status
// line that reports a renderer starting or failing. The other three are created
// at runtime, like the viewport, so the .grc's positional ids stay put.
//
// ⚠️ EVERY WRITE IS COMPARED FIRST. A SetText is a DG paint into the palette that
// hosts the render surface (ArchVizPanel::RefreshStatus); an unchanged block must
// not be rewritten on every idle tick.
//
// MAIN THREAD ONLY, like every DG object.

#include "DGModule.hpp"

#include "ArchViz/ViewerStatus.hpp"

#include <memory>

namespace geomsrv::archviz {

class ViewerStatusBar final {
  public:
    ViewerStatusBar (DG::Panel& panel, DG::LeftText& analysis, short height);

    // True when this changed whether the blocks are shown: the caller re-lays out.
    bool SetShown (bool shown);
    bool IsShown () const
    {
        return shown_;
    }
    // The strip [left, right) x [top, bottom), logical pixels. Only while shown;
    // the analysis item is the caller's to place otherwise.
    void Layout (short left, short right, short top, short bottom);
    // Returns the analysis text, for the caller's own change tracking of the
    // borrowed item.
    std::string Apply (const viewerstatus::Blocks& blocks);

  private:
    DG::LeftText& analysis_;
    std::unique_ptr<DG::LeftText> view_;
    std::unique_ptr<DG::LeftText> inspection_;
    std::unique_ptr<DG::RightText> model_;
    bool shown_ = false;
    GS::UniString lastView_, lastInspection_, lastModel_;
    bool analysisAlert_ = false, modelAlert_ = false;
};

} // namespace geomsrv::archviz

#endif
