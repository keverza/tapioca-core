#ifndef EVP_NATIVECOMMANDS_DRAFTINGDATABASETARGET_HPP
#define EVP_NATIVECOMMANDS_DRAFTINGDATABASETARGET_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"

namespace geomsrv {

class AnchoredWorksheetDatabase {
  public:
    AnchoredWorksheetDatabase () = default;
    ~AnchoredWorksheetDatabase ();

    AnchoredWorksheetDatabase (const AnchoredWorksheetDatabase&) = delete;
    AnchoredWorksheetDatabase& operator= (const AnchoredWorksheetDatabase&) = delete;

    bool Activate (const GS::ObjectState& params, GS::UniString& targetGuid, GS::UniString& error);

  private:
    API_DatabaseInfo original_ = {};
    bool changed_ = false;
};

bool VerifyCreatedDraftingElement (const API_Guid& guid, API_ElemTypeID expectedType,
                                   const GS::UniString& expectedDatabaseGuid, GS::ObjectState& result,
                                   GS::UniString& error);

} // namespace geomsrv

#endif
