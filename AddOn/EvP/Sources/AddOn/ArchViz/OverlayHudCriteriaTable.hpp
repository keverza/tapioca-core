// ArchViz/OverlayHudCriteriaTable -- the criteria grid's ImGui table, without the HUD engine.
//
// Bound by OVERLAY-INVARIANTS.md: ImGui layout/input only. The HUD engine owns the
// held text and turns edits into change events; the UI playground draws the same
// table from a fixture.
//
// Every column keeps a legible minimum width. When the panel is wider than their sum,
// the criterion, condition and value columns stretch in proportion; when it is
// narrower, the table alone scrolls horizontally instead of crushing its columns.
#ifndef TAPIOCA_ARCHVIZ_OVERLAYHUDCRITERIATABLE_HPP
#define TAPIOCA_ARCHVIZ_OVERLAYHUDCRITERIATABLE_HPP

#include "ArchViz/OverlayLayers.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace geomsrv::archviz::overlayhud {

struct CriteriaText {
    std::string sent, current, search;
    uint32_t field = 0;
};
struct CriteriaEdit {
    const char* kind = "";
    std::string rowId;
    double value = 0;
    std::string text;
};
struct CriteriaWidths {
    float open = 0, field = 0, condition = 0, value = 0, close = 0, link = 0, order = 0, remove = 0;
    float minimum = 0; // inner width at which every column is at its minimum
};

// Minimum column widths for the current font and style.
CriteriaWidths MeasureCriteria (const overlaylayers::PanelItem& item);
// Draws the grid `width` wide. Edits are appended; textActive is set while a text field has focus.
void DrawCriteriaTable (const overlaylayers::PanelItem& item, std::map<std::string, CriteriaText>& texts, float width,
                        std::vector<CriteriaEdit>& edits, bool& textActive);

} // namespace geomsrv::archviz::overlayhud

#endif
