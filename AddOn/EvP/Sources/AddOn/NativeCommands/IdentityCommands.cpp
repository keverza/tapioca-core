#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/IdentityCommands.hpp"
#include "NativeCommands/CommandBase.hpp"

namespace geomsrv {

namespace {

// ---------------------------------------------------------------------------
// The element "ID" — the one field EVERY element type has.
//
// In Element Settings it is the "ID" box (W-001, C-12, …). In the API it is the
// "compound info string", reached NOT through any element struct but through a
// pair of type-agnostic calls:
//
//   ACAPI_Element_GetElementInfoString    (ACAPI_Database.h)
//   ACAPI_Element_ChangeElementInfoString (ACAPI_Database.h)
//
// That is why this is its own domain rather than a branch in the per-type reads:
// there is no `element.wall.id`. The one call answers for a wall, a text, a
// drawing and a zone alike, so a caller can number ANY selection without this
// file ever learning a new type. Both functions document APIERR_BADELEMENTTYPE
// for the handful of types that genuinely have no ID — reported per element as
// `found=false, reason="noInfoString"` rather than failing the whole batch,
// because a mixed selection is the normal input.
//
// ChangeElementInfoString is a project modification, so SetElementIds is a
// WriteCommand and opens NO undo scope of its own (see CommandBase.hpp): a
// numbering run puts the whole batch in one evp.transaction and gets one undo.
// ---------------------------------------------------------------------------

// Localized type name + stable typeID, so a miss in a bulk read is actionable
// ("which types have no ID?") instead of an anonymous guid. Same shape as the
// miss records of GetElementDetails.
void AddTypeFields (const API_Guid& guid, GS::ObjectState& rec)
{
    API_Elem_Head head = {};
    head.guid = guid;
    if (ACAPI_Element_GetHeader (&head) != NoError) {
        rec.Add ("typeName", GS::UniString ());
        rec.Add ("typeId",   (GS::Int32) 0);
        return;
    }
    GS::UniString typeName;
    ACAPI_Element_GetElemTypeName (head.type, typeName);
    rec.Add ("typeName", typeName);
    rec.Add ("typeId",   (GS::Int32) head.type.typeID);
}

bool ReadElementId (const GS::ObjectState& item, GS::UniString& guid)
{
    GS::ObjectState elementId;
    return item.Get ("elementId", elementId) && elementId.Get ("guid", guid) && !guid.IsEmpty ();
}

void AddElementId (const GS::UniString& guid, GS::ObjectState& item)
{
    GS::ObjectState elementId;
    elementId.Add ("guid", guid);
    item.Add ("elementId", elementId);
}

// ---------------------------------------------------------------------------
// Tapioca.GetElementIds { elements:[{elementId:{guid}}] }
//   -> { count, identities: [ {elementId:{guid}, found, value, typeName, typeId,
//                              reason?} ] }
//
// One record per input guid, positionally aligned, so a caller can zip it
// against its own list without a branch. `elementId` is "" on a miss.
// ---------------------------------------------------------------------------
class GetElementIdsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetElementIds"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> elements;
        if (!params.Get ("elements", elements))
            return NativeCommandResult::Failure (EVP_FAIL ("need elements=[{elementId:{guid}}]", "Tapioca.GetElementIds"));

        GS::Array<GS::ObjectState> records;
        for (const GS::ObjectState& element : elements) {
            GS::UniString guidString;
            if (!ReadElementId (element, guidString))
                return NativeCommandResult::Failure (EVP_FAIL ("every element needs elementId.guid", "Tapioca.GetElementIds"));
            const API_Guid guid = APIGuidFromString (guidString.ToCStr ().Get ());

            GS::ObjectState rec;
            AddElementId (guidString, rec);

            GS::UniString    infoString;
            const GSErrCode  err = ACAPI_Element_GetElementInfoString (&guid, &infoString);
            if (err == NoError) {
                rec.Add ("found",     true);
                rec.Add ("value", infoString);
            } else {
                rec.Add ("found",     false);
                rec.Add ("value", GS::UniString ());
                // The two failures a caller must tell apart: the guid is stale
                // vs. this type has no ID field at all.
                rec.Add ("reason", GS::UniString (err == APIERR_BADELEMENTTYPE ? "noInfoString"
                                                                               : "notFound"));
            }
            AddTypeFields (guid, rec);
            records.Push (rec);
        }

