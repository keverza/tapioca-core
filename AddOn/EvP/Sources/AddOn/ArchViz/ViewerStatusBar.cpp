#include "ArchViz/ViewerStatusBar.hpp"

#include <algorithm>

namespace geomsrv::archviz {

namespace {

// The fixed blocks; the analysis block takes the rest. Logical pixels, sized for
// the longest text each block composes ("Perspective", "1234 selected",
// "Reading model 100%").
constexpr short kViewWidth = 84;
constexpr short kInspectionWidth = 96;
constexpr short kModelWidth = 150;
constexpr short kGap = 12;

GS::UniString FromStd (const std::string& s)
{
    return GS::UniString (s.c_str (), CC_UTF8);
}

// What an alert block is drawn in: the command palette's advisory orange
// (Palette/ParamAdvisory.cpp), so "act on this" reads the same in both palettes.
Gfx::Color AlertColour ()
{
    return Gfx::Color (static_cast<unsigned char> (220), static_cast<unsigned char> (120),
                       static_cast<unsigned char> (20));
}

void Write (DG::StaticText& item, GS::UniString& last, const std::string& value)
{
    const GS::UniString next = FromStd (value);
    if (next != last) {
        item.SetText (next);
        last = next;
    }
}

template <typename Item> void Colour (Item& item, bool& last, bool alert)
{
    if (alert == last)
        return;
    if (alert)
        item.SetTextColor (AlertColour ());
    else
        item.ResetDefaultTextColor ();
    last = alert;
}

} // namespace

ViewerStatusBar::ViewerStatusBar (DG::Panel& panel, DG::LeftText& analysis, short height) : analysis_ (analysis)
{
    // A runtime DG item starts HIDDEN, which is what the bar wants until the
    // viewer draws.
    const DG::Rect rect (0, 0, 10, height);
    view_ = std::make_unique<DG::LeftText> (panel, rect, DG::StaticText::NoFrame, DG::StaticText::VCenter,
                                            DG::StaticText::EndTruncate);
    inspection_ = std::make_unique<DG::LeftText> (panel, rect, DG::StaticText::NoFrame, DG::StaticText::VCenter,
                                                  DG::StaticText::EndTruncate);
    model_ = std::make_unique<DG::RightText> (panel, rect, DG::StaticText::NoFrame, DG::StaticText::VCenter,
                                              DG::StaticText::EndTruncate);
}

bool ViewerStatusBar::SetShown (bool shown)
{
    if (shown == shown_)
        return false;
    shown_ = shown;
    for (DG::Item* item : { (DG::Item*) view_.get (), (DG::Item*) inspection_.get (), (DG::Item*) model_.get () }) {
        if (shown)
            item->Show ();
        else
            item->Hide ();
    }
    if (!shown) {
        // Forget what the blocks said, so the next viewer writes each afresh, and
        // give the borrowed item back in its own colour.
        lastView_ = lastInspection_ = lastModel_ = GS::UniString ();
        Colour (analysis_, analysisAlert_, false);
        Colour (*model_, modelAlert_, false);
    }
    return true;
}

void ViewerStatusBar::Layout (short left, short right, short top, short bottom)
{
    view_->SetRect (DG::Rect (left, top, (short) (left + kViewWidth), bottom));
    left = (short) (left + kViewWidth + kGap);
    inspection_->SetRect (DG::Rect (left, top, (short) (left + kInspectionWidth), bottom));
    left = (short) (left + kInspectionWidth + kGap);
    const short modelLeft = (short) (right - kModelWidth);
    model_->SetRect (DG::Rect (modelLeft, top, right, bottom));
    analysis_.SetRect (DG::Rect (left, top, (std::max) (left, (short) (modelLeft - kGap)), bottom));
}

std::string ViewerStatusBar::Apply (const viewerstatus::Blocks& blocks)
{
    Write (*view_, lastView_, blocks.view);
    Write (*inspection_, lastInspection_, blocks.inspection);
    Write (*model_, lastModel_, blocks.model);
    Colour (analysis_, analysisAlert_, blocks.analysisAlert);
    Colour (*model_, modelAlert_, blocks.modelAlert);
    return blocks.analysis;
}

} // namespace geomsrv::archviz
