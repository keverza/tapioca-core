#ifndef GEOMETRYSERVER_PALETTE_INPUTMODEL_HPP
#define GEOMETRYSERVER_PALETTE_INPUTMODEL_HPP

// The neutral input model: what a workflow declares, in terms no runtime owns.
//
// DELIBERATELY FREE OF THE DEVKIT, exactly like Palette/CommandFilter and
// Palette/ParamVisibility. ParamPanel converts at the boundary and builds DG
// controls from this; nothing here knows what a DG::Item is, and nothing here
// knows what a Grasshopper parameter is either. That second half is the point.
//
// ⚠️ THIS EXISTS SO THE PANEL HAS ONE VOCABULARY, NOT TWO.
// HANDOFF-GHHost.md §2, "Native typed controls": extract a neutral input model
// and do not fabricate a Python command for it. Today the palette builds rows
// from CommandInfo, which is a Python command's signature; a Grasshopper
// workflow is not one, and inventing a fake CommandInfo to reuse the row builder
// would put a Python-shaped lie in the middle of the GH path — a command that
// does not exist, a scanner entry nothing scanned, and a run() nothing can call.
// The model below is what both can be converted INTO.
//
// ⚠️ VALIDATION LIVES HERE, NOT AT THE CONTROL. A range is declared by the
// definition's author and enforced before a solve is requested, because the
// alternative is a round trip to Rhino that comes back saying the author's own
// slider bounds were exceeded. It is also why Coerce takes and returns text: the
// wire carries the neutral text form (GhSessionProtocol.hpp), and a model that
// parsed values into typed fields would have to render them back to send them.

#include <string>
#include <vector>

namespace evp {

// The five primitive input types Tapioca's Grasshopper package declares, and the
// only ones the panel promises to build a control for. A type this build does
// not know is kept as Unsupported rather than dropped: the user has to be able
// to see that the definition asks for something this Tapioca cannot offer, and a
// row that silently vanished would look like a definition with fewer inputs.
enum class InputKind {
    Number,
    Integer,
    Boolean,
    Text,
    Enum,
    Unsupported,
};

const char* DescribeInputKind (InputKind kind);

// The schema's spelling of a type, as TapiocaInputSchema.ToJson writes it.
// Unknown spellings become Unsupported.
InputKind InputKindFromName (const std::string& name);

struct InputControl {
    // The stable id the panel keys its control by and the wire carries. Never
    // shown to the user; `label` is.
    std::string id;
    std::string label;
    std::string group;
    InputKind kind = InputKind::Text;

    // Resolved by the worker's discovery, which already applied group-then-order
    // -then-canvas-position. Kept so the panel can sort without re-deriving a
    // rule that lives in the definition's own author's intent.
    int order = 0;

    bool required = false;

    // Declared bounds. Both optional and independently so: an author who set
    // only a minimum meant only a minimum.
    bool hasMinimum = false;
    double minimum = 0.0;
    bool hasMaximum = false;
    double maximum = 0.0;

    // Enum rows in author order. Empty for every other kind.
    std::vector<std::string> choices;

    // The value the definition currently holds, as the schema expressed it.
    // This is the panel's initial value, and it is deliberately NOT assumed
    // valid: a definition can be saved with a slider outside bounds its author
    // narrowed afterwards.
    std::string defaultValue;

    // The type name as written, kept only for Unsupported so the panel can say
    // WHICH type it cannot offer.
    std::string declaredType;
};

// One definition's whole input contract.
struct InputModel {
    std::string workflowId;
    std::string name;

    // What the definition's author says it does, from a Tapioca Description
    // component. Empty when the definition carries none -- which is the
    // ordinary case and not an error.
    std::string description;

    std::vector<InputControl> controls;

    // What discovery refused, verbatim from the worker. These are the author's
    // problems -- a duplicate id, an enum with no choices, a minimum above a
    // maximum -- and the panel shows them rather than hiding them behind an
    // empty row list.
    std::vector<std::string> errors;

    // Whether the schema itself parsed. False means `errors` holds the reason
    // and `controls` is empty; it does NOT mean the definition is bad.
    bool parsed = false;
};

// Parses the WorkflowSchema JSON the worker sends.
//
// ⚠️ NEVER THROWS AND NEVER PARTIALLY FILLS. A schema that will not parse
// produces a model with parsed=false and one error, because the panel's next act
// is to render whatever comes back and a half-built model would render as a
// definition with some of its inputs missing -- the one failure mode that looks
// like a working panel.
InputModel ParseInputModel (const std::string& schemaJson);

// Renders a double the way every value this model sends is rendered: invariant,
// shortest form that reads back identically, and never an exponent unless the
// number genuinely needs one. One spelling per value, in one place, because the
// panel's hint, the wire's value and the input-snapshot hash all have to agree.
std::string FormatInputNumber (double value);

// The result of checking one value against one control.
struct CoerceResult {
    bool ok = false;
    // The value to send, normalised: "true"/"false" for a boolean, an
    // invariant-formatted number, the exact choice text for an enum.
    std::string value;
    // Empty when ok. A sentence naming the control's LABEL, because the id is
    // not what the user is looking at.
    std::string error;
};

// Checks and normalises one value for one control.
//
// ⚠️ NORMALISED, NOT MERELY CHECKED, AND THAT IS WHY IT RETURNS A STRING. The
// worker's parameters convert text themselves, but two spellings of one value
// ("True" and "true", "1.50" and "1.5") would hash to two different input
// snapshots and make the change detector report a change the user did not make.
// One spelling per value is what makes the snapshot hash mean anything.
CoerceResult Coerce (const InputControl& control, const std::string& text);

// Checks a whole snapshot, in the model's own order. Returns true when every
// value was accepted; `values` is filled with the normalised text either way, so
// a caller can show every bad row at once rather than the first one.
bool CoerceAll (const InputModel& model, const std::vector<std::string>& texts, std::vector<std::string>& values,
                std::vector<std::string>& errors);

} // namespace evp

#endif
