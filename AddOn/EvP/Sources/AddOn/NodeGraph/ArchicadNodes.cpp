#include "NodeGraph/ArchicadNodes.hpp"

#include "NodeGraph/ArchicadHost.hpp"
#include "NodeGraph/ElementClassification.hpp"
#include "NodeGraph/Json.hpp"
#include "NodeGraph/ParameterDescriptors.hpp"

#include <stdexcept>

namespace evp::nodegraph {
namespace {

constexpr const char kGetSelection[] = "archicad.getSelection";
constexpr const char kSetSelection[] = "archicad.setSelection";
constexpr const char kContainerPrefix[] = "archicad.container.";
constexpr const char kLibraryPart[] = "archicad.libraryPart";

Value::List ToValueList (const std::vector<ArchicadElementRef>& elements)
{
    Value::List list;
    list.reserve (elements.size ());
    for (const ArchicadElementRef& element : elements)
        list.emplace_back (element);
    return list;
}

Value::List ToStringList (const std::vector<std::string>& values)
{
    Value::List list;
    list.reserve (values.size ());
    for (const std::string& value : values)
        list.emplace_back (value);
    return list;
}

} // namespace

const char* const kSelectionSetNodeType = kGetSelection;
const char* const kSelectionSetParameter = "elements";
const char* const kSelectionTypesParameter = "elementTypes";
const char* const kElementContainerPrefix = kContainerPrefix;

std::string ElementContainerNodeType (const std::string& elementTypeId)
{
    const ElementTypeDescriptor* type = FindElementType (elementTypeId);
    if (type == nullptr || !type->container)
        return {};
    return std::string (kContainerPrefix) + type->id;
}

std::string ElementTypeOfContainerNode (const std::string& nodeTypeId)
{
    const std::string prefix (kContainerPrefix);
    if (nodeTypeId.rfind (prefix, 0) != 0)
        return {};
    const std::string elementType = nodeTypeId.substr (prefix.size ());
    const ElementTypeDescriptor* type = FindElementType (elementType);
    return (type != nullptr && type->container) ? elementType : std::string {};
}

std::vector<std::string> TypesFromValue (const Value& value)
{
    std::vector<std::string> types;
    if (value.Type () != ValueType::List)
        return types;
    for (const Value& item : std::get<Value::List> (value.DataValue ())) {
        types.push_back (item.Type () == ValueType::String ? std::get<std::string> (item.DataValue ())
                                                           : std::string (kUnclassifiedElementTypeId));
    }
    return types;
}

Value ValueFromTypes (const std::vector<std::string>& types)
{
    return Value (ToStringList (types));
}

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

void RegisterElementContainers (NodeRegistry& registry);

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
    getSelection.description = "A set of Archicad elements you capture. Update replaces it with the current selection, "
                               "Add and Remove change it, Reselect selects it in Archicad, Clear empties it.";
    getSelection.executionDomain = ExecutionDomain::Worker;
    getSelection.effect = EffectKind::Pure;
    getSelection.display = NodeDisplay::SelectionSet;
    // NO DEFAULT VALUE, deliberately. An absent parameter already means an
    // empty set - ExecuteArchicadNode reads it that way - so a default would be
    // a second spelling of the same state, and it would put a list into the
    // catalog's defaultValue where every other node has a scalar.
    getSelection.parameters.push_back ({ kSelectionSetParameter, "Elements", ValueType::List, false });
    // Parallel to the guids, one type id each. See kSelectionTypesParameter for
    // why the type is captured rather than looked up.
    getSelection.parameters.push_back ({ kSelectionTypesParameter, "Element Types", ValueType::List, false });
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

