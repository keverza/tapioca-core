// Metadata/MetadataStorage -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Metadata/MetadataStorage.hpp"

#include <cstdio>
#include <cstring>

namespace geomsrv {
namespace metadata {
namespace storage {

namespace {

// The user data's first bytes: Tapioca's, then the JSON. Another add-on's data on the element
// is its own; this checks the payload is ours before parsing it.
constexpr char kMagic[4] = { 'T', 'P', 'M', 'D' };

std::string Describe (GSErrCode err)
{
    char text[32] = {};
    std::snprintf (text, sizeof (text), "0x%08X", unsigned (err));
    return text;
}

bool GuidOf (const std::string& text, API_Guid& guid, std::string& error)
{
    guid = APIGuidFromString (text.c_str ());
    if (guid == APINULLGuid) {
        error = "\"" + text + "\" is not an element GUID";
        return false;
    }
    return true;
}

// `text` into a new handle after the magic; null when it could not be had.
GSHandle Payload (const std::string& text)
{
    const GSSize size = GSSize (sizeof (kMagic) + text.size ());
    GSHandle handle = BMAllocateHandle (size, ALLOCATE_CLEAR, 0);
    if (handle == nullptr)
        return nullptr;
    std::memcpy (*handle, kMagic, sizeof (kMagic));
    std::memcpy (*handle + sizeof (kMagic), text.data (), text.size ());
    return handle;
}

// The JSON after the magic, or false when the handle is not ours.
bool Unpack (GSConstHandle handle, std::string& text)
{
    const GSSize size = handle != nullptr ? BMGetHandleSize (handle) : 0;
    if (size < GSSize (sizeof (kMagic)) || std::memcmp (*handle, kMagic, sizeof (kMagic)) != 0)
        return false;
    text.assign (*handle + sizeof (kMagic), size_t (size) - sizeof (kMagic));
    // A handle may be padded with zeros past the text.
    const size_t end = text.find ('\0');
    if (end != std::string::npos)
        text.resize (end);
    return true;
}

} // namespace

bool Read (const std::string& guid, EntityMetadata& meta, bool& present, std::string& error)
{
    present = false;
    meta = EntityMetadata {};
    API_Elem_Head head = {};
    if (!GuidOf (guid, head.guid, error))
        return false;
    API_ElementUserData userData = {};
    const GSErrCode err = ACAPI_Element_GetUserData (&head, &userData);
    if (err == APIERR_NOUSERDATA) {
        AdoptElement (meta, guid);
        return true;
    }
    if (err != NoError) {
        error = "the element " + guid + " could not be read (" + Describe (err) + ")";
        return false;
    }
    std::string text;
    const bool ours = Unpack (userData.dataHdl, text);
    BMKillHandle (&userData.dataHdl);
    if (!ours) {
        // Not Tapioca's: nothing of ours is on it.
        AdoptElement (meta, guid);
        return true;
    }
    if (!FromJson (text, meta, error)) {
        error = "the element " + guid + "'s Tapioca metadata: " + error;
        return false;
    }
    present = true;
    AdoptElement (meta, guid);
    return true;
}

bool Write (const std::string& guid, EntityMetadata meta, std::string& error)
{
    API_Elem_Head head = {};
    if (!GuidOf (guid, head.guid, error))
        return false;
    AdoptElement (meta, guid);
    API_ElementUserData userData = {};
    userData.dataVersion = short (kFormatVersion);
    userData.platformSign = GS::Act_Platform_Sign;
    userData.flags = 0;
    userData.dataHdl = Payload (ToJson (meta));
    if (userData.dataHdl == nullptr) {
        error = "no memory for the element " + guid + "'s metadata";
        return false;
    }
    const GSErrCode err = ACAPI_Element_SetUserData (&head, &userData);
    BMKillHandle (&userData.dataHdl);
    if (err != NoError) {
        error = "the element " + guid + "'s metadata could not be stored (" + Describe (err) + ")" +
                (err == APIERR_NEEDSUNDOSCOPE ? ": no undo scope is open" : "");
        return false;
    }
    return true;
}

bool ReadSchema (ProjectSchema& schema, bool& stored, std::string& error)
{
    stored = false;
    schema = DefaultSchema ();
    API_Guid object = APINULLGuid;
    if (ACAPI_AddOnObject_GetUniqueObjectGuidFromName (GS::UniString (kSchemaObjectName), &object) != NoError ||
        object == APINULLGuid)
        return true;
    GS::UniString name;
    GSHandle content = nullptr;
    const GSErrCode err = ACAPI_AddOnObject_GetObjectContent (object, &name, &content);
    if (err != NoError) {
        error = "the project's Tapioca schema could not be read (" + Describe (err) + ")";
        return false;
    }
    std::string text;
    const bool ours = Unpack (content, text);
    BMKillHandle (&content);
    if (!ours) {
        error = "the project's Tapioca schema object holds something else";
        return false;
    }
    ProjectSchema read;
    if (!FromJson (text, read, error)) {
        error = "the project's Tapioca schema: " + error;
        return false;
    }
    // What a newer default offers that this project's schema lacks, in memory only.
    Extend (read, DefaultSchema ());
    schema = std::move (read);
    stored = true;
    return true;
}

bool WriteSchema (ProjectSchema schema, std::string& error)
{
    ++schema.revision;
    API_Guid object = APINULLGuid;
    if (ACAPI_AddOnObject_GetUniqueObjectGuidFromName (GS::UniString (kSchemaObjectName), &object) != NoError ||
        object == APINULLGuid) {
        const GSErrCode created = ACAPI_AddOnObject_CreateUniqueObject (GS::UniString (kSchemaObjectName), &object);
        if (created != NoError) {
            error = "the project's Tapioca schema could not be created (" + Describe (created) + ")" +
                    (created == APIERR_NOTEAMWORKPROJECT ? ": an offline Teamwork project cannot create it" : "") +
                    (created == APIERR_CANCEL ? ": the Teamwork send and receive was refused" : "");
            return false;
        }
    }
    GSHandle content = Payload (ToJson (schema));
    if (content == nullptr) {
        error = "no memory for the project's Tapioca schema";
        return false;
    }
    const GSErrCode err = ACAPI_AddOnObject_ModifyObject (object, nullptr, &content);
    BMKillHandle (&content);
    if (err != NoError) {
        error = "the project's Tapioca schema could not be stored (" + Describe (err) + ")" +
                (err == APIERR_NOTMINE ? ": another Teamwork user holds it -- reserve it first" : "");
        return false;
    }
    return true;
}

} // namespace storage
} // namespace metadata
} // namespace geomsrv
