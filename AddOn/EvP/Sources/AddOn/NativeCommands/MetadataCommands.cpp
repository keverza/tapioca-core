#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/MetadataCommands.hpp"
#include "NativeCommands/CommandBase.hpp"

#include "Metadata/MetadataStorage.hpp"
#include "Metadata/TapiocaMetadata.hpp"

#include <string>
#include <vector>

namespace geomsrv {

namespace {

namespace meta = metadata;

// ---------------------------------------------------------------------------
// Tapioca's metadata on elements, and the project's schema of it (the user,
// 2026-10-03: a typed property graph -- six primitives a workflow defines its
// schema over -- stored on the element, the schema in one project object).
//
// ⚠️ JSON TEXT IN, JSON TEXT OUT. An entity's metadata and the schema are the
// model's own JSON (Metadata/TapiocaMetadata.hpp ToJson / FromJson), carried as
// a string: the format is the model's, versioned with it, and a script hands it
// to json.loads rather than to a second mapping that could drift from it.
//
// ⚠️ A WRITE IS VALIDATED AGAINST THE PROJECT'S SCHEMA FIRST: a defined key of
// another type, an option its enumeration lacks, a range over a domain its
// definition does not allow are refused with the schema's sentences, and the
// element is not touched. Undefined keys are fine -- the graph is open.
//
// The writes are WriteCommands: the dispatcher's one undo scope around them
// (CommandBase.hpp), never one of their own.
// ---------------------------------------------------------------------------

GS::UniString Uni (const std::string& text)
{
    return GS::UniString (text.c_str (), CC_UTF8);
}

std::string Std (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
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

GS::Array<GS::UniString> Sentences (const std::vector<std::string>& lines)
{
    GS::Array<GS::UniString> out;
    for (const std::string& line : lines)
        out.Push (Uni (line));
    return out;
}

// ---------------------------------------------------------------------------
// Tapioca.GetElementMetadata { elements:[{elementId:{guid}}] }
//   -> { count, items:[{elementId:{guid}, found, metadata, error?}] }
//
// One record per input, positionally aligned. `found` says the element carries
// Tapioca metadata; without it `metadata` is the element's empty entity, ready
// to be filled and written back. An element that cannot be read says why in
// `error` and leaves `metadata` empty -- the batch goes on.
// ---------------------------------------------------------------------------
class GetElementMetadataCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "GetElementMetadata";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> elements;
        if (!params.Get ("elements", elements))
            return NativeCommandResult::Failure (
                EVP_FAIL ("need elements=[{elementId:{guid}}]", "Tapioca.GetElementMetadata"));

        GS::Array<GS::ObjectState> items;
        for (const GS::ObjectState& element : elements) {
            GS::UniString guid;
            if (!ReadElementId (element, guid))
                return NativeCommandResult::Failure (
                    EVP_FAIL ("every element needs elementId.guid", "Tapioca.GetElementMetadata"));
            GS::ObjectState item;
            AddElementId (guid, item);
            meta::EntityMetadata entity;
            bool present = false;
            std::string error;
            if (meta::storage::Read (Std (guid), entity, present, error)) {
                item.Add ("found", present);
                item.Add ("metadata", Uni (meta::ToJson (entity)));
            }
            else {
                item.Add ("found", false);
                item.Add ("metadata", GS::UniString ());
                item.Add ("error", Uni (error));
            }
            items.Push (item);
        }
        GS::ObjectState os;
        os.Add ("items", items);
        os.Add ("count", (GS::Int32) items.GetSize ());
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.SetElementMetadata { items:[{elementId:{guid}, metadata}], mode? }
//   -> { count, changed, results:[{elementId:{guid}, succeeded, error?, problems?}] }
//
// `mode` "merge" (the default) lays each item's metadata over what the element
// carries -- what it does not mention is kept (metadata::Merge); "replace" makes
// it the element's whole metadata. Per-element results, as SetElementIds: a
// refused item is said and the rest are written; a caller that wants all or
// nothing runs it in an evp.transaction.
// ---------------------------------------------------------------------------
class SetElementMetadataCommand : public WriteCommand {
  public:
    GS::String GetName () const override
    {
        return "SetElementMetadata";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> items;
        if (!params.Get ("items", items))
            return NativeCommandResult::Failure (
                EVP_FAIL ("need items=[{elementId:{guid},metadata}]", "Tapioca.SetElementMetadata"));
        GS::UniString mode ("merge");
        params.Get ("mode", mode);
        const bool replace = mode == "replace";
        if (!replace && !(mode == "merge"))
            return NativeCommandResult::Failure (EVP_FAIL ("mode is merge or replace", "Tapioca.SetElementMetadata"));

        meta::ProjectSchema schema;
        bool stored = false;
        std::string schemaError;
        if (!meta::storage::ReadSchema (schema, stored, schemaError))
            return NativeCommandResult::Failure (EVP_FAIL (Uni (schemaError), "Tapioca.SetElementMetadata"));

        GS::Array<GS::ObjectState> results;
        GS::Int32 changed = 0;
        for (const GS::ObjectState& item : items) {
            GS::UniString guid, text;
            if (!ReadElementId (item, guid))
                return NativeCommandResult::Failure (
                    EVP_FAIL ("every item needs elementId.guid", "Tapioca.SetElementMetadata"));
            item.Get ("metadata", text);
            GS::ObjectState rec;
            AddElementId (guid, rec);
            const auto refuse = [&] (const std::string& error, const std::vector<std::string>& problems) {
                rec.Add ("succeeded", false);
                rec.Add ("error", Uni (error));
                if (!problems.empty ())
                    rec.Add ("problems", Sentences (problems));
                results.Push (rec);
            };

            meta::EntityMetadata given;
            std::string error;
            if (!meta::FromJson (Std (text), given, error)) {
                refuse ("the metadata is not the model's JSON: " + error, {});
                continue;
            }
            meta::EntityMetadata entity;
            if (replace) {
                entity = std::move (given);
            }
            else {
                bool present = false;
                if (!meta::storage::Read (Std (guid), entity, present, error)) {
                    refuse (error, {});
                    continue;
                }
                meta::Merge (entity, given);
            }
            const std::vector<std::string> problems = meta::Validate (entity, schema);
            if (!problems.empty ()) {
                refuse ("the project's schema refuses it", problems);
                continue;
            }
            // NO undo scope here -- see WriteCommand. The caller has one open.
            if (!meta::storage::Write (Std (guid), std::move (entity), error)) {
                refuse (error, {});
                continue;
            }
            rec.Add ("succeeded", true);
            ++changed;
            results.Push (rec);
        }
        GS::ObjectState os;
        os.Add ("results", results);
        os.Add ("count", (GS::Int32) results.GetSize ());
        os.Add ("changed", changed);
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.GetMetadataSchema {} -> { schema, stored, revision }
//
// The project's schema as JSON: the stored one, extended in memory with what a
// newer default offers -- or the default itself, `stored` false. Reading never
// writes.
// ---------------------------------------------------------------------------
class GetMetadataSchemaCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "GetMetadataSchema";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        meta::ProjectSchema schema;
        bool stored = false;
        std::string error;
        if (!meta::storage::ReadSchema (schema, stored, error))
            return NativeCommandResult::Failure (EVP_FAIL (Uni (error), "Tapioca.GetMetadataSchema"));
        GS::ObjectState os;
        os.Add ("schema", Uni (meta::ToJson (schema)));
        os.Add ("stored", stored);
        os.Add ("revision", (GS::Int32) schema.revision);
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.SetMetadataSchema { schema, mode? } -> { revision, added }
//
// `mode` "extend" (the default) adds what `schema` defines that the project's
// lacks, by id -- a workflow's keys, enumerations, systems -- and never changes
// what is there; "replace" stores `schema` as the project's whole schema, the
// way to change an existing definition. Either way the revision moves on.
//
// ⚠️ THE FIRST WRITE CREATES THE PROJECT'S SCHEMA OBJECT, which in Teamwork is a
// full send and receive (Metadata/MetadataStorage.hpp); a later one is refused
// while another user holds it, with that reason.
// ---------------------------------------------------------------------------
class SetMetadataSchemaCommand : public WriteCommand {
  public:
    GS::String GetName () const override
    {
        return "SetMetadataSchema";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString text;
        if (!params.Get ("schema", text))
            return NativeCommandResult::Failure (
                EVP_FAIL ("need schema (the model's JSON)", "Tapioca.SetMetadataSchema"));
        GS::UniString mode ("extend");
        params.Get ("mode", mode);
        const bool replace = mode == "replace";
        if (!replace && !(mode == "extend"))
            return NativeCommandResult::Failure (EVP_FAIL ("mode is extend or replace", "Tapioca.SetMetadataSchema"));

        meta::ProjectSchema given;
        std::string error;
        if (!meta::FromJson (Std (text), given, error))
            return NativeCommandResult::Failure (
                EVP_FAIL (Uni ("the schema is not the model's JSON: " + error), "Tapioca.SetMetadataSchema"));
        meta::ProjectSchema schema;
        bool stored = false;
        if (!meta::storage::ReadSchema (schema, stored, error))
            return NativeCommandResult::Failure (EVP_FAIL (Uni (error), "Tapioca.SetMetadataSchema"));
        size_t added = 0;
        if (replace) {
            given.revision = schema.revision;
            schema = std::move (given);
        }
        else {
            added = meta::Extend (schema, given);
        }
        // NO undo scope here -- see WriteCommand. The caller has one open.
        if (!meta::storage::WriteSchema (schema, error))
            return NativeCommandResult::Failure (EVP_FAIL (Uni (error), "Tapioca.SetMetadataSchema"));
        GS::ObjectState os;
        os.Add ("revision", (GS::Int32) (schema.revision + 1));
        os.Add ("added", (GS::Int32) added);
        return os;
    }
};

const NativeCommandRegistration kMetadataCommandRegistrations[] = {
    { "GetElementMetadata", &MakeRegisteredNativeCommand<GetElementMetadataCommand>, false,
      R"json({"type":"object","properties":{"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}}},"additionalProperties":false,"required":["elements"]})json",
      R"json({"type":"object","properties":{"items":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"found":{"type":"boolean"},"metadata":{"type":"string"},"error":{"type":"string"}},"additionalProperties":false,"required":["elementId","found","metadata"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["items","count"]})json" },
    { "SetElementMetadata",
      &MakeRegisteredNativeCommand<SetElementMetadataCommand>, false, R"json({"type":"object","properties":{"items":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},"metadata":{"type":"string","minLength":2}},"additionalProperties":false,"required":["elementId","metadata"]}},"mode":{"type":"string","enum":["merge","replace"]}},"additionalProperties":false,"required":["items"]})json", R"json({"type":"object","properties":{"results":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"succeeded":{"type":"boolean"},"error":{"type":"string"},"problems":{"type":"array","items":{"type":"string"}}},"additionalProperties":false,"required":["elementId","succeeded"]}},"count":{"type":"integer","minimum":0},"changed":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["results","count","changed"]})json" },
    { "GetMetadataSchema", &MakeRegisteredNativeCommand<GetMetadataSchemaCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"schema":{"type":"string"},"stored":{"type":"boolean"},"revision":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["schema","stored","revision"]})json" },
    { "SetMetadataSchema", &MakeRegisteredNativeCommand<SetMetadataSchemaCommand>, false,
      R"json({"type":"object","properties":{"schema":{"type":"string","minLength":2},"mode":{"type":"string","enum":["extend","replace"]}},"additionalProperties":false,"required":["schema"]})json",
      R"json({"type":"object","properties":{"revision":{"type":"integer","minimum":0},"added":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["revision","added"]})json" }
};

} // namespace

NativeCommandRegistrations GetMetadataCommandRegistrations ()
{
    return MakeRegistrationView (kMetadataCommandRegistrations);
}

} // namespace geomsrv