    // ONE GDL LIBRARY PART, chosen from what the project has loaded.
    //
    // ⚠️ Pure/Worker AND IT READS NOTHING, exactly like the selection set and for
    // the same reason: the choice is a thing the USER made and stored, not a
    // question about the model. Evaluating it parses its own parameter, so a
    // saved graph names its objects with no project open - which is what makes a
    // workflow portable between files.
    //
    // ⚠️ THE PARAMETER CARRIES THE PALETTE'S OWN JSON, not just a name. The
    // catalogue's header says it plainly: a document name is unique only in the
    // sense that Archicad registers the NEWEST part carrying it, so two loaded
    // libraries shipping "Chair 01" leave one of them invisible. The unID is what
    // survives a session and a library reload, so it travels with the label -
    // and it travels in the SAME shape Palette/CatalogPicker.cpp stores, so a
    // part means one thing in the palette and in the graph.
    NodeType libraryPart;
    libraryPart.id = kLibraryPart;
    libraryPart.label = "Library Part";
    libraryPart.category = "Archicad";
    libraryPart.description =
        "One GDL object chosen from the loaded libraries. Browse the folders Object Settings shows.";
    libraryPart.executionDomain = ExecutionDomain::Worker;
    libraryPart.effect = EffectKind::Pure;
    {
        ParameterSchema part { "part", "Object", ValueType::String, false, Value (std::string {}) };
        ParameterUi ui;
        ui.widget = ParameterWidget::LibraryPart;
        ui.section = "Object";
        ui.help = "The loaded libraries, in the folders Archicad's own Object Settings shows.";
        part.ui = ui;
        libraryPart.parameters.push_back (std::move (part));
    }
    // Split out for downstream use rather than left as one opaque blob: a graph
    // that wants to place this object needs the NAME, and one that wants to
    // record which object it was needs the unID. Making a consumer parse JSON out
    // of a string output would put a second parser in every such node.
    libraryPart.outputs.push_back ({ "name", "Name", ValueType::String });
    libraryPart.outputs.push_back ({ "unID", "Unique ID", ValueType::String });
    libraryPart.outputs.push_back ({ "type", "Type", ValueType::String });
    if (!registry.Register (std::move (libraryPart), error))
        throw std::logic_error (error);

    RegisterElementContainers (registry);
}

// ---------------------------------------------------------------------------
// THE CONTAINERS. One node per native element type, generated from
// ElementClassification's catalog rather than written out fifteen times.
//
// ⚠️ GENERATED, BECAUSE THE ALTERNATIVE IS FIFTEEN NEAR-IDENTICAL BLOCKS that
// drift. A Wall container whose port was called "elements" and a Slab container
// whose port was called "items" would make every graph type-specific for no
// reason a user could see, and the fifteenth block is where the typo lives.
// Adding a container is now one `container = true` in the table.
//
// ⚠️ AND THEY ARE ReadModel, NOT Pure, WHICH THE SELECTION SET IS NOT. Deciding
// whether an element is a wall means asking Archicad, so a container genuinely
// depends on host state and says so by declaring the Project generation. That is
// the difference between the two nodes: a captured set is a thing the user
// holds, a container is a question about the model, and a question has to be
// asked again when the model moves.
void RegisterElementContainers (NodeRegistry& registry)
{
    std::string error;
    for (const ElementTypeDescriptor& type : ElementTypeCatalog ()) {
        if (!type.container)
            continue;

        NodeType container;
        container.id = std::string (kContainerPrefix) + type.id;
        container.label = type.plural;
        container.category = "Archicad Elements";
        container.description = "Keeps only the " + type.plural + " out of the elements given to it.";
        container.executionDomain = ExecutionDomain::ArchicadMainThread;
        container.effect = EffectKind::ReadModel;
        container.generations = { GenerationDomain::Project };
        // NOT REQUIRED. A container is a thing you drop on the canvas and wire
        // up afterwards; an unwired one is EMPTY, and reporting a broken graph
        // for the seconds between those two acts would be noise.
        container.inputs.push_back ({ "elements", "Elements", ValueType::List, false });
        container.outputs.push_back ({ "elements", type.plural, ValueType::List });
        container.outputs.push_back ({ "count", "Count", ValueType::Integer });
        // NO BYPASS, and the registry is what settled it. Passing the input
        // through is the only reading of bypass that means anything for a node
        // whose job is to remove things - but a bypass table must feed EVERY
        // output, and `count` has no input to come from. Rather than drop the
        // count (which is the whole point of a container in a stack) or invent a
        // number for it, the type simply is not bypassable, which is the default.
        if (!registry.Register (std::move (container), error))
            throw std::logic_error (error);
    }
}

