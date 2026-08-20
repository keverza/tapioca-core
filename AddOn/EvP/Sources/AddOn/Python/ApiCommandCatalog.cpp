#include "ApiCommandCatalog.hpp"

#include "AddOnCommands.hpp"
#include "DispatcherVerbs.hpp"
#include "NativeCommands/CommandSchemas.hpp"
#include "NativeCommands/SchemaValidator.hpp"

namespace evp {

GS::Array<TapiocaCommandInfo> EnumerateTapiocaCommands ()
{
    GS::Array<TapiocaCommandInfo> commands;
    for (const geomsrv::NativeCommandInfo& native : geomsrv::EnumerateNativeCommands ()) {
        TapiocaCommandInfo info;
        info.name = GS::UniString (native.name.ToCStr ());
        info.inputSchema = native.inputSchema;
        info.responseSchema = native.responseSchema;
        commands.Push (std::move (info));
    }
    for (const DispatcherVerbRegistration& verb : GetDispatcherVerbRegistrations ()) {
        TapiocaCommandInfo info;
        info.name = verb.name;
        info.dispatcherLocal = true;
        info.inputSchema = geomsrv::GetNativeInputSchema (GS::String (verb.name));
        info.responseSchema = geomsrv::GetNativeResponseSchema (GS::String (verb.name));
        commands.Push (std::move (info));
    }
    return commands;
}

TapiocaSchemaReport GetTapiocaSchemaReport ()
{
    TapiocaSchemaReport report;
    for (const TapiocaCommandInfo& command : EnumerateTapiocaCommands ()) {
        ++report.total;
        if (!command.inputSchema.HasValue ())
            report.missingInput.Push (command.name);
        if (!command.responseSchema.HasValue ())
            report.missingResponse.Push (command.name);
        if (command.inputSchema.HasValue () && command.responseSchema.HasValue ())
            ++report.complete;
    }
    return report;
}

bool ValidateTapiocaCommandCatalog (GS::UniString& error)
{
    if (!geomsrv::RunSchemaValidatorSelfCheck (error))
        return false;

    GS::Array<GS::UniString> names;
    for (const TapiocaCommandInfo& command : EnumerateTapiocaCommands ()) {
        if (command.name.IsEmpty ()) {
            error = "Tapioca command catalog contains an empty name";
            return false;
        }
        if (!command.inputSchema.HasValue () || !command.responseSchema.HasValue ()) {
            error = "Tapioca command is missing an input or response schema: " + command.name;
            return false;
        }
        for (const GS::UniString& existing : names) {
            if (existing == command.name) {
                error = "duplicate Tapioca command: " + command.name;
                return false;
            }
        }
        names.Push (command.name);
    }
    return true;
}

} // namespace evp
