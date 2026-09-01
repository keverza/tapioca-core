#include "NodeGraph/NodeRegistry.hpp"

#include <set>
#include <string>
#include <utility>

namespace evp::nodegraph {

const char* ExecutionDomainName (ExecutionDomain domain)
{
    switch (domain) {
        case ExecutionDomain::Worker:
            return "worker";
        case ExecutionDomain::ArchicadMainThread:
            return "archicadMainThread";
        case ExecutionDomain::RenderThread:
            return "renderThread";
    }
    return "worker";
}

const char* NodeDisplayName (NodeDisplay display)
{
    switch (display) {
        case NodeDisplay::Ports:
            return "ports";
        case NodeDisplay::Text:
            return "text";
        case NodeDisplay::Preview:
            return "preview";
        case NodeDisplay::SelectionSet:
            return "selectionSet";
    }
    return "ports";
}

const char* EffectKindName (EffectKind effect)
{
    switch (effect) {
        case EffectKind::Pure:
            return "pure";
        case EffectKind::ReadModel:
            return "readModel";
        case EffectKind::HostUiWrite:
            return "hostUiWrite";
    }
    return "pure";
}

const char* ParameterWidgetName (ParameterWidget widget)
{
    switch (widget) {
        case ParameterWidget::Auto:
            return "auto";
        case ParameterWidget::Number:
            return "number";
        case ParameterWidget::Slider:
            return "slider";
        case ParameterWidget::Boolean:
            return "boolean";
        case ParameterWidget::Select:
            return "select";
        case ParameterWidget::Text:
            return "text";
        case ParameterWidget::Vector:
            return "vector";
        case ParameterWidget::Point:
            return "point";
        case ParameterWidget::Color:
            return "color";
        case ParameterWidget::ReadOnly:
            return "readOnly";
    }
    return "auto";
}

const char* ParameterOptionSourceName (ParameterOptionSource source)
{
    switch (source) {
        case ParameterOptionSource::None:
            return "none";
        case ParameterOptionSource::Layer:
            return "layer";
        case ParameterOptionSource::Pen:
            return "pen";
        case ParameterOptionSource::Fill:
            return "fill";
        case ParameterOptionSource::LineType:
            return "lineType";
        case ParameterOptionSource::Surface:
            return "surface";
        case ParameterOptionSource::BuildingMaterial:
            return "buildingMaterial";
        case ParameterOptionSource::Composite:
            return "composite";
        case ParameterOptionSource::Profile:
            return "profile";
    }
    return "none";
}
namespace {

template <typename Schema> bool ValidateIds (const std::vector<Schema>& schemas, const char* kind, std::string& error)
{
    std::set<std::string> ids;
    for (const Schema& schema : schemas) {
        if (schema.id.empty () || !ids.insert (schema.id).second) {
            error = std::string ("invalid or duplicate ") + kind + " id: " + schema.id;
            return false;
        }
    }
    return true;
}

bool IsNumeric (ValueType valueType)
{
    return valueType == ValueType::Double || valueType == ValueType::Integer;
}

// UI-1's registration-time check.
//
// WARNING: THIS RUNS ONCE, HERE, RATHER THAN ON EVERY RENDER. Malformed metadata
// is a defect in a node type, and a defect in a node type should stop it
// entering the catalog - where it fails loudly, in one place, at startup -
// instead of reaching every client as a control that silently does the wrong
// thing. The catalog is also a strict-schema response: a widget name no schema
// admits makes GraphGetNodeTypes fail, and that is the first call the editor
// makes.
bool ValidateParameterUi (const NodeType& nodeType, const ParameterSchema& parameter, std::string& error)
{
    if (!parameter.ui.has_value ())
        return true;
    const ParameterUi& ui = *parameter.ui;
    const std::string where = " for parameter '" + parameter.id + "'";

    switch (ui.widget) {
        case ParameterWidget::Number:
        case ParameterWidget::Slider:
            if (!IsNumeric (parameter.valueType)) {
                error = "widget 'number'/'slider' needs an integer or double parameter" + where;
                return false;
            }
            break;
        case ParameterWidget::Boolean:
            if (parameter.valueType != ValueType::Bool) {
                error = "widget 'boolean' needs a bool parameter" + where;
                return false;
            }
            break;
        case ParameterWidget::Text:
        case ParameterWidget::Color:
            if (parameter.valueType != ValueType::String) {
                error = "widget 'text'/'color' needs a string parameter" + where;
                return false;
            }
            break;
        case ParameterWidget::Vector:
        case ParameterWidget::Point:
            if (parameter.valueType != ValueType::Point3) {
                error = "widget 'vector'/'point' needs a point3 parameter" + where;
                return false;
            }
            if (!ui.components.empty () && ui.components.size () != 3) {
                error = "a point3 widget names three components or none" + where;
                return false;
            }
            break;
        case ParameterWidget::Select:
            // Scalars only: an option is something a person picks off a list,
            // and a list-valued option has no readable label or stable identity.
            if (parameter.valueType == ValueType::List || parameter.valueType == ValueType::Absent) {
                error = "widget 'select' needs a scalar parameter" + where;
                return false;
            }
            break;
        case ParameterWidget::Auto:
        case ParameterWidget::ReadOnly:
            break;
    }

    // A slider must be bounded, by constants or by the parameters it names.
    // Otherwise there is nothing to drag along, and a client would have to
    // invent a range - which would then differ between clients.
    if (ui.widget == ParameterWidget::Slider) {
        const bool lower = ui.minimum.has_value () || !ui.minimumParameter.empty ();
        const bool upper = ui.maximum.has_value () || !ui.maximumParameter.empty ();
        if (!lower || !upper) {
            error = "widget 'slider' needs a minimum and a maximum" + where;
            return false;
        }
    }

    if (ui.minimum.has_value () && ui.maximum.has_value () && *ui.minimum > *ui.maximum) {
        error = "minimum is greater than maximum" + where;
        return false;
    }
    if (ui.step.has_value () && *ui.step <= 0.0) {
        error = "step must be positive" + where;
        return false;
    }
    if (ui.decimals.has_value () && (*ui.decimals < 0 || *ui.decimals > 15)) {
        error = "decimals must be between 0 and 15" + where;
        return false;
    }

    // A bound that names a sibling must name one that exists, is numeric, and is
    // not the parameter itself - a self-reference is a cycle a client would have
    // to resolve at render time.
    const std::pair<const std::string*, const char*> bounds[] = {
        { &ui.minimumParameter, "minimumParameter" },
        { &ui.maximumParameter, "maximumParameter" },
        { &ui.stepParameter, "stepParameter" },
        { &ui.decimalsParameter, "decimalsParameter" },
    };
    for (const auto& [named, what] : bounds) {
        if (named->empty ())
            continue;
        if (*named == parameter.id) {
            error = std::string (what) + " names its own parameter" + where;
            return false;
        }
        const ParameterSchema* sibling = FindParameter (nodeType, *named);
        if (sibling == nullptr || !IsNumeric (sibling->valueType)) {
            error = std::string (what) + " must name a numeric sibling parameter, not '" + *named + "'" + where;
            return false;
        }
    }

    if (!ui.options.empty () && ui.optionSource != ParameterOptionSource::None) {
        error = "a select takes literal options or an option source, not both" + where;
        return false;
    }
    if (ui.widget == ParameterWidget::Select && ui.options.empty () && ui.optionSource == ParameterOptionSource::None) {
        error = "widget 'select' needs options or an option source" + where;
        return false;
    }
    if ((!ui.options.empty () || ui.optionSource != ParameterOptionSource::None) &&
        ui.widget != ParameterWidget::Select) {
        error = "options belong to widget 'select'" + where;
        return false;
    }

    std::set<size_t> optionHashes;
    for (const ParameterOption& option : ui.options) {
        if (option.label.empty ()) {
            error = "an option has no label" + where;
            return false;
        }
        if (option.value.Type () != parameter.valueType) {
            error = "option '" + option.label + "' does not carry the parameter's value type" + where;
            return false;
        }
        // Identity is the VALUE, not the label: two options that submit the same
        // value make the control's selected state unanswerable.
        if (!optionHashes.insert (option.value.Hash ()).second) {
            error = "two options carry the same value" + where;
            return false;
        }
    }
    return true;
}

} // namespace

bool NodeRegistry::Register (NodeType nodeType, std::string& error)
{
    if (nodeType.id.empty ()) {
        error = "node type id is empty";
        return false;
    }
    if (types_.contains (nodeType.id)) {
        error = "node type already registered: " + nodeType.id;
        return false;
    }
    if (!ValidateIds (nodeType.inputs, "input", error) || !ValidateIds (nodeType.outputs, "output", error) ||
        !ValidateIds (nodeType.parameters, "parameter", error))
        return false;

    for (const ParameterSchema& parameter : nodeType.parameters) {
        if (parameter.defaultValue && parameter.defaultValue->Type () != parameter.valueType) {
            error = "default value type mismatch for parameter: " + parameter.id;
            return false;
        }
    }

    for (const ParameterSchema& parameter : nodeType.parameters) {
        if (!ValidateParameterUi (nodeType, parameter, error))
            return false;
    }

    // Stage F3. Checked HERE, once, rather than on every bypassed evaluation:
    // an ambiguous or ill-typed table is a defect in the node type, and a defect
    // in a node type should stop it entering the catalog rather than surface as
    // a puzzling run-time result later.
    std::set<std::string> mappedInputs;
    std::set<std::string> mappedOutputs;
    for (const BypassMapping& mapping : nodeType.bypassMappings) {
        const PortSchema* input = FindInput (nodeType, mapping.inputId);
        const PortSchema* output = FindOutput (nodeType, mapping.outputId);
        if (input == nullptr || output == nullptr) {
            error =
                "bypass mapping names a port this type does not have: " + mapping.inputId + " -> " + mapping.outputId;
            return false;
        }
        if (input->valueType != output->valueType) {
            error = "bypass mapping is not type-compatible: " + mapping.inputId + " -> " + mapping.outputId;
            return false;
        }
        // One output may be fed by exactly one input and vice versa. Two
        // mappings onto one output is the ambiguity the declaration exists to
        // remove.
        if (!mappedInputs.insert (mapping.inputId).second || !mappedOutputs.insert (mapping.outputId).second) {
            error =
                "bypass mapping is ambiguous; a port is mapped twice: " + mapping.inputId + " -> " + mapping.outputId;
            return false;
        }
    }
    // A partial table would leave a required output absent while bypassed, which
    // reads to a consumer as a broken node rather than a bypassed one.
    if (!nodeType.bypassMappings.empty ()) {
        for (const PortSchema& output : nodeType.outputs) {
            if (output.required && !mappedOutputs.contains (output.id)) {
                error = "bypass mapping leaves required output '" + output.id + "' unfed";
                return false;
            }
        }
    }

    types_.emplace (nodeType.id, std::move (nodeType));
    error.clear ();
    return true;
}

const NodeType* NodeRegistry::Find (const std::string& nodeTypeId) const
{
    const auto iterator = types_.find (nodeTypeId);
    return iterator == types_.end () ? nullptr : &iterator->second;
}

const PortSchema* FindInput (const NodeType& nodeType, const std::string& portId)
{
    for (const PortSchema& port : nodeType.inputs)
        if (port.id == portId)
            return &port;
    return nullptr;
}

const PortSchema* FindOutput (const NodeType& nodeType, const std::string& portId)
{
    for (const PortSchema& port : nodeType.outputs)
        if (port.id == portId)
            return &port;
    return nullptr;
}

const ParameterSchema* FindParameter (const NodeType& nodeType, const std::string& parameterId)
{
    for (const ParameterSchema& parameter : nodeType.parameters)
        if (parameter.id == parameterId)
            return &parameter;
    return nullptr;
}

} // namespace evp::nodegraph
