#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/NodeGraphElementCommands.hpp"

#include "NativeCommands/NodeGraphCommandSupport.hpp"
#include "NodeGraph/ArchicadHost.hpp"
#include "NodeGraph/ElementClassification.hpp"
#include "NodeGraph/ValueText.hpp"

#include <string>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// WHAT THESE ELEMENTS ARE: the classification-sensitive settings tree, on
// demand.
//
// ⚠️ A COMMAND RATHER THAN A NODE OUTPUT, DELIBERATELY. The settings a user
// inspects are not something a graph computes with; putting them on a port would
// mean every evaluation carried a whole property tree per element through the
// evaluator's cache for a panel that is usually closed. This is a QUESTION the
// inspector asks when it is opened, and its answer is live - unlike the type ids
// the selection node captures, which are as old as the capture.
//
// ⚠️ IT ANSWERS WITH THE SCHEMA IT USED. `types` carries the ordered settings
// descriptors for exactly the types present in `elements`, so a client can never
// render a value under a label it was not given, and never has to keep its own
// copy of the table. The alternative - a static catalog the client caches - is
// the one that goes stale silently after a build.
//
// ⚠️ READ-ONLY. ADR-007 excludes model writes, so there is no companion verb
// that sets any of this, and the descriptors carry no editor.
// ---------------------------------------------------------------------------

namespace geomsrv {
namespace {

// One selection's worth of elements, not one project's. A describe of the whole
// model is not something an inspector panel does, and an unbounded read across
// the gate is a hang wearing a command's clothes.
constexpr size_t kMaxDescribedElements = 2000;

constexpr const char kDescribeInputSchema[] =
    R"json({"type":"object","properties":{"guids":{"type":"array","items":{"type":"string","minLength":1}}},"additionalProperties":false,"required":["guids"]})json";

constexpr const char kDescribeResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"error":{"type":"string"},"truncated":{"type":"boolean"},"types":{"type":"array","items":{"type":"object","properties":{"id":{"type":"string"},"label":{"type":"string"},"plural":{"type":"string"},"settings":{"type":"array","items":{"type":"object","properties":{"id":{"type":"string"},"label":{"type":"string"},"group":{"type":"string"},"valueType":{"type":"string"},"unit":{"type":"string"},"origin":{"type":"string","enum":["archicad","derived"]},"appliesWhenSetting":{"type":"string"},"appliesWhenEquals":{"type":"string"}},"additionalProperties":false,"required":["id","label","group","valueType","unit","origin","appliesWhenSetting","appliesWhenEquals"]}}},"additionalProperties":false,"required":["id","label","plural","settings"]}},"elements":{"type":"array","items":{"type":"object","properties":{"guid":{"type":"string"},"elementType":{"type":"string"},"typeLabel":{"type":"string"},"available":{"type":"boolean"},"detail":{"type":"string"},"settings":{"type":"array","items":{"type":"object","properties":{"id":{"type":"string"},"text":{"type":"string"},"hasNumber":{"type":"boolean"},"number":{"type":"number"}},"additionalProperties":false,"required":["id","text","hasNumber","number"]}}},"additionalProperties":false,"required":["guid","elementType","typeLabel","available","detail","settings"]}}},"additionalProperties":false,"required":["ok","error","truncated","types","elements"]})json";

// A setting's number, when it HAS one. Bools deliberately do not: a checkbox
// rendered as 1 is worse than a checkbox rendered from its text.
bool NumberOf (const graph::Value& value, double& number)
{
    switch (value.Type ()) {
        case graph::ValueType::Double:
            number = std::get<double> (value.DataValue ());
            return true;
        case graph::ValueType::Integer:
            number = static_cast<double> (std::get<int64_t> (value.DataValue ()));
            return true;
        default:
            return false;
    }
}

GS::ObjectState EncodeSetting (const std::string& id, const graph::Value& value)
{
    GS::ObjectState encoded;
    encoded.Add ("id", GraphText (id));
    // The SAME renderer the panel node and every port summary use, so a
    // thickness reads identically wherever the user meets it.
    encoded.Add ("text", GraphText (graph::FormatValue (value)));
    double number = 0.0;
    const bool hasNumber = NumberOf (value, number);
    encoded.Add ("hasNumber", hasNumber);
    encoded.Add ("number", number);
    return encoded;
}

