#include "ParamNumericControls.hpp"
#include "ParamPanel.hpp"
#include "ParamValues.hpp"

namespace evp {

GS::UniString BuildIntegerControl (ParamControl& pc, const GS::ObjectState& metadata, const DG::Panel& panel,
                                   const DG::Rect& seed)
{
    pc.kind = ParamControl::Kind::Int;
    auto edit = std::make_unique<DG::IntEdit> (panel, seed);
    GS::Int32 minimum = 0, maximum = 0, value = 0;
    const bool haveMin = pc.type == "Hour" || metadata.Get ("minimum", minimum);
    const bool haveMax = pc.type == "Hour" || metadata.Get ("maximum", maximum);
    if (pc.type == "Hour") {
        minimum = 0;
        maximum = 23;
    }
    if (haveMin)
        edit->SetMin (minimum);
    if (haveMax)
        edit->SetMax (maximum);
    metadata.Get ("default", value);
    edit->SetValue (value);
    if (pc.type == "Hour") {
        pc.hourSpin = std::make_unique<DG::EditSpin> (panel, seed, *edit);
        pc.hourSpin->SetMin (0);
        pc.hourSpin->SetMax (23);
        pc.hourSpin->SetValue (edit->GetValue ());
    }
    pc.control = std::move (edit);
    return FormatDomain (haveMin, haveMax, GS::UniString::Printf ("%d", (int) minimum),
                         GS::UniString::Printf ("%d", (int) maximum));
}

} // namespace evp
