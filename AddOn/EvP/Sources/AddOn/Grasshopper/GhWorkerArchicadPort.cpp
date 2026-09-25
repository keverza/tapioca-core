#include "APIEnvir.h"
#include "ACAPinc.h"

#include "GhWorkerArchicadPort.hpp"

namespace evp {
namespace grasshopper {

uint32_t ReadArchicadJsonPort (GS::UniString& failure)
{
    UShort port = 0;
    const GSErrCode err = ACAPI_Command_GetHttpConnectionPort (&port);
    if (err != NoError) {
        failure = GS::UniString::Printf ("ACAPI_Command_GetHttpConnectionPort failed (%d); Tapir components will "
                                         "have to be given a port by hand",
                                         (int) err);
        return 0;
    }
    return (uint32_t) port;
}

} // namespace grasshopper
} // namespace evp