bool IsArchicadNodeType (const std::string& nodeTypeId)
{
    return nodeTypeId == kGetSelection || nodeTypeId == kSetSelection || nodeTypeId == kLibraryPart ||
           !ElementTypeOfContainerNode (nodeTypeId).empty ();
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
            stored == node.parameters.end () ? std::vector<ArchicadElementRef> {} : ElementsFromValue (stored->second);
        outputs.emplace ("count", Value (static_cast<int64_t> (elements.size ())));
        outputs.emplace ("elements", ValueFromElements (elements));
        return true;
    }

    // Reads its own parameter and nothing else, so it runs with no host at all.
    if (node.nodeType == kLibraryPart) {
        const auto stored = node.parameters.find ("part");
        std::string encoded;
        if (stored != node.parameters.end () && stored->second.Type () == ValueType::String)
            encoded = std::get<std::string> (stored->second.DataValue ());

        // An unchosen part is EMPTY, not an error: the node is something you drop
        // and then browse from, and a red node for the seconds in between is
        // noise. A blob that will not parse is treated the same way rather than
        // failing the graph - it can only come from a hand-edited file, and
        // losing the choice is better than refusing to open the document.
        std::string name, unID, type;
        if (!encoded.empty ()) {
            const json::ParseResult parsed = json::Parse (encoded);
            if (parsed.ok) {
                const auto read = [&parsed] (const char* key, std::string& out) {
                    const json::JsonValue* found = parsed.value.Find (key);
                    if (found != nullptr)
                        found->AsString (out);
                };
                read ("name", name);
                read ("unID", unID);
                read ("type", type);
            }
        }
        outputs.emplace ("name", Value (name));
        outputs.emplace ("unID", Value (unID));
        outputs.emplace ("type", Value (type));
        return true;
    }

    if (context.archicad == nullptr || !context.archicad->IsAvailable ()) {
        // Belt and braces: the plan already refuses this, so reaching here means
        // the project closed between planning and execution.
        error = "the Archicad project is no longer available";
        return false;
    }

    const std::string containerType = ElementTypeOfContainerNode (node.nodeType);
    if (!containerType.empty ()) {
        const auto found = inputs.find ("elements");
        std::vector<ArchicadElementRef> candidates;
        if (found != inputs.end ())
            candidates = ElementsFromValue (found->second);

        // An unwired container is EMPTY, not an error. It is a thing you drop on
        // the canvas and wire up afterwards, and a node that reported a failure
        // for the ten seconds between those two acts would be noise.
        std::vector<ArchicadElementRef> kept;
        if (!candidates.empty ()) {
            std::vector<ElementDescription> descriptions;
            // ONE read for the whole input list; see IArchicadHost.
            if (!context.archicad->DescribeElements (candidates, descriptions, error))
                return false;
            for (const ElementDescription& description : descriptions) {
                // An element that could not be read is NOT kept. "I could not
                // tell what this is" must not answer "it is a wall": a container
                // that quietly admitted unreadable elements would hand them
                // downstream to nodes that assume the type.
                if (!description.available || description.elementType != containerType)
                    continue;
                kept.push_back (ArchicadElementRef { description.guid });
            }
        }

        outputs.emplace ("elements", ValueFromElements (kept));
        outputs.emplace ("count", Value (static_cast<int64_t> (kept.size ())));
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
