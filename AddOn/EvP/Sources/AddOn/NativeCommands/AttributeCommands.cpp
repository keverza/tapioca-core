#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/AttributeCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp"      // AttributeNameToIndex

// ACAPI does not expose a profile attribute's vector image directly
// (ACAPI_Attribute_GetDefExt is documented for lines/fills/composites/layers/zones
// only — see ACAPinc.h:1855). The profile read therefore creates a TEMP wall with the
// picked profile, reads its memo.customOrigProfile, and computes the building
// dimensions via ProfileVectorImageOperations — all wrapped in an undoable session so
// the wall is created+deleted invisibly. This include is the reason this command is
// worth its own file: nothing else in the read domain needs it.
#include "ProfileVectorImageOperations.hpp"

#include <memory>

namespace geomsrv {

namespace {


// ---------------------------------------------------------------------------
// EvP.GetAttributeInfo { name, kind } -> composite thickness / profile bbox.
//
// Reads a NAMED attribute's geometry-backed dimensions for the cases where
// ShaftShell-style commands need to OFFSET geometry by the attribute's size:
//   composite  : totalThick [m] — for the future roof contour offset by the
//                wall composite width (read straight from API_CompWallType).
//   profile    : building-bbox width + height [m] — for the grate wall height,
//                which must come from the profile geometry, not an extra input.
//                ACAPI does not expose the profile attribute's vector image
//                (ACAPI_Attribute_GetDefExt is documented composite-only), so
//                the read creates a TEMP wall with the picked profile, reads
//                memo.customOrigProfile, calls
//                ProfileVectorImageOperations::CalculateBuildingDimensions,
//                then deletes the temp wall — all wrapped in an undoable
//                session so the project is unchanged when the call returns.
//   buildingMaterial : nothing to read beyond existence/index (returned always).
//
// LOGICALLY a read (no open caller transaction expected). It does, however,
// open its OWN undoable scope for the temp-wall probe — so it MUST be called
// OUTSIDE any open evp.transaction. IsWrite=false because the dispatcher must
// NOT wrap it (that would nest scopes and ACAPI_CallUndoableCommand refuses to
// nest — APIERR_REFUSEDCMD).
// ---------------------------------------------------------------------------
class GetAttributeInfoCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetAttributeInfo"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString name;
        if (!params.Get ("name", name) || name.IsEmpty ())
            return NativeCommandResult::Failure ("need name");
        GS::UniString kind;
        if (!params.Get ("kind", kind) || kind.IsEmpty ())
            return NativeCommandResult::Failure ("need kind = composite|profile|buildingMaterial");

        API_AttrTypeID typeID;
        if      (kind == "composite")        typeID = API_CompWallID;
        else if (kind == "profile")          typeID = API_ProfileID;
        else if (kind == "buildingMaterial") typeID = API_BuildingMaterialID;
        else
            return NativeCommandResult::Failure ("kind must be composite|profile|buildingMaterial");

        API_AttributeIndex idx;
        if (!AttributeNameToIndex (typeID, name, idx))
            return NativeCommandResult::Failure (GS::UniString::Printf ("%s not found: %T",
                                                        kind.ToCStr ().Get (), name.ToPrintf ()));

        GS::ObjectState os;
        os.Add ("name", name);
        os.Add ("kind", kind);
        os.Add ("index", idx.ToInt32_Deprecated ());

        // COMPOSITE — totalThick is a documented member of API_CompWallType, and
        // ACAPI_Attribute_Get fetches it directly. No memo / extended data needed.
        if (kind == "composite") {
            API_Attribute attribute = {};
            attribute.header.typeID = API_CompWallID;
            attribute.header.index = idx;
            const GSErrCode err = ACAPI_Attribute_Get (&attribute);
            if (err != NoError)
                return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Attribute_Get", err, "API_CompWallID (composite thickness)"));
            os.Add ("thickness", attribute.compWall.totalThick);   // meters
            return os;
        }

