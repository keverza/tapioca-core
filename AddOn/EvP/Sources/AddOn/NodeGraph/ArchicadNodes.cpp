#include "NodeGraph/ArchicadNodes.hpp"

#include "NodeGraph/ArchicadHost.hpp"

#include <stdexcept>

namespace evp::nodegraph {
namespace {

constexpr const char kGetSelection[] = "archicad.getSelection";
constexpr const char kSetSelection[] = "archicad.setSelection";

Value::List ToValueList (const std::vector<ArchicadElementRef>& elements)
{
    Value::List list;
    list.reserve (elements.size ());
    for (const ArchicadElementRef& element : elements)
        list.emplace_back (element);
    return list;
}

} // namespace

void RegisterArchicadNodes (NodeRegistry& registry)
{
    std::string error;

    NodeType getSelection;
    getSelection.id = kGetSelection;
    getSelection.label = "Get Selection";
    getSelection.category = "Archicad";
    getSelection.description = "The elements currently selected in Archicad.";
    getSelection.executionDomain = ExecutionDomain::ArchicadMainThread;
    getSelection.effect = EffectKind::ReadModel;
    // Declaring these is what re-runs the node when the user selects something
    // else. Omitting them would cache the first answer forever.
    getSelection.generations = { GenerationDomain::Selection, GenerationDomain::Project };
    getSelection.outputs.push_back ({ "elements", "Elements", ValueType::List });
    getSelection.outputs.push_back ({ "count", "Count", ValueType::Integer });
    if (!registry.Register (std::move (getSelection), error))
        throw std::logic_error (error);

    NodeType setSelection;
    setSelection.id = kSetSelection;
    setSelection.label = "Set Selection";
    setSelection.category = "Archicad";
    setSelection.description = "Selects the given elements in Archicad. Runs only on an explicit run.";
    setSelection.executionDomain = ExecutionDomain::ArchicadMainThread;
    // HostUiWrite, not ReadModel: this is why the node is deferred to the second
    // phase and refused on a preview.
    setSelection.effect = EffectKind::HostUiWrite;
    setSelection.generations = { GenerationDomain::Project };
    setSelection.inputs.push_back ({ "elements", "Elements", ValueType::List });
    setSelection.outputs.push_back ({ "count", "Count", ValueType::Integer });
    if (!registry.Register (std::move (setSelection), error))
        throw std::logic_error (error);
}

bool IsArchicadNodeType (const std::string& nodeTypeId)
{
    return nodeTypeId == kGetSelection || nodeTypeId == kSetSelection;
}

bool ExecuteArchicadNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                          ValueMap& outputs, std::string& error)
{
    if (context.archicad == nullptr || !context.archicad->IsAvailable ()) {
        // Belt and braces: the plan already refuses this, so reaching here means
        // the project closed between planning and execution.
        error = "the Archicad project is no longer available";
        return false;
    }

    if (node.nodeType == kGetSelection) {
        std::vector<ArchicadElementRef> elements;
        if (!context.archicad->GetSelection (elements, error))
            return false;
        outputs.emplace ("count", Value (static_cast<int64_t> (elements.size ())));
        outputs.emplace ("elements", Value (ToValueList (elements)));
        return true;
    }

    if (node.nodeType == kSetSelection) {
        const auto found = inputs.find ("elements");
        if (found == inputs.end () || found->second.Type () != ValueType::List) {
            error = "the elements input must be a list";
            return false;
        }

        std::vector<ArchicadElementRef> elements;
        for (const Value& item : std::get<Value::List> (found->second.DataValue ())) {
            if (item.Type () != ValueType::ArchicadElementRef) {
                error = "the elements input must contain only Archicad element references";
                return false;
            }
            elements.push_back (std::get<ArchicadElementRef> (item.DataValue ()));
        }

        // Resolve BEFORE asking the host to change anything, and resolve them all
        // in ONE call - a per-element resolve would cross onto the host thread
        // once per element. A stale reference must fail this node, not silently
        // select the subset that still exists: a partial selection looks like a
        // correct answer and is not one.
        std::vector<Reference> references;
        references.reserve (elements.size ());
        for (const ArchicadElementRef& element : elements)
            references.push_back (Reference { ReferenceKind::Element, element.guid, {} });

        const std::vector<ReferenceResolution> resolutions = context.references->ResolveAll (references);
        for (size_t i = 0; i < resolutions.size (); ++i) {
            if (!resolutions[i].Usable ()) {
                error = "cannot select: " + resolutions[i].detail;
                return false;
            }
        }

        if (context.cancellation.IsCancelled ()) {
            error = "the evaluation was cancelled before the selection was applied";
            return false;
        }

        if (!context.archicad->SetSelection (elements, error))
            return false;
        outputs.emplace ("count", Value (static_cast<int64_t> (elements.size ())));
        return true;
    }

    error = "unknown Archicad node type: " + node.nodeType;
    return false;
}

} // namespace evp::nodegraph
