#include "Grasshopper/GrasshopperBridge.hpp"

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "AddOnCommands.hpp"
#include "ObjectStateJSONConversion.hpp"
#include "Python/MainThreadGate.hpp"

#include <atomic>
#include <cstring>
#include <string>

namespace evp {
namespace grasshopper {

namespace {

// Set once the host is up, cleared by RevokeNativeApi. Atomic because the check
// is the one thing standing between a stale function pointer in a foreign
// runtime and a call into a torn-down add-on.
std::atomic<bool> serving { false };

GS::UniString FromWide (const uint16_t* text)
{
    if (text == nullptr)
        return GS::UniString ();
    return GS::UniString ((const GS::uchar_t*) text);
}

// One shape for every answer, success or failure, so the managed side has one
// thing to parse and a component can always show something. It mirrors the
// envelope ApiDispatcher already uses on the Python bus rather than inventing a
// second one.
GS::UniString Envelope (bool ok, const GS::ObjectState& data, const GS::UniString& error)
{
    GS::ObjectState envelope;
    envelope.Add ("ok", ok);
    if (ok)
        envelope.Add ("data", data);
    else
        envelope.Add ("error", error);

    GS::UniString json;
    if (JSON::CreateFromObjectState (envelope, json) != NoError) {
        // Hand-built, because even a serializer failure must be valid JSON: the
        // caller is a Grasshopper component that will try to parse whatever it
        // gets and should show a message, not throw.
        return GS::UniString ("{\"ok\":false,\"error\":\"The response could not be serialized.\"}");
    }
    return json;
}

// Copies `text` out under the caller-owned-buffer rule. Returns Ok when it fit,
// BufferTooSmall otherwise; `neededChars` is always set, so one failed call is
// enough to size the next one.
int32_t CopyOut (const GS::UniString& text, uint16_t* buffer, int32_t capacityChars, int32_t* neededChars)
{
    const GS::uchar_t* source = text.ToUStr ().Get ();
    int32_t            length = 0;
    while (source[length] != 0)
        ++length;

    if (neededChars != nullptr)
        *neededChars = length;

    if (buffer == nullptr || capacityChars <= length)
        return TapiocaGhStatus_BufferTooSmall;

    std::memcpy (buffer, source, (size_t) length * sizeof (uint16_t));
    buffer[length] = 0;
    return TapiocaGhStatus_Ok;
}

int32_t CallNative (const uint16_t* commandName,
                    const uint16_t* parametersJson,
                    uint16_t*       buffer,
                    int32_t         capacityChars,
                    int32_t*        neededChars)
{
    if (neededChars != nullptr)
        *neededChars = 0;

    if (!serving.load ())
        return TapiocaGhStatus_NotRunning;

    // ⚠️ THE THREAD CHECK IS NOT TIDINESS. Everything below touches ACAPI, and
    // ACAPI off the main thread corrupts Archicad rather than failing. A
    // Grasshopper component that quietly moved its work to a worker must be
    // refused here, loudly, not trusted.
    if (!MainThreadGate::Get ().IsMainThread ())
        return TapiocaGhStatus_WrongThread;

    const GS::UniString name = FromWide (commandName);
    if (name.IsEmpty ())
        return TapiocaGhStatus_BadRequest;

    const GS::String commandKey (name.ToCStr ().Get ());

    // Unknown before anything else, so a typo reads as a typo rather than as a
    // schema failure from whatever the dispatcher would have guessed.
    bool isWrite = false;
    if (!geomsrv::IsWriteCommand (commandKey, isWrite))
        return TapiocaGhStatus_UnknownCommand;

    // ⚠️ READS ONLY, BY DESIGN. A write needs exactly one undo scope, and
    // ExecuteNativeCommand deliberately opens none — core/CLAUDE.md puts undo
    // ownership on the dispatcher, not on the command. Opening one here would be
    // a second, weaker transaction owner. Writes arrive with that seam.
    if (isWrite)
        return TapiocaGhStatus_WriteRefused;

    GS::ObjectState params;
    const GS::UniString paramsJson = FromWide (parametersJson);
    if (!paramsJson.IsEmpty ()) {
        if (JSON::ConvertToObjectState (paramsJson, params) != NoError)
            return TapiocaGhStatus_BadRequest;
    }

    // No try/catch around this: ExecuteNativeCommand already converts every
    // throw it can see into a NativeCommandResult failure (see CommandRegistry),
    // and a second net here would only hide which layer failed.
    const geomsrv::NativeCommandResult result = geomsrv::ExecuteNativeCommand (commandKey, params);

    const GS::UniString json = Envelope (result.ok, result.data, result.error);
    const int32_t       copied = CopyOut (json, buffer, capacityChars, neededChars);
    if (copied != TapiocaGhStatus_Ok)
        return copied;

    // The command ran; whether it SUCCEEDED is in the envelope. Both are
    // reported, because a component needs to tell "Archicad said no" apart from
    // "the bridge is broken" and only the status can carry the second.
    return result.ok ? TapiocaGhStatus_Ok : TapiocaGhStatus_CommandFailed;
}

TapiocaGhNativeApi api = { (uint32_t) sizeof (TapiocaGhNativeApi), TAPIOCA_GH_ABI_VERSION, &CallNative };

} // namespace

const TapiocaGhNativeApi* NativeApi ()
{
    serving.store (true);
    return &api;
}

void RevokeNativeApi ()
{
    serving.store (false);
}

} // namespace grasshopper
} // namespace evp
