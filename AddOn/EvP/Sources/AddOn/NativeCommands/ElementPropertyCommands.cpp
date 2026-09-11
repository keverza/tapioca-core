#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ElementPropertyCommands.hpp"
#include "NativeCommands/CommandBase.hpp"

#include <string>
#include <vector>

namespace geomsrv {

namespace {

// ---------------------------------------------------------------------------
// Tapioca.GetElementProperties { elements:[{elementId:{guid}}], scope?:"all"|"user"|"builtin" }
//   -> { count, propertiesOfElements:[ one nested record per element ] }
//
// The Property-manager side of an element: its properties WITH THEIR EVALUATION
// STATUS, its classifications, and the four identity fields that make a property
// table readable (type name, element ID, layer, home story).
//
// ⚠️ THE STATUS IS THE POINT, AND LEAVING IT OUT IS WHY VALUES LOOKED
// "UNDEFINED FOR PROPERTIES ARCHICAD CLEARLY DEFINES". API_Property carries a
// status, and APIdefs_Properties.h is explicit about what it governs:
//
//     API_Property_NotAvailable  - not available for this element; "the content
//                                  of the value field is undefined"
//     API_Property_NotEvaluated  - could not be evaluated (an expression that
//                                  does not resolve, a static built-in that is
//                                  not evaluable for this element -- the header's
//                                  own example is a composite index on a basic
//                                  wall); value field undefined
//     API_Property_HasValue      - evaluated; the value field is meaningful
//
// MetadataExtractor called ACAPI_Property_GetPropertyValueString for EVERY
// property regardless, which formats a field the API says is undefined. The
// result is a string that looks like an answer and is not one. Here the value
// string is asked for ONLY on HasValue, and the status travels with the record so
// a caller can tell "empty" from "not applicable to this element" from "Archicad
// could not work it out".
//
// ⚠️ AND THE FILTER DEFAULTS TO ALL, NOT TO USER-DEFINED. The extractor passes
// API_PropertyDefinitionFilter_UserDefined, which omits every BUILT-IN property
// -- most of what the Property Manager shows. _All and _BuiltIn exist
// (APIdefs_Properties.h:523-530). `scope` chooses; "all" is the default because
// a caller asking what an element has means what it has.
//
// ⚠️ THE GROUP NAME COMES ALONG, because property names are only unique within
// a group and the palette shows them grouped. Groups are read once into a small
// cache per call rather than per property: a project has a handful of groups and
// an element can have dozens of properties.
//
// ⚠️ AN UNRESOLVABLE CLASSIFICATION IS REPORTED, NOT DROPPED.
// ACAPI_Element_GetClassificationItems answers one (system, item) pair per
// system the element participates in, and an element that is UNCLASSIFIED in a
// system still has a pair -- with an item guid that resolves to nothing. Dropping
// those (as the extractor does) is why a classification list could come back
// shorter than the systems a project has, with nothing saying which system was
// blank. Each pair is emitted; `found` says whether the item resolved.
//
// ⚠️ ONE RECORD PER INPUT GUID, POSITIONALLY ALIGNED. A short list would
// silently shift every later element's data onto the wrong guid -- the trap
// ElementReadCommands names. A guid that is not an API element (a composite
// sub-part is the ordinary case) emits `found:false` and empty tables.
//
// EXPENSIVE BY NATURE: a definition list and a value read per element, which is
// why it is called with exactly the guids asked for and never the whole model.
// ---------------------------------------------------------------------------

bool ReadElementId (const GS::ObjectState& record, GS::UniString& guid)
{
    GS::ObjectState elementId;
    if (!record.Get ("elementId", elementId))
        return false;
    return elementId.Get ("guid", guid);
}

API_PropertyDefinitionFilter FilterFor (const GS::UniString& scope)
{
    if (scope == "user")
        return API_PropertyDefinitionFilter_UserDefined;
    if (scope == "builtin")
        return API_PropertyDefinitionFilter_BuiltIn;
    return API_PropertyDefinitionFilter_All;
}

const char* DescribeStatus (API_PropertyValueStatus status)
{
    switch (status) {
        case API_Property_HasValue:
            return "hasValue";
        case API_Property_NotAvailable:
            return "notAvailable";
        case API_Property_NotEvaluated:
            return "notEvaluated";
    }
    return "unknown";
}

// A property group's name, remembered for the rest of this call.
class GroupNames {
  public:
    GS::UniString Of (const API_Guid& guid)
    {
        for (const Entry& entry : entries) {
            if (entry.guid == guid)
                return entry.name;
        }

        Entry entry;
        entry.guid = guid;
        API_PropertyGroup group = {};
        group.guid = guid;
        if (ACAPI_Property_GetPropertyGroup (group) == NoError)
            entry.name = group.name;
        entries.Push (entry);
        return entry.name;
    }

