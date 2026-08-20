# Agent Specification

Status: canonical specification for `internal work item`. Live task state is in
`private development tasks/agent.yaml`; the registry is intentionally empty until an independently actionable
agent integration task is proposed.

## Scope

`internal work item` owns downstream artifact handoff and later coding-agent or autonomous workflow
integration. It consumes scoped outputs from the other initiatives; it does not become a second BIM
authority, extraction path, analysis implementation, evaluator, or command runtime.

## Artifact Handoff

An agent may consume:

- scoped OpenUSD scenes;
- CSV/XLS or other tabular exports;
- mesh exports;
- shared analysis packages; and
- structured evaluation results.

Artifacts must retain enough scope and provenance for the agent to understand which design option,
source elements, and Archicad revision they describe. The agent reasons over those artifacts rather
than silently reconstructing a whole Archicad project.

## MCP Decision Gate

MCP is deliberately deferred. A complete Tapioca package followed by agent evaluation does not need
MCP. Introduce an MCP boundary only when the workflow requires the agent to:

1. investigate Archicad beyond the packaged evidence;
2. ask Archicad for additional information;
3. request another analysis study; or
4. trigger an Archicad action.

That change is a cross-project architectural decision and requires an ADR plus owned provider/API
tasks before implementation. Until then, agent integration remains downstream and read-oriented.

## Verification

Handoff tests validate artifact scope, provenance, stable schemas, and failure reporting for missing
evidence. They must demonstrate that an agent can consume the package without direct ACAPI access,
whole-project mirroring, or duplicated feature logic.

## Canonical References

- OpenUSD artifacts: `docs/architecture/usd/SPEC.md`.
- Shared analysis results: `docs/architecture/analysis/SPEC.md`.
- Evaluation results: `docs/architecture/evaluator/SPEC.md`.
