# API Specification

Status: canonical API v2 contract extracted from the former
`historical source docs/evp-command-system-plan.md` and archived `historical source docs/api-v2-migration.md`. Live task
state is in the registered API task file and history; this document defines the boundary and
wire-shape rules.

## Ownership and Layers

`internal work item` owns the controlled C++ Archicad SDK boundary, the Python API surface, selective
extraction, scheduling, dirty events, and navigation exposure. It does not own rendering,
analysis algorithms, USD serialization, or evaluation.

There are three callable backend families:

```text
tapioca.api.call("Tapioca.Name", params)  # native command, canonical public spelling
tapioca.api.call("API.Name", params)      # official Archicad JSON API wrapper
tapioca.api.call("Tapir.Name", params)    # routed Tapir add-on command
```

`EvP.*` remains a compatibility alias for unmigrated consumers. New diagnostics, docs, and
callers use canonical `Tapioca.*` names. The native implementation remains under `EvP` paths
for ABI and deployment compatibility.

At the Python layer, `tapioca` is the public shell over `evp`; existing commands that import
`evp` remain valid. This package relationship is separate from the wire-level namespace rule:
`Tapioca.*` is canonical, while `EvP.*` is accepted only at the dispatcher boundary during the
compatibility window.

The API is request-shaped: callers request a scope, representation, properties, and execution
intent. A snapshot or cache never becomes an alternate BIM authority. Archicad owns identity,
element type, geometry, properties, and project meaning.

## Selective Extraction

An extraction request is a projection over Archicad, not a universal Tapioca BIM object. It
identifies:

- the scope, such as a selection, explicit element IDs, an analysis scope, a design option, or
  required context;
- the representation, such as mesh, materials, or other task-specific geometry;
- the Archicad properties required by the consumer; and
- the execution intent: `interactive`, `background`, or `auto`.

No scope implies the whole project. Every downstream cache is task-scoped and may keep its own
representation, while preserving Archicad identity, source revision, and provenance. A consumer
must not request all BIM objects, metadata, properties, relationships, or BREP by default.

## API V2 Contract

API version `2.0.0` is a clean wire-contract break. Every retained `Tapioca.*` command has a
strict request and response schema owned by its `NativeCommandRegistration` row, or by the
dispatcher-local schema table for a bus or palette control verb. The generated catalog is the
count of record: read it from `dist/TAPIOCA-API-V2.json`'s `metadata.counts` rather than from a
number written here, because a figure transcribed into prose goes stale on the next registered
command and then contradicts the artifact it describes:

- `dist/TAPIOCA-API-V2.json` is the machine-readable catalog;
- `dist/TAPIOCA-API-V2.md` is its generated human-readable table;
- `AddOn/EvP/tools/generate_tapioca_api_v2.py` is the generator and count tripwire.

Input validation runs before a handler and successful response validation runs after it. The
transport owns `ok`, `data`, `error`, and `meta`; native success payloads contain domain fields
only. A partially successful batch returns typed records with `succeeded`, not a nested transport
status. No new caller may introduce `EvP.*`, and active commands migrate only when resumed rather
than keeping every inactive probe compatible indefinitely.

## Wire Shapes

Element records use nested, Tapir-style records:

- one response record per input GUID, positionally aligned;
- explicit `found: false` records for misses;
- a type-discriminated `details` object for kind-specific fields;
- coordinate records use `{x, y}` or `{x, y, z}`;
- an element-record request takes the typed identity envelope `elements: [{elementId:{guid}}]`,
  matching the response's own identity shape; the bare `guids` input survives only where a
  command's registered schema still declares it, and the schema — not this list — is what a
  caller reads. `Tapioca.GetElementDetails` requires `elements`, and its root schema is a strict
  object, so a bare `guids` body fails validation before the handler runs;
- kind-specific fields stay inside `details` and are never promoted into a universal sparse row.

