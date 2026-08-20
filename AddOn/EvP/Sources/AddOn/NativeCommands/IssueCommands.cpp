#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/IssueCommands.hpp"
#include "NativeCommands/CommandBase.hpp"

namespace geomsrv {

namespace {


// ===========================================================================
// E8 — Issues (Archicad "Mark-Ups"). WRITES: both ACAPI_Markup_Create and
// ACAPI_Markup_AttachElements return APIERR_NEEDSUNDOSCOPE outside an undoable
// scope, so both are WriteCommands (the caller supplies the scope — a transaction
// for atomic create+attach in one undo step, or the dispatcher for a lone call).
//
// Natively absorbs Tapir's CreateIssue / AttachElementsToIssue (MultimodalCheck's
// only Tapir dependency) onto the real AC29 markup symbols: API_MarkUpType and
// API_MarkUpComponentTypeID from APIdefs_Elements.h. An "Issue" in the Archicad
// UI is a Mark-Up entry in the API; the name difference is purely terminology.
// ===========================================================================

// AttachElements component type name -> API_MarkUpComponentTypeID. Names match
// Tapir's AttachElementsToIssue `type` field so a caller passes the same strings
// it passed through the proxy. Default is Highlight (a non-destructive overlay,
// what MultimodalCheck uses); an unknown name is a structured error, not a guess.
static bool MarkUpComponentTypeFromName (const GS::UniString& name, API_MarkUpComponentTypeID& out)
{
    if (name == "Highlight")    { out = APIMarkUpComponent_Highlight;    return true; }
    if (name == "Creation")     { out = APIMarkUpComponent_Creation;     return true; }
    if (name == "Deletion")     { out = APIMarkUpComponent_Deletion;     return true; }
    if (name == "Modification") { out = APIMarkUpComponent_Modification; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Tapioca.CreateIssue { name, tagText? } -> { issueId }
//
// Creates one Mark-Up (Issue) entry. `issueId` is the new Mark-Up's guid, to be
// passed to EvP.AttachElementsToIssue. ACAPI fills guid + timestamps in place.
// ---------------------------------------------------------------------------
class CreateIssueCommand : public WriteCommand {
public:
    GS::String GetName () const override { return "CreateIssue"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::UniString name;
        if (!params.Get ("name", name) || name.IsEmpty ())
            return NativeCommandResult::Failure ("need name (the Issue's name)");

        API_MarkUpType markUp (name);   // ctor takes the name; guid/time filled by Create
        GS::UniString tagText;
        if (params.Get ("tagText", tagText))
            markUp.tagText = tagText;

        // NO undo scope here — see WriteCommand. The caller has one open.
        const GSErrCode err = ACAPI_Markup_Create (markUp);
        if (err != NoError) {
            return NativeCommandResult::Failure (
                EVP_ACAPI_FAIL ("ACAPI_Markup_Create", err, GS::UniString::Printf ("issue \"%T\"", markUp.name.ToPrintf ())));
        }

        os.Add ("issueId", GS::UniString (APIGuidToString (markUp.guid).ToCStr ()));
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.AttachElementsToIssue { issueId, elements, componentType?:"Highlight" }
//   -> { attached }
//
// Attaches elements to an existing Issue. `componentType` is the role
// (Highlight/Creation/Deletion/Modification); default Highlight. `attached` is
// the number of guids submitted (ACAPI attaches the list atomically or fails).
//
// NOTE the key is `componentType`, NOT `type`: GS's JSON<->ObjectState converter
// reserves "type" as the ObjectState type discriminator, so a plain "type" field
// is swallowed and never readable with params.Get (the same reason
// GetConnectedElements uses "connectedElementType"). Using "type" here silently
// defaulted every call to Highlight — caught by IssueProbe's bad-type check.
// ---------------------------------------------------------------------------
class AttachElementsToIssueCommand : public WriteCommand {
public:
    GS::String GetName () const override { return "AttachElementsToIssue"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::UniString issueIdStr;
        if (!params.Get ("issueId", issueIdStr) || issueIdStr.IsEmpty ())
            return NativeCommandResult::Failure ("need issueId (from EvP.CreateIssue)");

        GS::Array<GS::ObjectState> elements;
        if (!params.Get ("elements", elements))
            return NativeCommandResult::Failure ("need elements=[{elementId:{guid}}]");

        API_MarkUpComponentTypeID compType = APIMarkUpComponent_Highlight;
        GS::UniString typeName;
        if (params.Get ("componentType", typeName) && !MarkUpComponentTypeFromName (typeName, compType)) {
            return NativeCommandResult::Failure (GS::UniString ("unknown componentType: \"" + typeName +
                    "\" (expected Highlight, Creation, Deletion, or Modification)"));
        }

        const API_Guid issueGuid = APIGuidFromString (issueIdStr.ToCStr ().Get ());
        GS::Array<API_Guid> elemList;
        for (const GS::ObjectState& item : elements) {
            GS::ObjectState elementId;
            GS::UniString guid;
            if (!item.Get ("elementId", elementId) || !elementId.Get ("guid", guid) || guid.IsEmpty ())
                return NativeCommandResult::Failure ("every element needs elementId.guid");
            elemList.Push (APIGuidFromString (guid.ToCStr ().Get ()));
        }

        // NO undo scope here — see WriteCommand. The caller has one open.
        const GSErrCode err = ACAPI_Markup_AttachElements (issueGuid, elemList, compType);
        if (err != NoError) {
            return NativeCommandResult::Failure (
                EVP_ACAPI_FAIL ("ACAPI_Markup_AttachElements", err, GS::UniString::Printf ("%u element(s) to the issue", (unsigned) elemList.GetSize ())));
        }

        os.Add ("attached", (GS::Int32) elemList.GetSize ());
        return os;
    }
};

const NativeCommandRegistration commandRegistrations[] = {
    { "CreateIssue", &MakeRegisteredNativeCommand<CreateIssueCommand>, false,
      R"json({"type":"object","properties":{"name":{"type":"string","minLength":1},"tagText":{"type":"string"}},"additionalProperties":false,"required":["name"]})json",
      R"json({"oneOf":[{"type":"object","properties":{"issueId":{"type":"string","minLength":1}},"additionalProperties":false,"required":["issueId"]},{"type":"object","properties":{"ok":{"const":false},"error":{"type":"string"}},"additionalProperties":false,"required":["ok","error"]}]})json" },
    { "AttachElementsToIssue", &MakeRegisteredNativeCommand<AttachElementsToIssueCommand>, false,
      R"json({"type":"object","properties":{"issueId":{"type":"string","minLength":1},"elements":{"$ref":"#Elements"},"componentType":{"type":"string","enum":["Highlight","Creation","Deletion","Modification"]}},"additionalProperties":false,"required":["issueId","elements"]})json",
      R"json({"oneOf":[{"type":"object","properties":{"attached":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["attached"]},{"type":"object","properties":{"ok":{"const":false},"error":{"type":"string"}},"additionalProperties":false,"required":["ok","error"]}]})json" },
};

}   // namespace

NativeCommandRegistrations GetIssueCommandRegistrations ()
{
    return MakeRegistrationView (commandRegistrations);
}

} // namespace geomsrv
