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

const char* const kSelectionSetNodeType = kGetSelection;
const char* const kSelectionSetParameter = "elements";

std::vector<ArchicadElementRef> ElementsFromValue (const Value& value)
{
    std::vector<ArchicadElementRef> elements;
    if (value.Type () != ValueType::List)
        return elements;
    for (const Value& item : std::get<Value::List> (value.DataValue ())) {
        if (item.Type () == ValueType::ArchicadElementRef)
            elements.push_back (std::get<ArchicadElementRef> (item.DataValue ()));
    }
    return elements;
}

Value ValueFromElements (const std::vector<ArchicadElementRef>& elements)
{
    return Value (ToValueList (elements));
}

void RegisterArchicadNodes (NodeRegistry& registry)
{
    std::string error;

    // A SELECTION SET the user captures, not a live mirror of Archicad's
    // selection - the command palette's Update/Add/Remove/Reselect/Clear rows,
    // as a node.
    //
    // ⚠️ THIS IS WHY IT IS Pure/Worker AND DECLARES NO GENERATION. The node's
    // output IS its stored parameter, so evaluating it reads nothing from the
    // host. That is the whole behavioural point: a graph whose source tracked
    // the live selection silently changed its answer every time the user
    // clicked in the model, and every downstream node went dirty with it. A
    // captured set changes only when the user presses one of its buttons, and
    // those buttons evaluate what they affect on the spot - so nobody has to
    // press Evaluate to see the result of pressing Update.
    //
    // It also means the set PERSISTS with the graph: it is an ordinary
    // parameter, so it saves, loads and round-trips with no extra machinery,
    // and §7.2's rule holds - references stay references and are re-resolved
    // when something actually uses them.
    NodeType getSelection;
    getSelection.id = kGetSelection;
    getSelection.label = "Get Selection";
    getSelection.category = "Archicad";
    getSelection.description =
        "A set of Archicad elements you capture. Update replaces it with the current selection, "
        "Add and Remove change it, Reselect selects it in Archicad, Clear empties it.";
    getSelection.executionDomain = ExecutionDomain::Worker;
    getSelection.effect = EffectKind::Pure;
    getSelection.display = NodeDisplay::SelectionSet;
    // NO DEFAULT VALUE, deliberately. An absent parameter already means an
    // empty set - ExecuteArchicadNode reads it that way - so a default would be
    // a second spelling of the same state, and it would put a list into the
    // catalog's defaultValue where every other node has a scalar.
    getSelection.parameters.push_back ({ kSelectionSetParameter, "Elements", ValueType::List, false });
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
    // The selection set evaluates to what it holds. No host, no project, no
    // generation - which is what lets it run offline and stay clean while the
    // user clicks around in the model.
    if (node.nodeType == kGetSelection) {
        const auto stored = node.parameters.find (kSelectionSetParameter);
        const std::vector<ArchicadElementRef> elements =
            stored == node.parameters.end () ? std::vector<ArchicadElementRef> {}
                                             : ElementsFromValue (stored->second);
        outputs.emplace ("count", Value (static_cast<int64_t> (elements.size ())));
        outputs.emplace ("elements", ValueFromElements (elements));
        return true;
    }

    if (context.archicad == nullptr || !context.archicad->IsAvailable ()) {
        // Belt and braces: the plan already refuses this, so reaching here means
        // the project closed between planning and execution.
        error = "the Archicad project is no longer available";
        return false;
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
