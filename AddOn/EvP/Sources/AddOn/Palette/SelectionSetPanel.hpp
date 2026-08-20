#ifndef GEOMETRYSERVER_PALETTE_SELECTIONSETPANEL_HPP
#define GEOMETRYSERVER_PALETTE_SELECTIONSETPANEL_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"

#include <memory>
#include <vector>

namespace evp {

class PaletteScroll;

// PLAT-6 — command-declared element-set capture controls. The shell remains the
// only observer; this panel owns just its runtime DG items and layout band.
//
// ⚠️ THIS BAND SITS BETWEEN THE DESCRIPTION AND THE PARAMETER BLOCK. The order is
// the order of the work: the description says what the command does, the user then
// builds the set, and the parameters below describe what to DO with that set.
// Underneath the parameters it read as a footnote to inputs that already depended
// on it; above the description it split the title from the prose explaining it.
// The ordering lives in ControlPaletteLayout, which is short of room and cannot
// hold the reasoning; it points here instead.
//
// Each row reads "<role> (<count>)", refreshed on every PlaceAt — the count is the
// only part that changes, and the role name is written by the command as the
// action it is ("Pasirinkti blokus"), so a trailing ": n saved" read as a second
// sentence about a label that was already one.
class SelectionSetPanel {
public:
    SelectionSetPanel (const DG::Panel& panel, DG::ButtonItemObserver& observer);

    void Rebuild (const GS::Array<GS::UniString>& names);
    void Clear ();
    short PlaceAt (short top, short left, short right, const PaletteScroll& clip);
    bool HandleButtonClicked (const DG::ButtonClickEvent& ev);

private:
    enum class Action { Update, Add, Remove, Reselect, Clear };
    struct Row {
        GS::UniString name;
        GS::UniString lastAction;
        std::unique_ptr<DG::LeftText> label;
        std::unique_ptr<DG::Button> update;
        std::unique_ptr<DG::Button> add;
        std::unique_ptr<DG::Button> remove;
        std::unique_ptr<DG::Button> reselect;
        std::unique_ptr<DG::Button> clear;
    };

    void RefreshLabel (Row& row);
    bool Apply (Row& row, Action action);

    const DG::Panel& panel;
    DG::ButtonItemObserver& observer;
    std::vector<Row> rows;
};

} // namespace evp

#endif
