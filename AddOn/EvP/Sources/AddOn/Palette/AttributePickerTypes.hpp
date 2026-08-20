#ifndef GEOMETRYSERVER_PALETTE_ATTRIBUTEPICKERTYPES_HPP
#define GEOMETRYSERVER_PALETTE_ATTRIBUTEPICKERTYPES_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "APIdefs_Interface.h"   // API_UserControlType, API_AttributePickerParams

namespace evp {

// An evp.<Attribute> parameter type -> the Archicad picker control that lists it,
// and the attribute type behind that control. False for anything that is not an
// attribute picker at all (numbers, popups, evp.View, …), which is what routes a
// parameter down the rest of ParamPanel's chain.
//
// Lives apart from ParamPanel because it is API KNOWLEDGE, not panel layout: every
// entry here is a claim about the picker's supported-type list, and one of them was
// already wrong once (see the .cpp).
bool UserControlTypeFor (const GS::UniString& type, API_UserControlType& control,
                         API_AttrTypeID& attrType);

}   // namespace evp

#endif
