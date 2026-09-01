#ifndef EVP_NODEGRAPH_NODETYPE_HPP
#define EVP_NODEGRAPH_NODETYPE_HPP

#include "NodeGraph/ProjectGenerations.hpp"
#include "NodeGraph/Value.hpp"

#include <optional>
#include <string>
#include <vector>

namespace evp::nodegraph {

// WHERE a node runs.
enum class ExecutionDomain {
    Worker,
    ArchicadMainThread,
    RenderThread,
};

const char* ExecutionDomainName (ExecutionDomain domain);

// WHAT a node does to the world outside the graph. Deliberately separate from
// ExecutionDomain: the executor picks a thread from the domain, and the
// evaluator decides scheduling and permission from the effect.
enum class EffectKind {
    // Computes from its inputs and nothing else.
    Pure,

    // Reads host state. Safe on any evaluation, including a preview - reading
    // cannot surprise the user.
    ReadModel,

    // Changes what the user sees, without changing the model. Selection is the
    // only member today. These are DEFERRED and permissioned: see
    // EvaluationRequest::allowSideEffects.
    HostUiWrite,
};

const char* EffectKindName (EffectKind effect);

// Deliberately no WriteModel. ADR-007 excludes model writes from this track, and
// an unused enum value is an invitation. The write pipeline is designed for in
// the architecture document sections 26-27 and arrives with the value.

// How a client should draw the node's body.
//
// A hint, not a command: a client is free to ignore it. It exists because the
// editor was branching on node id - `nodeType === 'watch'` - which means every
// new inspectable node needs a frontend change, and a renamed node silently
// loses its rendering. The runtime already owns the schema; it should own this
// too.
enum class NodeDisplay {
    // Ports and status only. The default.
    Ports,

    // The node has something to READ. A client should show the node's `text`
    // output in the body, scrollable. This is the Grasshopper-panel shape.
    Text,

    // The node has something to LOOK at; a client may host a viewport.
    Preview,

    // The node OWNS a set of Archicad elements and is operated on directly:
    // Update, Add, Remove, Reselect, Clear, exactly as the command palette's
    // selection rows. A client should draw those five actions and the set's
    // size. The actions are native verbs, not evaluations - see
    // NodeGraphSelectionCommands.cpp.
    SelectionSet,
};

const char* NodeDisplayName (NodeDisplay display);

// ---------------------------------------------------------------------------
// UI-1. How a parameter is EDITED, as a projection of the node type.
//
// A closed native-validated enum, never a component name: the widget vocabulary
// is a contract between the runtime and every client, and a client that could
// name its own component would make the catalog a frontend config file. See
// docs/architecture/api/HANDOFF-NodeGraphUIBuilder.md section 4.
//
// Metadata is a DISPLAY AND INPUT hint only. It never changes a parameter's
// ValueType, never makes a required parameter optional, and the evaluator never
// reads it - a node clamped by its own body stays clamped whether or not a
// client honoured the range.
enum class ParameterWidget {
    // No opinion. The client picks from the ValueType, which is what every
    // parameter registered before this existed still does.
    Auto,

    // A number typed into a field. `minimum`, `maximum`, `step` and `decimals`
    // apply.
    Number,

    // The same number, dragged. Requires a bounded range, because a slider
    // without one has nothing to drag along.
    Slider,

    // A bool, as a switch.
    Boolean,

    // One value chosen from `options`, or from what `optionSource` enumerates.
    Select,

    // A single line of text.
    Text,

    // Point3, as one row of component fields - the shape the reference image
    // shows. `components` names the axes.
    Vector,

    // Point3, likewise, but positional: a client may offer a pick-in-model
    // affordance for a point where it would not for a direction.
    Point,

    // A canonical hex colour string. No new ValueType: the value stays String.
    Color,

