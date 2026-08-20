# Analysis Specification

Status: canonical specification for `internal work item`. Live task state is in
`private development tasks/analysis.yaml`; the registry is intentionally empty until an independently
actionable analysis task is proposed.

## Scope

`internal work item` owns analysis algorithms and shared result contracts. It owns derived results, not
the original Archicad geometry, element identity, or project authority. Inputs arrive through the
request-shaped API boundary and are sufficient for the current analysis only.

Initial result families include sun exposure, shadow studies, heatmaps, visibility, and feasibility
metrics. This list is a contract boundary, not an automatic task list.

## One Result Contract

Each logical analysis has one consumer-independent result contract. The same result may be rendered
as a Diligent overlay, stored in an OpenUSD scene, written as CSV/XLS, or supplied as structured
evidence to the evaluator. Consumers must not independently reinterpret the same analysis.

An analysis result carries enough scope and provenance to associate it with the source elements,
design option, and Archicad revision from which it was derived. Missing or out-of-scope input is
represented explicitly; a result never invents geometry, properties, or evidence.

## Boundary And Execution

The API extracts only the geometry, orientation, location, properties, and other facts required by
the analysis. Archicad-dependent reads stay in the controlled extraction phase. Pure analysis work
runs from the task snapshot outside the Archicad main loop whenever possible.

Analysis does not:

- maintain a whole-project BIM mirror;
- implement a second Archicad extraction path;
- own Diligent rendering, USD serialization, evaluator provider configuration, or WebUI policy; or
- change the Archicad model as a side effect of computing a result.

## Consumer Handoffs

- Diligent consumes the shared result for overlays and visual inspection.
- OpenUSD packages the result only when a scoped downstream scene is useful.
- Tabular exports represent the same result without requiring USD.
- The evaluator receives the result as objective evidence and applies its stable rubric rules.

All handoffs preserve the result contract, scope, and provenance. A consumer-specific projection is
allowed; a consumer-specific meaning is not.

## Verification

An analysis implementation is correct when the same input and versioned source context produce the
same contract across viewer, export, and evaluation paths, and when a result can be traced back to
its scoped Archicad inputs. Offline tests cover pure algorithms and contract validation. Live tests
are required for Archicad extraction conventions and visual presentation.

## Canonical References

- API extraction boundary: `docs/architecture/api/SPEC.md`.
- Diligent presentation: `docs/architecture/diligent/SPEC.md`.
- OpenUSD packaging: `docs/architecture/usd/SPEC.md`.
- Evaluator evidence boundary: `docs/architecture/evaluator/SPEC.md`.