  private:
    struct Entry {
        API_Guid guid = APINULLGuid;
        GS::UniString name;
    };

    GS::Array<Entry> entries;
};

GS::Array<GS::ObjectState> PropertiesOf (const API_Guid& guid, API_PropertyDefinitionFilter filter, GroupNames& groups)
{
    GS::Array<GS::ObjectState> records;

    GS::Array<API_PropertyDefinition> definitions;
    if (ACAPI_Element_GetPropertyDefinitions (guid, filter, definitions) != NoError)
        return records;

    GS::Array<API_Property> properties;
    if (ACAPI_Element_GetPropertyValues (guid, definitions, properties) != NoError)
        return records;

    for (USize index = 0; index < properties.GetSize (); ++index) {
        const API_Property& property = properties[index];

        GS::ObjectState record;
        record.Add ("name", property.definition.name);
        record.Add ("group", groups.Of (property.definition.groupGuid));
        record.Add ("status", GS::UniString (DescribeStatus (property.status)));

        // ⚠️ ONLY ON HasValue. See the header note: on the other two statuses the
        // value field is documented as undefined, and formatting it would be
        // inventing an answer.
        GS::UniString text;
        if (property.status == API_Property_HasValue)
            ACAPI_Property_GetPropertyValueString (property, &text);
        record.Add ("value", text);
        record.Add ("isDefault", property.isDefault);
        records.Push (record);
    }

    return records;
}

GS::Array<GS::ObjectState> ClassificationsOf (const API_Guid& guid)
{
    GS::Array<GS::ObjectState> records;

    GS::Array<GS::Pair<API_Guid, API_Guid>> pairs; // (system, item)
    if (ACAPI_Element_GetClassificationItems (guid, pairs) != NoError)
        return records;

    for (const GS::Pair<API_Guid, API_Guid>& pair : pairs) {
        GS::ObjectState record;

        API_ClassificationSystem system = {};
        system.guid = pair.first;
        record.Add ("system",
                    ACAPI_Classification_GetClassificationSystem (system) == NoError ? system.name : GS::UniString ());

        API_ClassificationItem item = {};
        item.guid = pair.second;
        const bool resolved = ACAPI_Classification_GetClassificationItem (item) == NoError;
        record.Add ("found", resolved);
        record.Add ("code", resolved ? item.id : GS::UniString ());
        record.Add ("name", resolved ? item.name : GS::UniString ());
        records.Push (record);
    }

    return records;
}

// ⚠️ uniStringNamePtr, NOT header.name. The fixed char array truncates and
// mangles anything that is not ASCII; the UniString out-parameter is what
// MetadataExtractor::LayerName uses and is the only one that answers a layer
// called "Wände" correctly.
GS::UniString LayerNameOf (const API_AttributeIndex& index)
{
    API_Attribute attribute = {};
    attribute.header.typeID = API_LayerID;
    attribute.header.index = index;
    GS::UniString name;
    attribute.header.uniStringNamePtr = &name;
    if (ACAPI_Attribute_Get (&attribute) != NoError)
        return GS::UniString ();
    return name;
}

class GetElementPropertiesCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "GetElementProperties";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> elements;
        if (!params.Get ("elements", elements))
            return NativeCommandResult::Failure ("need elements=[{elementId:{guid}}]");

        GS::UniString scope ("all");
        params.Get ("scope", scope);
        const API_PropertyDefinitionFilter filter = FilterFor (scope);

        // The story names, read once for the batch: every element reports a
        // floor index and a table lookup is cheaper than a settings call each.
        API_StoryInfo stories = {};
        const bool haveStories = ACAPI_ProjectSetting_GetStorySettings (&stories) == NoError && stories.data != nullptr;

        GroupNames groups;
        GS::Array<GS::ObjectState> records;

