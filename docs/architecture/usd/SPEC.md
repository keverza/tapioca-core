# OpenUSD Specification

Status: canonical specification for `internal work item`. Live task state is in `private development tasks/usd.yaml`;
the implementation handoff remains `docs/architecture/usd/HANDOFF.md` while its registered work is active.

## Scope

`internal work item` owns scoped downstream scene packaging, provenance, and sharing formats. OpenUSD is a
durable aggregation format when a workflow needs geometry, design-option information, analysis, and
metadata together. It is not an Archicad project backup or a complete BIM replica.

## Scene Boundary

An exported scene is project-scoped and usually design-option-scoped. It contains only the requested
proposal geometry, relevant context, analysis results, feasibility facts, metadata, and deliberate
artifacts such as screenshots. Source Archicad element GUIDs and source revision information remain
associated with the exported data.

The builder consumes task snapshots and shared analysis results. It never becomes a second BIM
authority, silently expands a request to the whole project, or writes back to Archicad as part of
serialization.

## USD And Narrow Bypasses

Use OpenUSD when a durable combined scene benefits preview, sharing, comparison, evaluation, or
agent handoff. Do not require it when the consumer needs only:

- mesh geometry;
- CSV or XLS facts;
- a small JSON payload; or
- direct analysis input.

Those workflows may use a narrower export path without changing the shared analysis or provenance
contract.

## Packaging Flow

The normal flow is:

```text
request-shaped API snapshot
        -> shared analysis result, when needed
        -> scoped OpenUSD scene
        -> external consumer
```

An option scene may contain proposal geometry, required context, sun/shadow/heatmap results,
feasibility facts, source metadata, and screenshots. Each option remains distinguishable and each
analysis remains associated with the option and source scope that produced it.

## Verification

Offline tests validate stable prim identity, units, mesh partitions, material conversion, option
composition, analysis association, and manifest/schema rules as those features are implemented.
Live checks confirm that an export contains only the requested scope, preserves source identity and
provenance, and that mesh/CSV/XLS bypasses remain usable without USD.

## Canonical References

- API snapshots and source authority: `docs/architecture/api/SPEC.md`.
- Shared analysis results: `docs/architecture/analysis/SPEC.md`.
- Active USD work: `private development tasks/usd.yaml`.
