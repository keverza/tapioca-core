#include "NativeCommands/DraftingDatabaseTarget.hpp"

#include "Diagnostics/ApiError.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "NativeCommands/DraftingCurveKinds.hpp"

#include <cmath>

namespace geomsrv {

namespace {

GS::UniString DatabaseGuidString (const API_DatabaseInfo& database)
{
    return GS::UniString (APIGuidToString (database.databaseUnId.elemSetId).ToCStr ());
}

} // namespace

AnchoredWorksheetDatabase::~AnchoredWorksheetDatabase ()
{
    if (changed_) {
        if (const GSErrCode err = ACAPI_Database_ChangeCurrentDatabase (&original_); err != NoError)
            EVP_ACAPI_FAIL ("ACAPI_Database_ChangeCurrentDatabase", err,
                            "restoring the database after a worksheet drafting write");
    }
}

bool AnchoredWorksheetDatabase::Activate (const GS::ObjectState& params, GS::UniString& targetGuid,
                                          GS::UniString& error)
{
    GS::ObjectState anchorId;
    GS::UniString anchorGuidString;
    if (!params.Get ("databaseAnchorElementId", anchorId))
        return true;
    if (!anchorId.Get ("guid", anchorGuidString) || anchorGuidString.IsEmpty ()) {
        error = EVP_FAIL ("databaseAnchorElementId needs guid", "resolving the worksheet drafting target");
        return false;
    }

    const API_Guid anchorGuid = APIGuidFromString (anchorGuidString.ToCStr ().Get ());
    API_DatabaseInfo target = {};
    if (const GSErrCode err = ACAPI_Database_GetContainingDatabase (&anchorGuid, &target); err != NoError) {
        error = EVP_ACAPI_FAIL ("ACAPI_Database_GetContainingDatabase", err,
                                GS::UniString ("finding the database that contains anchor ") + anchorGuidString);
        return false;
    }
    if (target.typeID != APIWind_WorksheetID) {
        error = EVP_FAIL (GS::UniString ("database anchor is not in a worksheet: ") + anchorGuidString,
                          "validating the worksheet drafting target");
        return false;
    }
    if (const GSErrCode err = ACAPI_Database_GetCurrentDatabase (&original_); err != NoError) {
        error = EVP_ACAPI_FAIL ("ACAPI_Database_GetCurrentDatabase", err,
                                "saving the database before a worksheet drafting write");
        return false;
    }

    targetGuid = DatabaseGuidString (target);
    if (original_.typeID == target.typeID && original_.databaseUnId.elemSetId == target.databaseUnId.elemSetId)
        return true;
    if (const GSErrCode err = ACAPI_Database_ChangeCurrentDatabase (&target); err != NoError) {
        error = EVP_ACAPI_FAIL ("ACAPI_Database_ChangeCurrentDatabase", err,
                                GS::UniString ("activating worksheet database ") + targetGuid);
        return false;
    }
    changed_ = true;
    return true;
}

bool VerifyCreatedDraftingElement (const API_Guid& guid, API_ElemTypeID expectedType,
                                   const GS::UniString& expectedDatabaseGuid, GS::ObjectState& result,
                                   GS::UniString& error)
{
    API_Element created = {};
    if (const GSErrCode err = ACAPI_Element_GetElementFromAnywhere (&guid, &created); err != NoError) {
        error =
            EVP_ACAPI_FAIL ("ACAPI_Element_GetElementFromAnywhere", err, "verifying a newly created drafting element");
        return false;
    }
    const bool matches = expectedType == API_CircleID
                             ? IsWholeDraftingCurve (created) && std::isfinite (created.arc.ratio) &&
                                   std::fabs (created.arc.ratio - 1.0) <= 1e-9 && std::isfinite (created.arc.r) &&
                                   created.arc.r > 0.0
                             : created.header.type.typeID == expectedType;
    if (!matches) {
        GS::UniString description =
            GS::UniString::Printf ("new drafting element %T has the wrong type/geometry: expected=%d actual=%d",
                                   GS::UniString (APIGuidToString (guid).ToCStr ()).ToPrintf (), (int) expectedType,
                                   (int) created.header.type.typeID);
        if (created.header.type == API_ArcID || created.header.type == API_CircleID)
            description += GS::UniString::Printf (" whole=%d radius=%.9g ratio=%.9g begAngle=%.9g endAngle=%.9g",
                                                  (int) created.arc.whole, created.arc.r, created.arc.ratio,
                                                  created.arc.begAng, created.arc.endAng);
        error = EVP_FAIL (description, "verifying a newly created drafting element");
        return false;
    }

    API_DatabaseInfo containing = {};
    if (const GSErrCode err = ACAPI_Database_GetContainingDatabase (&guid, &containing); err != NoError) {
        error = EVP_ACAPI_FAIL ("ACAPI_Database_GetContainingDatabase", err,
                                "verifying the database of a newly created drafting element");
        return false;
    }
    const GS::UniString databaseGuid = DatabaseGuidString (containing);
    if (!expectedDatabaseGuid.IsEmpty () && databaseGuid != expectedDatabaseGuid) {
        error = EVP_FAIL (GS::UniString::Printf ("drafting element landed in database %T instead of worksheet %T",
                                                 databaseGuid.ToPrintf (), expectedDatabaseGuid.ToPrintf ()),
                          "verifying a newly created drafting element");
        return false;
    }

    GS::ObjectState databaseId;
    databaseId.Add ("guid", databaseGuid);
    result.Add ("databaseId", databaseId);
    result.Add ("layer", AttributeIndexToName (API_LayerID, created.header.layer));
    result.Add ("verified", true);
    return true;
}

} // namespace geomsrv