    // Shown, not edited. The fallback for a value a client cannot author.
    ReadOnly,
};

const char* ParameterWidgetName (ParameterWidget widget);

// Where a Select's options come from when they are not literal.
//
// ⚠️ THE BROWSER NEVER ENUMERATES A MODEL DOMAIN. A layer list is Archicad's to
// answer, and it changes with the open project, so it cannot live in a static
// catalog either. The parameter names the DOMAIN; a client asks the native
// attribute listing for the members. Same names-not-indices policy the palette's
// pickers already follow - the value carried is the attribute NAME (a Pen is the
// exception and carries its number).
enum class ParameterOptionSource {
    None,
    Layer,
    Pen,
    Fill,
    LineType,
    Surface,
    BuildingMaterial,
    Composite,
    Profile,
};

const char* ParameterOptionSourceName (ParameterOptionSource source);

struct ParameterOption {
    std::string label;
    Value value;
};

struct ParameterUi {
    ParameterWidget widget = ParameterWidget::Auto;

    // A flat grouping key. Deliberately not a path and not a tree: recursive
    // layout is a separate decision and an unusable one to guess at now.
    std::string section;
    int order = 0;
    std::string help;

    // Drawn after the field - "mm", "deg". Presentation only: the value is
    // always in the runtime's own units.
    std::string unit;

    std::optional<double> minimum;
    std::optional<double> maximum;
    std::optional<double> step;
    std::optional<int> decimals;

    // A sibling parameter supplying the live bound instead of the constant
    // above. This is what makes a slider whose range and precision the USER
    // controls expressible without the client branching on nodeType: the range
    // is three more ordinary parameters, and the slider says which.
    std::string minimumParameter;
    std::string maximumParameter;
    std::string stepParameter;
    std::string decimalsParameter;

    // Axis labels for Vector and Point. Two or three entries.
    std::vector<std::string> components;

    // Literal options. Mutually exclusive with optionSource.
    std::vector<ParameterOption> options;
    ParameterOptionSource optionSource = ParameterOptionSource::None;
};

struct PortSchema {
    std::string id;
    std::string label;
    ValueType valueType = ValueType::Absent;
    bool required = true;
    bool acceptsMultiple = false;
};

struct ParameterSchema {
    std::string id;
    std::string label;
    ValueType valueType = ValueType::Absent;
    bool required = false;
    std::optional<Value> defaultValue;

    // Absent is valid and is the state every parameter registered before UI-1
    // is still in: a client falls back to a control derived from the ValueType.
    std::optional<ParameterUi> ui;
};

// Stage F3: one input forwarded to one output while the node is bypassed.
//
// ⚠️ THE MAPPING IS DECLARED BY THE TYPE, NEVER INFERRED. A node with two
// number inputs and one number output has two type-compatible candidates and no
// correct answer; letting the editor or the evaluator pick one would make bypass
// mean something different on each node it was applied to. A type that cannot
// say unambiguously what bypass means simply cannot be bypassed.
struct BypassMapping {
    std::string inputId;
    std::string outputId;
};

struct NodeType {
    std::string id;
    std::string label;
    std::string category;
    std::string description;
    ExecutionDomain executionDomain = ExecutionDomain::Worker;
    EffectKind effect = EffectKind::Pure;
    NodeDisplay display = NodeDisplay::Ports;

    // Empty means the type cannot be bypassed, which is the default: bypass is
    // opt-in per type. NodeRegistry::Register refuses a table that names an
    // unknown port, mismatches value types, or covers one port twice - so an
    // unambiguous table is a property of the registry rather than something the
    // evaluator re-checks on every run.
    std::vector<BypassMapping> bypassMappings;

    // Stage F4: whether ExecutionMode::Holding is legal for this type. A Data
    // Dam is a contract about staging a value, not a badge every node can wear.
    bool holdCapable = false;

    // Host state this node reads that its ports do not express. Folded into the
    // node's cache key, so declaring a domain is what makes the node re-run when
    // that domain changes - and failing to declare one is what makes it serve a
    // stale answer.
    GenerationSet generations;

    std::vector<PortSchema> inputs;
    std::vector<PortSchema> outputs;
    std::vector<ParameterSchema> parameters;
};

} // namespace evp::nodegraph

#endif
