# Notebook Specification

Status: canonical specification for `internal work item`. Live task state is in
`private development tasks/notebook.yaml`; the UI technology decision is deliberately open in
`private development decisions/ADR-005-notebook-ui-technology.md`.

## Scope And Ownership

`internal work item` owns the workflow and command-instance model, input bindings, aliases and ordering,
typed reference modes, validation states, the Notebook shell, and project-scoped internalized
references. It is a consumer of the existing command and API surfaces, not a second implementation
of either.

The dependency direction is:

```text
NOTEBOOK -> COMMANDS, API
NOTEBOOK -> WEBUI   (only if the webview candidate wins)
```

Notebook does not own Archicad extraction, analysis algorithms, evaluation, a second command
runtime, or a new API surface. A notebook command is an ordinary `internal work item` command, and typed
Archicad selectors resolve through `internal work item`.

## Workflow Model

A workflow contains metadata, workflow inputs, an ordered collection of command instances, and
outputs. A command instance has a stable instance ID, an immutable command type, a user-editable
alias, and named input bindings:

```text
Workflow
  metadata
  inputs
  commands[]
    id
    commandType
    alias
    inputs{}
  outputs
```

The alias changes how a command is presented without changing the command definition. The workflow
representation remains normal editable Python so it can be read, debugged, and edited in VS Code or
by a coding agent. Existing monolithic scripts may first appear as one command and be decomposed
later without changing the workflow boundary.

The Notebook is a linear document model, not a free-canvas node editor. Sections, comments,
variables, command aliases, and outputs remain readable in a vertical document. Bindings may expose
dependency relationships, but the MVP does not add reactive recomputation or a separate graph
runtime.

## Command And Schema Contract

Commands are small reusable Python operations. They may use the official Archicad Python API, Tapir,
pure Python, or an existing Tapioca C++ API through the normal command/API boundary. The Notebook
does not care which provider supplies a command and does not fill provider gaps locally.

Pydantic models are the shared contract between Python and the UI. They provide:

- validation before execution;
- input and output type discovery;
- the property tree shown to users; and
- UI generation from typed fields.

Output fields retain semantic names and types. A result such as a list of element references,
distances, or projected points can be selected by property path rather than by an opaque serialized
value.

## Input Binding

Every command parameter receives a value from one of these sources:

- `Constant`;
- `WorkflowInput`;
- `PreviousCommandOutput`;
- `PreviousCommandProperty`; or
- `ArchicadReference`.

Property bindings are direct semantic paths. For example, `centers -> elements -> center` is a
binding to a named output property, not a wire coordinate or a raw dictionary lookup. The picker
shows only values compatible with the parameter's expected type.

## Typed References And Modes

The UI uses semantic reference types rather than generic strings or GUIDs. The initial family
includes `ElementRef`, `PolylineRef`, `PropertyRef`, `ViewRef`, `LayoutRef`, `LayerRef`, `PenRef`,
`FavoriteRef`, `SurfaceRef`, `BuildingMaterialRef`, `CompositeRef`, and `ProfileRef` as the
corresponding commands require them.

An `ArchicadReference` has one of three modes:

- `Pick`: select a project object explicitly;
- `Current`: use the current Archicad selection or context; or
- `Internalized`: resolve a stored project-specific reference.

Internalized references are stored by workflow and semantic role, such as `elements` or `path`, and
resolve only in the project that owns them. Opening the same Python workflow in another PLN does not
carry its internalized GUIDs across. A deleted or project-invalid reference remains visible as a
repairable warning rather than becoming an arbitrary new object.

## Validation And Execution

The MVP lifecycle is explicit:

```text
edit -> validate -> Run -> execute command instances -> results
```

Validation distinguishes valid input, missing/deleted/project-invalid references, and execution
errors. Required unresolved inputs show a warning and disable `Run` until repaired. The workflow
remains editable while invalid. Execution reports per-command status and duration, and `Cancel`
attempts to stop the active Python workflow through the normal runtime cancellation path.

There is no reactive recomputation in the MVP. The ordered command stack is the user-facing and
execution model; any future dependency-driven scheduling or non-sequential dataflow must be a
separate decision rather than an implicit second runtime.

## Persistence Split

Reusable workflow logic is stored in normal editable Python, including metadata, command types,
aliases, ordering, inputs, outputs, and defaults. Project-specific internalized references are
stored in Archicad project data and are keyed by workflow and semantic input role. These scopes must
not merge.

Small local configuration may store the workflow folder, recent workflows, palette sizing, and
server settings. No database is required for the MVP.

## UI Shell And Technology Gate

The shell presents workflow discovery/search, description and status, typed inputs, an ordered
command stack, outputs, validation feedback, and explicit Run/Cancel controls. The layout remains a
single-column notebook with sections and an outline; it does not require wires, a minimap, arbitrary
canvas positioning, or a node graph.

The shell contract is independent of its rendering technology. A single-column ImGui palette and a
`DG::Browser` webview with a vanilla TypeScript/Vite artifact are both candidates. The isolated
`internal work item` experiment is evidence for the choice, not a production workflow backend;
it must not gain a JavaScript-to-ACAPI bridge, Python execution, persistence, or model writes while
the technology ADR remains open.

## First Workflow Shape

The initial useful workflow demonstrates typed selection and direct output binding:

```text
elements + path
    -> centers: Object Center Points
    -> sorted: Sort Along Polyline
    -> numbers: Generate Sequence
    -> result: Set Element ID
```

Object/library-part centers use the appropriate insertion or origin point; other supported elements
use a bounding-box center. A polyline's stored start and direction define distance-zero and ordering.
This example exercises the model contract without making the Notebook the owner of geometry or
element-ID operations.

## Verification

Offline checks validate workflow/schema parsing, binding compatibility, reference-state transitions,
validation gating, explicit execution, and cancellation behavior without requiring Archicad. UI
technology evidence must also cover keyboard access, narrow and wide palette layouts, theme
readability, search/navigation, section behavior, and repair of missing references. A live Archicad
claim requires the user to build and review the chosen shell; it cannot be inferred from a browser
build alone.

## Canonical References

- UI technology decision and evidence gate: `private development decisions/ADR-005-notebook-ui-technology.md`.
- Notebook task registry: `private development tasks/notebook.yaml`.
- Existing experiment handoff: `docs/architecture/notebook/HANDOFF.md`.
- Command boundary: `docs/architecture/commands/SPEC.md`.
- API and typed Archicad boundary: `docs/architecture/api/SPEC.md`.
