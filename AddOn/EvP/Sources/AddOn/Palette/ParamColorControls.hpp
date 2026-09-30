#ifndef EVP_PALETTE_PARAMCOLORCONTROLS_HPP
#define EVP_PALETTE_PARAMCOLORCONTROLS_HPP

#include "DGModule.hpp"

namespace evp {

struct ParamControl;
void BuildColorControl (ParamControl& pc, const GS::ObjectState& metadata, const DG::Panel& panel, const DG::Rect& seed,
                        DG::ButtonItemObserver& buttonObserver, DG::UserItemObserver& swatchObserver);
bool OpenColorChooser (DG::Button& button, DG::UserItem* swatch, GS::UniString& hex);
bool DrawColorSwatch (const DG::UserItemUpdateEvent& ev, const DG::UserItem* swatch, const GS::UniString& hex);

} // namespace evp

#endif
