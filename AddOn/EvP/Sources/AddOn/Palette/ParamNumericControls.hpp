#ifndef EVP_PALETTE_PARAMNUMERICCONTROLS_HPP
#define EVP_PALETTE_PARAMNUMERICCONTROLS_HPP

#include "DGModule.hpp"
#include "ObjectState.hpp"

namespace evp {

struct ParamControl;
GS::UniString BuildIntegerControl (ParamControl& pc, const GS::ObjectState& metadata, const DG::Panel& panel,
                                   const DG::Rect& seed);

} // namespace evp

#endif
