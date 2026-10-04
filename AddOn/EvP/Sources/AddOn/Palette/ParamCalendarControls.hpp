#ifndef EVP_PALETTE_PARAMCALENDARCONTROLS_HPP
#define EVP_PALETTE_PARAMCALENDARCONTROLS_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGDateTime.hpp"

namespace evp {
struct ParamControl;
void BuildCalendarControl (ParamControl& pc, const GS::ObjectState& metadata, const DG::Panel& panel,
                           const DG::Rect& seed, DG::DateTimeObserver& observer);
GS::UniString CalendarValueText (DG::DateTime& control);
} // namespace evp
#endif
