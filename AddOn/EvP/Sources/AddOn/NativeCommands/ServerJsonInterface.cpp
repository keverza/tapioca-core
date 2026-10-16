#include "APIEnvir.h"
#include "ACAPinc.h"
#include "NativeCommands/ServerJsonInterface.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/SchemaValidator.hpp"
#include "Python/ApiDispatcher.hpp"
#include "ObjectStateJSONConversion.hpp"

namespace geomsrv {
namespace {
// Transport adapters only. The schemas and implementations remain the existing
// dispatcher-local server verbs used by the palette; no second server lifecycle.
class ServerJsonCommand final : public MainThreadCommand {
  public:
    explicit ServerJsonCommand (const char* name) : name (name)
    {
    }
    GS::String GetName () const override
    {
        return name;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString error, json;
        if (!ValidateObjectStateSchema (params, GetInputParametersSchema (), error))
            return NativeCommandResult::Failure (error);
        if (JSON::CreateFromObjectState (params, json) != NoError)
            return NativeCommandResult::Failure ("Cannot serialize server command parameters.");
        const auto response = evp::DispatchApiCall (GS::UniString ("Tapioca.") + GS::UniString (name), json, "json");
        GS::ObjectState envelope, data;
        bool ok = false;
        if (JSON::ConvertToObjectState (response, envelope) != NoError || !envelope.Get ("ok", ok))
            return NativeCommandResult::Failure ("Invalid server dispatcher response.");
        if (!ok) {
            GS::ObjectState failure;
            GS::UniString message;
            envelope.Get ("error", failure);
            failure.Get ("message", message);
            return NativeCommandResult::Failure (message.IsEmpty () ? GS::UniString ("Server command failed.")
                                                                    : message);
        }
        if (!envelope.Get ("data", data) || !ValidateObjectStateSchema (data, GetResponseSchema (), error))
            return NativeCommandResult::Failure ("Invalid server response: " + error);
        return data;
    }

  private:
    const char* name;
};
} // namespace

GSErrCode InstallServerJsonInterface ()
{
    for (const auto* name : { "GetServerState", "StartServer", "StopServer" }) {
        const auto err =
            ACAPI_AddOnAddOnCommunication_InstallAddOnCommandHandler (GS::NewOwned<ServerJsonCommand> (name));
        if (err != NoError)
            return err;
    }
    return NoError;
}
} // namespace geomsrv
