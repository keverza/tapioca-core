#ifndef EVP_NATIVECOMMANDS_DRAFTINGCURVEKINDS_HPP
#define EVP_NATIVECOMMANDS_DRAFTINGCURVEKINDS_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ObjectState.hpp"

namespace geomsrv {

inline bool IsWholeDraftingCurve (const API_Element& element)
{
    // Circle requests can read back as Arc. The SDK's Do_Arc_Edit changes
    // completeness through arc.whole without changing the element type.
    const API_ElemTypeID type = element.header.type.typeID;
    return type == API_CircleID || (type == API_ArcID && element.arc.whole);
}

inline void AddDraftingCurveDetails (const API_Element& element, GS::ObjectState& details)
{
    // Both native IDs share API_ArcType; field spelling is the public read contract.
    details.Add ("x", element.arc.origC.x);
    details.Add ("y", element.arc.origC.y);
    details.Add ("radius", element.arc.r);
    details.Add ("begAngle", element.arc.begAng);
    details.Add ("endAngle", element.arc.endAng);
    details.Add ("ratio", element.arc.ratio);
    details.Add ("angle", element.arc.angle);
    details.Add ("pen", (GS::Int32) element.arc.linePen.penIndex);
}

} // namespace geomsrv

#endif