GS::ObjectState EncodeType (const graph::ElementTypeDescriptor& type)
{
    GS::ObjectState encoded;
    encoded.Add ("id", GraphText (type.id));
    encoded.Add ("label", GraphText (type.label));
    encoded.Add ("plural", GraphText (type.plural));
    GS::Array<GS::ObjectState> settings;
    for (const graph::ElementSettingDescriptor& setting : type.settings) {
        GS::ObjectState row;
        row.Add ("id", GraphText (setting.id));
        row.Add ("label", GraphText (setting.label));
        row.Add ("group", GraphText (graph::SettingGroupName (setting.group)));
        row.Add ("valueType", GraphText (GraphValueTypeName (setting.valueType)));
        row.Add ("unit", GraphText (setting.unit));
        // Where the value came from, and when the row applies at all. Both are
        // the CATALOG's answers - see ElementClassification.hpp. A client that
        // recomputed either would be a second place they could be got wrong, and
        // the one that renders "not read yet" over a row that simply does not
        // apply to this element.
        row.Add ("origin", GraphText (graph::SettingOriginName (setting.origin)));
        row.Add ("appliesWhenSetting", GraphText (setting.appliesWhen.settingId));
        row.Add ("appliesWhenEquals", GraphText (setting.appliesWhen.equalsText));
        settings.Push (row);
    }
    encoded.Add ("settings", settings);
    return encoded;
}

class GraphDescribeElementsCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::UniString> requested;
        params.Get ("guids", requested);

        std::vector<graph::ArchicadElementRef> elements;
        const bool truncated = requested.GetSize () > kMaxDescribedElements;
        for (UInt32 i = 0; i < requested.GetSize () && elements.size () < kMaxDescribedElements; ++i)
            elements.push_back (graph::ArchicadElementRef { GraphUtf8 (requested[i]) });

        std::vector<graph::ElementDescription> descriptions;
        std::string error;
        graph::IArchicadHost* host = graph::ActiveArchicadHost ();
        // A closed project is a REPORTED outcome, not a command failure: the
        // inspector sits beside a graph the user is still editing, and an error
        // dialog every time they open a panel with no project open is noise.
        bool ok = true;
        if (!elements.empty ()) {
            if (host == nullptr || !host->IsAvailable ()) {
                error = "no Archicad project is open";
                ok = false;
            }
            else {
                ok = host->DescribeElements (elements, descriptions, error);
            }
        }

        // Only the types actually present. A client renders values under labels
        // it was given in the same answer, so the two can never disagree.
        GS::Array<GS::ObjectState> types;
        for (const graph::ElementTypeGroup& group : graph::GroupByElementType (descriptions)) {
            const graph::ElementTypeDescriptor* type = graph::FindElementType (group.elementType);
            if (type != nullptr)
                types.Push (EncodeType (*type));
        }

        GS::Array<GS::ObjectState> encoded;
        for (const graph::ElementDescription& description : descriptions) {
            GS::ObjectState row;
            row.Add ("guid", GraphText (description.guid));
            row.Add ("elementType", GraphText (description.elementType));
            row.Add ("typeLabel", GraphText (description.typeLabel));
            row.Add ("available", description.available);
            row.Add ("detail", GraphText (description.detail));
            GS::Array<GS::ObjectState> settings;
            // In the TABLE's order, not the map's. The map is sorted by id, which
            // would put "bottomOffset" above "thickness" and scatter a wall's
            // fields into alphabetical nonsense.
            const graph::ElementTypeDescriptor* type = graph::FindElementType (description.elementType);
            if (type != nullptr) {
                for (const graph::ElementSettingDescriptor& descriptor : type->settings) {
                    const auto found = description.settings.find (descriptor.id);
                    // ABSENT STAYS ABSENT. A setting this build could not read is
                    // simply not in the answer, and the inspector shows it as
                    // unread rather than as zero.
                    if (found != description.settings.end ())
                        settings.Push (EncodeSetting (descriptor.id, found->second));
                }
            }
            row.Add ("settings", settings);
            encoded.Push (row);
        }

        GS::ObjectState response;
        response.Add ("ok", ok);
        response.Add ("error", GraphText (ok ? std::string {} : error));
        response.Add ("truncated", truncated);
        response.Add ("types", types);
        response.Add ("elements", encoded);
        return response;
    }
};

const NativeCommandRegistration registrations[] = {
    { "GraphDescribeElements", &MakeRegisteredNativeCommand<GraphDescribeElementsCommand>, false, kDescribeInputSchema,
      kDescribeResponseSchema },
};

} // namespace

NativeCommandRegistrations GetNodeGraphElementCommandRegistrations ()
{
    return MakeRegistrationView (registrations);
}

} // namespace geomsrv