        GS::ObjectState os;
        os.Add ("identities", records);
        os.Add ("count", (GS::Int32) records.GetSize ());
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.SetElementIds { identities:[{elementId:{guid},value}] }
//   -> { count, changed, results:[{elementId:{guid},succeeded,error?}] }
//
// Per-element results rather than an all-or-nothing failure: a numbering run
// over a mixed selection should write the 40 elements that CAN take an ID and
// tell the caller precisely which could not. `ok` at the top level is true when
// the batch RAN; `changed` is how many actually took the write. A caller that
// wants all-or-nothing has evp.transaction for that — a raised failure there
// rolls the whole scope back.
// ---------------------------------------------------------------------------
class SetElementIdsCommand : public WriteCommand {
public:
    GS::String GetName () const override { return "SetElementIds"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> items;
        if (!params.Get ("identities", items))
            return NativeCommandResult::Failure (EVP_FAIL ("need identities=[{elementId:{guid},value}]", "Tapioca.SetElementIds"));

        GS::Array<GS::ObjectState> results;
        GS::Int32 changed = 0;

        for (const GS::ObjectState& item : items) {
            GS::UniString guidString, value;
            GS::ObjectState rec;

            if (!ReadElementId (item, guidString))
                return NativeCommandResult::Failure (EVP_FAIL ("every identity needs elementId.guid", "Tapioca.SetElementIds"));
            // An empty value CLEARS the ID. Callers that want "leave alone"
            // simply omit the whole identity record.
            item.Get ("value", value);

            AddElementId (guidString, rec);

            const API_Guid  guid = APIGuidFromString (guidString.ToCStr ().Get ());
            // NO undo scope here — see WriteCommand. The caller has one open.
            const GSErrCode err  = ACAPI_Element_ChangeElementInfoString (&guid, &value);
            if (err == NoError) {
                rec.Add ("succeeded", true);
                ++changed;
            } else {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_ACAPI_FAIL ("ACAPI_Element_ChangeElementInfoString", err,
                                                  GS::UniString ("setting ID \"") + value + "\" on " + guidString));
            }
            results.Push (rec);
        }

        GS::ObjectState os;
        os.Add ("results", results);
        os.Add ("count",   (GS::Int32) results.GetSize ());
        os.Add ("changed", changed);
        return os;
    }
};

const NativeCommandRegistration kIdentityCommandRegistrations[] = {
    { "GetElementIds", &MakeRegisteredNativeCommand<GetElementIdsCommand>, false,
      R"json({"type":"object","properties":{"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}}},"additionalProperties":false,"required":["elements"]})json",
      R"json({"type":"object","properties":{"identities":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"found":{"type":"boolean"},"value":{"type":"string"},"typeName":{"type":"string"},"typeId":{"type":"integer"},"reason":{"type":"string","enum":["notFound","noInfoString"]}},"additionalProperties":false,"required":["elementId","found","value","typeName","typeId"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["identities","count"]})json" },
    { "SetElementIds", &MakeRegisteredNativeCommand<SetElementIdsCommand>, false,
      R"json({"type":"object","properties":{"identities":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},"value":{"type":"string"}},"additionalProperties":false,"required":["elementId","value"]}}},"additionalProperties":false,"required":["identities"]})json",
      R"json({"type":"object","properties":{"results":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"succeeded":{"type":"boolean"},"error":{"type":"string"}},"additionalProperties":false,"required":["elementId","succeeded"]}},"count":{"type":"integer","minimum":0},"changed":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["results","count","changed"]})json" }
};

}   // namespace

NativeCommandRegistrations GetIdentityCommandRegistrations ()
{
    return MakeRegistrationView (kIdentityCommandRegistrations);
}

} // namespace geomsrv
