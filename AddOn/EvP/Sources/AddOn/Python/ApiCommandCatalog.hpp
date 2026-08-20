#ifndef EVP_PYTHON_APICOMMANDCATALOG_HPP
#define EVP_PYTHON_APICOMMANDCATALOG_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"

namespace evp {

struct TapiocaCommandInfo {
    GS::UniString               name;
    bool                        dispatcherLocal = false;
    GS::Optional<GS::UniString> inputSchema;
    GS::Optional<GS::UniString> responseSchema;
};

struct TapiocaSchemaReport {
    UInt32                    total = 0;
    UInt32                    complete = 0;
    GS::Array<GS::UniString>  missingInput;
    GS::Array<GS::UniString>  missingResponse;
};

GS::Array<TapiocaCommandInfo> EnumerateTapiocaCommands ();
TapiocaSchemaReport GetTapiocaSchemaReport ();
bool ValidateTapiocaCommandCatalog (GS::UniString& error);

} // namespace evp

#endif