        for (const GS::ObjectState& element : elements) {
            GS::UniString guidString;
            if (!ReadElementId (element, guidString))
                return NativeCommandResult::Failure ("every element needs elementId.guid");

            GS::ObjectState elementId;
            elementId.Add ("guid", guidString);

            GS::ObjectState record;
            record.Add ("elementId", elementId);

            const API_Guid guid = APIGuidFromString (guidString.ToCStr ().Get ());
            API_Elem_Head head = {};
            head.guid = guid;
            const bool found = guid != APINULLGuid && ACAPI_Element_GetHeader (&head) == NoError;

            record.Add ("found", found);
            if (!found) {
                // A full record even on a miss, for the alignment reason above.
                record.Add ("typeName", GS::UniString ());
                record.Add ("elemId", GS::UniString ());
                record.Add ("layer", GS::UniString ());
                record.Add ("story", GS::UniString ());
                record.Add ("storyIndex", (GS::Int32) 0);
                record.Add ("classifications", GS::Array<GS::ObjectState> ());
                record.Add ("properties", GS::Array<GS::ObjectState> ());
                records.Push (record);
                continue;
            }

            GS::UniString typeName;
            ACAPI_Element_GetElemTypeName (head.type, typeName);
            record.Add ("typeName", typeName);

            // The same string Tapioca.GetElementIds returns as `value`: the
            // element's compound info string, which Element Settings shows as ID.
            GS::UniString elemId;
            ACAPI_Element_GetElementInfoString (&guid, &elemId);
            record.Add ("elemId", elemId);

            record.Add ("layer", LayerNameOf (head.layer));

            // Indexed from 0 over lastStory-firstStory+1 entries, which is how
            // MetadataExtractor::BuildStories walks the same handle; a story's
            // own `index` is what matches floorInd, not its position.
            GS::UniString storyName;
            if (haveStories) {
                const short count = (short) (stories.lastStory - stories.firstStory + 1);
                for (short offset = 0; offset < count; ++offset) {
                    const API_StoryType& story = (*stories.data)[offset];
                    if (story.index == head.floorInd) {
                        storyName = GS::UniString (story.uName);
                        break;
                    }
                }
            }
            record.Add ("story", storyName);
            // The index as well as the name: a caller that computes with storeys
            // needs the number, and one that labels them needs the word.
            record.Add ("storyIndex", (GS::Int32) head.floorInd);

            record.Add ("classifications", ClassificationsOf (guid));
            record.Add ("properties", PropertiesOf (guid, filter, groups));
            records.Push (record);
        }

        if (haveStories)
            BMKillHandle (reinterpret_cast<GSHandle*> (&stories.data));

        GS::ObjectState os;
        os.Add ("propertiesOfElements", records);
        os.Add ("count", (GS::Int32) records.GetSize ());
        return os;
    }
};

const NativeCommandRegistration kElementPropertyCommandRegistrations[] = {
    { "GetElementProperties", &MakeRegisteredNativeCommand<GetElementPropertiesCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "elements":{"type":"array","items":{
                    "type":"object",
                    "properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},
                    "additionalProperties":false,
                    "required":["elementId"]
                }},
                "scope":{"type":"string","enum":["all","user","builtin"]}
            },
            "additionalProperties":false,
            "required":["elements"]
        })json",
      R"json({
            "type":"object",
            "properties":{
                "propertiesOfElements":{"type":"array","items":{
                    "type":"object",
                    "properties":{
                        "elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},
                        "found":{"type":"boolean"},
                        "typeName":{"type":"string"},
                        "elemId":{"type":"string"},
                        "layer":{"type":"string"},
                        "story":{"type":"string"},
                        "storyIndex":{"type":"integer"},
                        "classifications":{"type":"array","items":{
                            "type":"object",
                            "properties":{
                                "system":{"type":"string"},
                                "found":{"type":"boolean"},
                                "code":{"type":"string"},
                                "name":{"type":"string"}
                            },
                            "additionalProperties":false,
                            "required":["system","found","code","name"]
                        }},
                        "properties":{"type":"array","items":{
                            "type":"object",
                            "properties":{
                                "name":{"type":"string"},
                                "group":{"type":"string"},
                                "status":{"type":"string"},
                                "value":{"type":"string"},
                                "isDefault":{"type":"boolean"}
                            },
                            "additionalProperties":false,
                            "required":["name","group","status","value","isDefault"]
                        }}
                    },
                    "additionalProperties":false,
                    "required":["elementId","found","typeName","elemId","layer","story","storyIndex","classifications","properties"]
                }},
                "count":{"type":"integer","minimum":0}
            },
            "additionalProperties":false,
            "required":["propertiesOfElements","count"]
        })json" }
};

} // namespace

NativeCommandRegistrations GetElementPropertyCommandRegistrations ()
{
    return MakeRegistrationView (kElementPropertyCommandRegistrations);
}

} // namespace geomsrv