        // PROFILE — building bbox is read via a temp wall + ProfileVectorImageOperations.
        // The temp wall creation+deletion all happens inside one undoable command so
        // the project is byte-for-byte unchanged once the scope closes.
        if (kind == "profile") {
            double outHeight = 0.0, outWidth = 0.0;
            bool   gotIt     = false;
            GS::UniString diagError;

            const GSErrCode scopeErr = ACAPI_CallUndoableCommand (
                GS::UniString ("EvP.GetAttributeInfo: profile probe"),
                [&] () -> GSErrCode {
                    API_Element element = {};
                    element.header.type = API_WallID;
                    GSErrCode err = ACAPI_Element_GetDefaults (&element, nullptr);
                    if (err != NoError) { diagError = EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", err, "temp wall"); return err; }

                    element.wall.modelElemStructureType = API_ProfileStructure;
                    element.wall.profileType            = APISect_Poly;
                    element.wall.profileAttr            = idx;
                    element.wall.begC.x = 0.0;  element.wall.begC.y = 0.0;
                    element.wall.endC.x = 0.001; element.wall.endC.y = 0.0;
                    // GetDefaults gives a real height; don't zero it — a 0-height
                    // wall could be rejected or change the memo's customOrigProfile.
                    err = ACAPI_Element_Create (&element, nullptr);
                    if (err != NoError) { diagError = EVP_ACAPI_FAIL ("ACAPI_Element_Create", err, "temp profiled wall"); return err; }
                    const API_Guid tempGuid = element.header.guid;

                    API_ElementMemo memo = {};
                    err = ACAPI_Element_GetMemo (tempGuid, &memo, APIMemoMask_All);
                    if (err != NoError) {
                        diagError = EVP_ACAPI_FAIL ("ACAPI_Element_GetMemo", err, "APIMemoMask_All on the temp wall");
                        GS::Array<API_Guid> dead = { tempGuid };
                        ACAPI_Element_Delete (dead);
                        return err;
                    }

                    if (memo.customOrigProfile != nullptr) {
                        try {
                            const ProfileVectorImageOperations::Dimensions dims =
                                ProfileVectorImageOperations::CalculateBuildingDimensions (*memo.customOrigProfile);
                            outHeight = dims.height;
                            outWidth  = dims.width;
                            gotIt = true;
                        } catch (const GS::Exception& ex) {
                            diagError = GS::UniString ("CalculateBuildingDimensions threw: ") + ex.GetMessage ();
                        } catch (...) {
                            diagError = "CalculateBuildingDimensions threw an unknown exception";
                        }
                    } else {
                        diagError = "memo.customOrigProfile was null";
                    }

                    ACAPI_DisposeElemMemoHdls (&memo);

                    // Explicit delete: with the scope still open, the wall is gone
                    // BEFORE the command returns, so the user sees nothing new even
                    // if they never undo. The undo step records create-then-delete;
                    // undoing it also results in "no wall" (delete reverted then
                    // create reverted — net state unchanged either way).
                    GS::Array<API_Guid> dead = { tempGuid };
                    ACAPI_Element_Delete (dead);

                    return NoError;
                });

            if (scopeErr != NoError)
                return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_CallUndoableCommand", scopeErr,
                                                      GS::UniString ("profile probe; inner failure: ") + diagError));
            if (!gotIt)
                return NativeCommandResult::Failure (GS::UniString::Printf ("profile geometry unreadable: %T",
                                                            diagError.ToPrintf ()));
            os.Add ("height", outHeight);   // meters
            os.Add ("width",  outWidth);    // meters
            return os;
        }

        // BUILDING MATERIAL — nothing to read past the index; the caller mainly
        // wants the name resolution verified.
        return os;
    }
};

const NativeCommandRegistration kAttributeCommandRegistrations[] = {
    { "GetAttributeInfo", &MakeRegisteredNativeCommand<GetAttributeInfoCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "name":{"type":"string","minLength":1},
                "kind":{"type":"string","enum":["composite","profile","buildingMaterial"]}
            },
            "additionalProperties":false,
            "required":["name","kind"]
        })json",
      R"json({
            "type":"object",
            "properties":{
                "name":{"type":"string"},
                "kind":{"type":"string","enum":["composite","profile","buildingMaterial"]},
                "index":{"type":"integer"},
                "thickness":{"type":"number"},
                "height":{"type":"number"},
                "width":{"type":"number"}
            },
            "additionalProperties":false,
            "required":["name","kind","index"]
        })json" }
};

}   // namespace

NativeCommandRegistrations GetAttributeCommandRegistrations ()
{
    return MakeRegistrationView (kAttributeCommandRegistrations);
}

} // namespace geomsrv
