# Evaluator Specification

Status: canonical specification for `internal work item`. Live task state is in
`private development tasks/evaluator.yaml`; the registry is intentionally empty until an independently
actionable evaluator task is proposed.

## Scope And Boundary

`internal work item` owns the provider-neutral local evaluation service, its stable evaluation API,
provider configuration, rubric and scoring rules, validation, ranking, and evaluator tests. It does
not modify the Archicad model. Archicad sees only the stable request and response contract.

The intended V1 boundary is a localhost `POST /evaluate` service. The initial implementation may
use Node.js, Fastify, the Vercel AI SDK, and Zod, but provider-specific details stay behind the local
service boundary.

## Evidence Inputs

An evaluation request contains only the evidence required for the comparison, such as a project
brief, option IDs, numeric feasibility metrics, geometry or model facts, shared sun/shadow results,
screenshots, or a scoped OpenUSD scene. OpenUSD is optional for a small feasibility payload.

Archicad and Analysis provide objective evidence. The evaluator does not extract a whole project,
invent missing evidence, or replace a source result with model speculation.

## Evaluation Rules

1. Derive one rubric from the project brief before grading options.
2. Apply exactly the same rubric to every option.
3. Treat numeric evidence as authoritative.
4. Score deterministic criteria in code where possible.
5. Use model reasoning only for qualitative criteria.
6. Report missing evidence explicitly rather than inventing it.
7. Calculate weighted totals in code.
8. Validate structured output before returning it to Archicad.

The response associates each option with its rank, total score, and criterion-level scores. The
schema must make the association and validation failures explicit.

## Provider And Secret Isolation

Provider configuration and API keys remain in the local evaluator service. They never enter the
Archicad add-on, command folders, WebUI, task artifacts, or committed repository configuration.
Changing providers must not change the Archicad-facing evaluation contract.

## MCP Boundary

V1 does not require MCP. It becomes justified only when an agent must actively interrogate Archicad,
request additional evidence, run another study, or trigger an action. A packaged evaluation of
already-produced evidence remains an ordinary evaluator request.

## Verification

Tests validate request and response schemas, common-rubric application, deterministic totals,
ranking, missing-evidence reporting, and provider isolation. Integration checks confirm that the
service can evaluate scoped artifacts without modifying Archicad.

## Canonical References

- Shared result semantics: `docs/architecture/analysis/SPEC.md`.
- Scoped scene packaging: `docs/architecture/usd/SPEC.md`.
- Artifact handoff and agent action threshold: `docs/architecture/agent/SPEC.md`.
