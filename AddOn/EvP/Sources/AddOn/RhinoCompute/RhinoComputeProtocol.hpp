#ifndef EVP_RHINOCOMPUTE_RHINOCOMPUTEPROTOCOL_HPP
#define EVP_RHINOCOMPUTE_RHINOCOMPUTEPROTOCOL_HPP

// The Resthopper wire: what Tapioca sends to compute.geometry and what it makes
// of the answer.
//
// This is the SECOND backend. The first is Tapioca.GhWorker.exe over a named
// pipe (Grasshopper/GhProtocol.hpp), which is stateful, interactive and binary.
// This one is stateless, headless and JSON over HTTP. They are ALTERNATIVES
// BEHIND ONE CONTRACT, not layers: a workflow that solves on one must solve on
// the other. See docs/architecture/api/SPEC-RhinoCompute.md.
//
// ⚠️ DevKit-free, Win32-free and httplib-free ON PURPOSE, exactly like
// GhProtocol.hpp. Everything here is string in, struct out, so the half that
// goes wrong silently — request shaping, response parsing, the error array
// nobody reads — can be proved offline with no Archicad, no Rhino and no
// socket. Process supervision lives in RhinoComputeManager, which is the half
// that cannot be proved that way.
//
// ⚠️ THE SERVER IS THE UNTRUSTED SIDE. compute.geometry runs third-party .gha
// code and answers over a socket; its JSON ends up driving a panel and a
// renderer inside Archicad.exe. Every parse below refuses malformed input with
// a named reason rather than returning a half-filled struct.

#include <cstdint>
#include <string>
#include <vector>

namespace evp {
namespace rhinocompute {

// Where an error came from, so the panel can say whether the user's definition
// is wrong or Tapioca is.
enum class ErrorSource {
    Tapioca,
    Compute,
    Rhino,
    Grasshopper,
};

struct Error {
    ErrorSource source = ErrorSource::Tapioca;
    std::string code;
    std::string message;
};

// What the panel does about it. A definition that names a missing input is the
// author's problem and stays on screen; a dead worker is ours and triggers a
// restart offer.
enum class Severity {
    Warning,
    RecoverableError,
    WorkflowValidationError,
    WorkerCrash,
};

// One public input, as POST /io reports it.
//
// ⚠️ `id` IS THE COMPUTE PARAMETER NAME, NOT A NICKNAME AND NOT AN INDEX.
// compute keys injection by name and refuses duplicates: three stock Hops "Get
// Number" components with unset nicknames made /io report ONE input and answer
// "Multiple input parameters with the same name were detected." twice, after
// which the solve returned empty trees. Measured, not assumed.
struct InputDescriptor {
    std::string id;
    std::string label;
    // number, integer, boolean, string, enum — the Tapioca schema names, mapped
    // from compute's ParamType. NOT compute's own spelling: the panel builds
    // controls from this and must not learn Grasshopper's vocabulary.
    std::string type;
    std::string group;
    int order = -1;

    bool hasMinimum = false;
    double minimum = 0.0;
    bool hasMaximum = false;
    double maximum = 0.0;

    // AtLeast from IGH_ContextualParameter: 0 means the definition solves
    // without it.
    bool required = true;

    std::vector<std::string> choices;
    std::string defaultValue;
};

struct WorkflowSchema {
    std::string workflowId;
    std::string name;
    // The md5_ key compute returns. THE DEBOUNCE BUDGET DEPENDS ON IT: measured
    // here, a solve carrying the 17 KB definition took 52 ms and the same solve
    // carrying this pointer took 4.5 ms. Upload once, then point.
    std::string pointer;
    std::vector<InputDescriptor> inputs;
    std::vector<Error> errors;
};

// One input value on its way to a solve. Values are carried as text and typed
// by `type` rather than as a variant: the panel reads DG controls as text
// anyway, and a double that round-trips through a locale-sensitive parse is a
// bug that only shows up on someone else's machine.
struct InputValue {
    std::string id;
    std::string type;
    std::string value;
};

enum class SolveMode {
    Schema,
    Preview,
    Final,
};

struct SolveRequest {
    // Either pointer OR definitionBase64 — pointer wins when both are set.
    std::string pointer;
    std::string definitionBase64;

    // Monotonic, assigned by the caller. A response carrying anything but the
    // latest is discarded rather than drawn; see RhinoComputeManager.
    uint64_t requestId = 0;
    SolveMode mode = SolveMode::Preview;
    std::vector<InputValue> inputs;
};

// A mesh as the preview path wants it, which is NOT how RhinoCommon would
// serialize itself.
//
// ⚠️ THE TAPIOCA PREVIEW OUTPUT COMPONENT EMITS EXPLICIT FLOAT ARRAYS, AND
// NOTHING HERE DECODES A RHINOCOMMON OBJECT. Compute serializes RhinoCommon
// geometry as an opennurbs archive; decoding that in the add-on would mean
// linking opennurbs into EvP.apx to draw a preview. It also would not match the
// pipe backend, whose GhPreviewProtocol already carries float32 vertices and
// indices. So the definition states its preview as numbers and both backends
// produce the same bytes. SPEC-RhinoCompute.md records this as the open
// question it closes.
struct PreviewMesh {
    uint64_t id = 0;
    std::vector<float> vertices; // xyz triples
    std::vector<float> normals;  // xyz triples, empty when the author sent none
    std::vector<uint32_t> indices;
};

struct SolveResult {
    uint64_t requestId = 0;
    bool success = false;
    // Echoed back by compute; lets a caller that sent a definition learn the
    // pointer to use next time.
    std::string pointer;
    // From the definition, e.g. "Millimeters". MUST be reconciled against the
    // Archicad project before any value becomes a dimension.
    std::string modelUnits;
    std::vector<PreviewMesh> meshes;
    std::vector<Error> errors;
    std::vector<std::string> warnings;
};

// ── request shaping ──────────────────────────────────────────────────────────

// Body for POST /io. Discovery always carries the definition: asking for a
// schema by pointer would answer from a cache that may predate an edit.
std::string BuildIoRequest (const std::string& definitionBase64);

// Body for POST /grasshopper.
std::string BuildSolveRequest (const SolveRequest& request);

// ── response parsing ─────────────────────────────────────────────────────────

// Reads POST /io. Returns false and fills `schema.errors` on malformed JSON;
// a WELL-FORMED response that itself reports errors returns TRUE with those
// errors carried, because "compute answered, and it says your definition is
// wrong" is not a transport failure.
bool ParseIoResponse (const std::string& json, WorkflowSchema& schema);

// Reads POST /grasshopper, keeping only the outputs the preview path names.
bool ParseSolveResponse (const std::string& json, SolveResult& result);

// ── helpers the parsers and the manager share ────────────────────────────────

// True when the body is compute's "no child is up" refusal rather than an
// answer. GET /healthcheck CANNOT be used for this: the parent answers
// "Healthy" for a full minute while it has zero working children.
bool IsNoServerResponse (const std::string& body);

// Maps compute's ParamType onto the Tapioca schema vocabulary. Unknown types
// map to an empty string, which the caller must treat as unsupported rather
// than as text.
std::string SchemaTypeForParamType (const std::string& paramType);

std::string ToBase64 (const std::vector<uint8_t>& bytes);

} // namespace rhinocompute
} // namespace evp

#endif