Bulk numeric operations retain flat parallel arrays because rays, snapshots, and geometry buffers
are consumed at high volume and by zero-copy NumPy views. Every packed array documents its count,
stride, and alignment convention in the native contract.

Native command registrations are the single source of truth for command names, makers,
cancellation traits, request schemas, and response schemas. Root schemas are strict objects;
success data contains domain fields only; failures use the command result failure path and the
dispatcher owns the transport envelope. A response validator is never weakened to hide a
handler/schema mismatch.

Element identity is represented as `{elementId:{guid}}` where the shared schema permits it. Raw
integer fields remain integer-typed. GUID comparisons normalize braces and case at the boundary.

## Native Command Contract

Each native domain has one `Make<Domain>Command` factory in
`AddOn/EvP/Sources/AddOn/NativeCommands/<Domain>Commands.cpp`, registered by the domain maker
array. The command classes remain in the implementation file unless a shared interface is
required.

- `MainThreadCommand` is the default when any ACAPI call is made.
- `WriteCommand` is for undoable element/project writes; the command opens no undo scope.
- `StructuralCommand` is for database or other non-undoable structural operations and is
  rejected inside transaction replay before ACAPI is reached.
- `NeedsMainThread() == false` is allowed only when the whole implementation is ACAPI-free and
  reads immutable or mutex-guarded data.
- Every ACAPI symbol is verified against the vendored AC29 DevKit before implementation.
- Native errors name the attempted operation through `EVP_ACAPI_FAIL`; non-ACAPI refusals use
  `EVP_FAIL`. A bare `GSErrCode` is not diagnostic evidence.
- Selection and highlight are UI state, not undoable writes.

The dispatcher supplies the main-loop gate, transaction replay, cancellation refusal, envelope,
and tracing. It does not duplicate domain operations implemented by a native command.

## Execution Boundary And Scheduler

Archicad-dependent work has a controlled phase: read the requested data through the SDK, copy the
task snapshot, and cross the boundary. Mesh transformation, GPU preparation, rendering, analysis,
OpenUSD serialization, tabular export, AI requests, and evaluation run outside that phase whenever
technically possible. A worker thread is never made safe merely by moving an ACAPI call there.

The Tapioca scheduler coordinates Archicad-safe extraction, priority, batching, dirty updates,
cancellation, superseding requests, and consumer demand. It does not become the implementation
home for Diligent, analysis, USD, commands, or evaluation. `auto` thresholds remain measurement-
driven; no production threshold is selected from an assumed element count or timer value.

Dirty synchronization is GUID-shaped. A consumer marks only changed objects dirty, requests the
profile needed for those objects, and patches its cache rather than rebuilding an entire scene.
Each consumer keeps an independent change cursor so one consumer cannot consume another consumer's
updates.

Development telemetry should identify the request, consumer, profile, element and triangle counts,
requested properties, SDK extraction time, processing time, transfer time, and completion points.
The measurements establish scheduler policy; they are not a second project-state authority.

## Delivered API Families

The current surface is organized by question rather than by every element type:

| Family | Surface | Contract boundary |
|---|---|---|
| Selection | `GetSelection`, `ModifySelection`, highlight, zoom, prompt | Main-thread UI state; no undo scope. |
| Element header/detail | `GetElementInfo`, `GetElementDetails`, `SetElementDetails` | Header coverage is broad; parametric geometry is nested and kind-specific; writes are sparse and reject read-only fields by name. |
| Geometry data plane | rays, nearest/query, slice, clash | Reads immutable snapshots off the gate where proven; flat arrays and batch counts are intentional. |
| Properties and topology | official `API.*` wrappers plus native collisions/connections | Resolve names and IDs before writes; batch writes; do not create project properties at runtime. |
| Drafting and identity | text, picture, columns, element IDs, Drawing clips | Use native commands for capabilities absent from the official JSON surface; read back unsupported or undocumented writes. |
| Layout and databases | navigator views, databases, 3D documents, Drawing placement | Structural commands are direct calls, not transaction members; view identity comes from the navigator tree, not a user-entered GUID or ambiguous name. |
| Structured Modeler data | bodies, polygons, materials, textures, lights, NURBS, components, connections | Preserve Modeler/C-API index bases and contour-break semantics; scratch sights restore the user's sight in a guard. |
| Change tracking | watch, token, dirty set, model diff, manual sync | A set coalesces edits; consumers need independent cursors; project changes and successful Tapioca writes both advance refresh state. |
| Runtime | embedded CPython, external runner, `requires`, cancellation, paths | Embedded and external zones expose the same command behavior; package preflight remains a separate platform task. |

