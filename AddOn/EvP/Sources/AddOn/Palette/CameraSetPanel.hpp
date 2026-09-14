#ifndef GEOMETRYSERVER_PALETTE_CAMERASETPANEL_HPP
#define GEOMETRYSERVER_PALETTE_CAMERASETPANEL_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"

#include <memory>
#include <vector>

namespace evp {

class PaletteScroll;

class CameraSetPanel {
  public:
    CameraSetPanel (const DG::Panel& panel, DG::ButtonItemObserver& buttonObserver, DG::ListBoxObserver& listObserver);

    void Rebuild (const GS::Array<GS::UniString>& names);
    void Clear ();
    short PlaceAt (short top, short left, short right, const PaletteScroll& clip);
    bool HandleButtonClicked (const DG::ButtonClickEvent& ev);
    bool HandleSelectionChanged (const DG::ListBoxSelectionEvent& ev);

  private:
    enum class Action { Add, Update, Remove, Restore, Clear };
    struct Row {
        GS::UniString name;
        GS::UniString status;
        std::unique_ptr<DG::LeftText> label;
        std::unique_ptr<DG::SingleSelListBox> list;
        std::unique_ptr<DG::Button> add;
        std::unique_ptr<DG::Button> update;
        std::unique_ptr<DG::Button> remove;
        std::unique_ptr<DG::Button> restore;
        std::unique_ptr<DG::Button> clear;
        std::unique_ptr<DG::LeftText> statusText;
    };

    void Populate (Row& row, short selected = 0);
    void RefreshActions (Row& row);
    bool Apply (Row& row, Action action);

    const DG::Panel& panel;
    DG::ButtonItemObserver& buttonObserver;
    DG::ListBoxObserver& listObserver;
    std::vector<Row> rows;
};

} // namespace evp

#endif
