#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Palette/AttributePickerTypes.hpp"

// ---- attribute pickers -----------------------------------------------------
// Layer/Fill/LineType are NAMED attributes, so the value a script receives is the
// name: it matches the `= "Annotation"` default, reads well in a log, and stays
// meaningful across projects. Pens have no names — they are numbers in Archicad's
// own UI — so a pen parameter carries an int.
//
// AttributeNameToIndex / AttributeIndexToName live in
// NativeCommands/CommandUtils.hpp — ParamPanel used to carry a byte-identical
// copy of the forward lookup. Call them qualified: geomsrv::AttributeNameToIndex.
//
// Split out of ParamPanel.cpp when that file hit the soft cap: this is a table of
// claims about Archicad's picker, and it belongs with the header comment that
// justifies each one rather than in the middle of DG control construction.

namespace evp {

// evp.Layer/Fill/LineType/Surface/… -> Archicad's own attribute picker.
//
// The type MUST come from the supported list documented on API_AttributePickerParams
// (APIdefs_Interface.h). ACAPI_Dialog_CreateAttributePicker rejects anything else,
// which is exactly why the first attempt here fell back to a plain popup: it asked
// for APIUserControlType_AllFill, which is NOT on that list. The fills that are:
// PolyFill, WallFill, WallCompFill, AllFillGradNoComp, CoverFill,
// PolyFillWithGradient (v27+). PolyFill is the general-purpose one DG_Test uses.
//
// COMPOSITES, PROFILES and BUILDING MATERIALS (all v26+ on that supported list):
// the "native toggle" the user asked for is that the API ships a SEPARATE control
// type per element family — WallComposites / SlabComposites / RoofComposites /
// ShellComposites, and WallProfile / BeamProfile / ColumnProfile / HandrailProfile /
// AllProfile — so each picker pre-filters to only the composites/profiles that
// family can use. But there is only ONE underlying attribute type behind each group:
// every composite is API_CompWallID, every profile is API_ProfileID (the control
// merely filters the display). So attrType groups them: all composites share
// API_CompWallID, all profiles share API_ProfileID. That is what lets name->index
// resolution stay uniform on the create-command side (NativeCommands/CreateCommands.cpp).
//
// Every supported type hosts on a DG::PushCheck. Pen is deliberately absent — the
// picker does not support it at all; see the pen pool in ParamPanel::Rebuild.
bool UserControlTypeFor (const GS::UniString& type, API_UserControlType& control, API_AttrTypeID& attrType)
{
    if (type == "Layer") {
        control  = APIUserControlType_Layer;
        attrType = API_LayerID;
    } else if (type == "Fill") {
        control  = APIUserControlType_PolyFill;
        attrType = API_FilltypeID;
    } else if (type == "LineType") {
        control  = APIUserControlType_SymbolLine;
        attrType = API_LinetypeID;
    } else if (type == "Surface") {
        // Surfaces are "materials" in the API. APIUserControlType_Material is on the
        // picker's supported list (v26+, API_AttributePickerParams) — verified before
        // wiring, unlike AllFill which was not and fell back to a popup.
        control  = APIUserControlType_Material;
        attrType = API_MaterialID;
    } else if (type == "BuildingMaterial") {
        control  = APIUserControlType_BuildingMaterial;
        attrType = API_BuildingMaterialID;
    } else if (type == "WallComposite") {
        control  = APIUserControlType_WallComposites;
        attrType = API_CompWallID;
    } else if (type == "SlabComposite") {
        control  = APIUserControlType_SlabComposites;
        attrType = API_CompWallID;
    } else if (type == "RoofComposite") {
        control  = APIUserControlType_RoofComposites;
        attrType = API_CompWallID;
    } else if (type == "ShellComposite") {
        control  = APIUserControlType_ShellComposites;
        attrType = API_CompWallID;
    } else if (type == "WallProfile") {
        control  = APIUserControlType_WallProfile;
        attrType = API_ProfileID;
    } else if (type == "BeamProfile") {
        control  = APIUserControlType_BeamProfile;
        attrType = API_ProfileID;
    } else if (type == "ColumnProfile") {
        control  = APIUserControlType_ColumnProfile;
        attrType = API_ProfileID;
    } else if (type == "HandrailProfile") {
        control  = APIUserControlType_HandrailProfile;
        attrType = API_ProfileID;
    } else if (type == "AllProfile") {
        control  = APIUserControlType_AllProfile;
        attrType = API_ProfileID;
    } else {
        return false;
    }
    return true;
}

}   // namespace evp
