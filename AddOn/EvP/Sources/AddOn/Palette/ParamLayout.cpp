#include "Palette/ParamLayout.hpp"

#include <algorithm>

namespace {

// 'GDLG' 32510 starts at 440 px wide with 14 px margins on either side.
constexpr int DefaultContentWidth = 412;
constexpr int NaturalInputWidth   = 100;
constexpr int WideGrowthPercent   = 10;

}   // namespace

namespace evp {

int InputColumnWidth (int contentWidth)
{
    contentWidth = std::max (0, contentWidth);

    // Until the default width, retain the requested one-third proportion, but
    // never make an ordinary text/numeric/picker field impractically narrow.
    if (contentWidth <= DefaultContentWidth)
        return std::max (NaturalInputWidth, contentWidth / 3);

    // A wider palette buys labels room first. Integer arithmetic is intentional:
    // DG rects are integral pixels, and a sub-pixel policy would only create
    // resize jitter after rounding.
    const int atDefault = std::max (NaturalInputWidth, DefaultContentWidth / 3);
    return atDefault + (contentWidth - DefaultContentWidth) * WideGrowthPercent / 100;
}

}   // namespace evp