## Python Facade

The Layer-2 package is the preferred authoring surface:

- `evp.api`: call, result, error, cancellation, tracing, and native error trails;
- `evp.transaction`: deferred writes, `Handle`, and `Ref` for one atomic user action;
- `evp.elements`: IDs, headers, nested details, sparse writes, and level offsets;
- `evp.geometry`: snapshots, zero-copy meshes, rays, queries, and slice helpers;
- `evp.properties` and `evp.topology`: project property and relationship wrappers;
- `evp.selection`, `evp.issues`, and `evp.drawings`: reusable domain operations;
- `evp.model`: structured Modeler records, loops, materials, textures, and lights;
- `evp.changes`: watch, token, polling, settle, and batched updates;
- `evp.paths`, `evp.runtime`, `evp.ui`, and `evp.command`: shared platform contracts.

A wrapper does not hide a provider gap. If a command needs a missing SDK capability, the gap is
requested in `internal work item` and the consumer task records the provider ID in `blocked_by`.

## API Gotchas

- `ObjectState` reserves `type` as its discriminator. Never use it as a named parameter.
- Official `API.*` names and shapes come from the local `archicad` package, not Tapir docs.
- Native property writes use the typed `NormalOrUserUndefinedPropertyValue` union. Numeric
  values must remain numeric; clearing uses the user-undefined status without a value.
- A command result can contain per-item failures while the transport call itself succeeds;
  read `results[]` and verify writes by reading back.
- `GetZoneBoundaries` is intentionally not absorbed until a consumer needs it; a heavy per-zone
  main-thread loop previously froze Archicad.
- View and database names are display data and are not unique. Build pickers from navigator
  records and carry GUIDs internally.
- `drawingGuid` links a project view when a Drawing is created and cannot be repointed later.
  Project-view Drawings are layout-only; generated content for a worksheet follows the IDF or
  native copy path instead.
- A structural effect is not on the undo stack. Destructive structural deletes require explicit
  confirmation.

## Verification

Offline validation covers schemas, pure wrappers, command dry runs, and ACAPI-free geometry.
Live validation is required for ACAPI conventions, visual results, runtime installation,
observer lifecycles, and any write whose native documentation is incomplete. Probes answer one
question per transaction and record decisive results in the owning task or a curated fixture.

The API reference generator and native contract checker must consume the same registered schema
source as the dispatcher. Hand-maintained sidecar schemas are not allowed. The current catalog
must fail generation if the registry count, dispatcher-local count, schema table, verb table, or
total command count drifts.

The release/devkit generator is the consumer of this contract: it emits native command schemas
from the registered implementation, bundles Tapir's authoritative JSON-with-prefix schema
files, and regenerates the human-readable API reference from those inputs. It must fail when a
registered command has no schema rather than silently producing an incomplete reference.

## Canonical References

- Native contract template: `private development templates/NATIVE-COMMAND-CONTRACT-TEMPLATE.md`.
- Native registry: `AddOn/EvP/Sources/AddOn/NativeCommands/CommandRegistry.cpp`.
- Python facade: `AddOn/EvP/Sources/PyPackage/evp/`.
- API task registry: `private task registry`.
- Former source and archaeology: `historical source docs/evp-command-system-plan.md`.
- V2 migration source and archaeology: `historical source docs/api-v2-migration.md`.
