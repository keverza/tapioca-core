# Exposed surface measurement contract

Contract version: **1**. C++ algorithm version: **3**.

This is the surface-specific contract for issue `tapioca-dev#193`. It freezes the
measurement semantics and regression oracle before the native controller, shared
page, region tools and Rust production adapter are added. It does not claim that
the later delivery slices are implemented.

## 1. Measures and units

- Coordinates are finite `double` world metres in a right-handed, Z-up frame.
  Capture and calculation retain world coordinates. Plane-local calculation uses
  each source face's origin, not a translation of the published mesh.
- Areas are square metres (`m2` in legacy wire payloads, `m²` in presentation).
- **Gross geometric** is the sum of validated source-face geometry with opening
  holes preserved. Coincident same-material source faces remain separate gross
  source requests; exposed ownership is deterministic and counts their overlap
  once. This distinction is pinned by
  `ExposedSurface.DuplicateRequestsAndCoincidentSameMaterialFacesOwnAreaOnce`.
- **Exposed** subtracts the union of contact/intersection coverage supplied by
  the explicit cover scope. Camera visibility, foreground occlusion and material
  filters never define exposure.
- **Selected exposed** is the exposed subset selected by captured surface
  attributes. Filtering targets never filters the cover scope.
- Net is unavailable. A missing net value must never be serialized as zero or
  relabelled as gross scheduled area.

Every published total is recomputed from the final emitted world-space
measurement triangles. Table, highlights and exports must consume the same
immutable result and selected measure. No display offset or projected area is a
measurement input.

## 2. Query schema

The controller/wire schema is versioned separately from the C++ `Request` so the
legacy native commands remain compatible. A version-1 query contains:

| Field | Required semantics |
| --- | --- |
| `contractVersion` | Integer `1`; unsupported versions refuse. |
| `documentId` | Stable open-document identity, assigned by the native controller. |
| `snapshotId`, `captureStamp` | Exact captured geometry identity; both must match. |
| `targetScope` | Explicit mode plus element GUIDs and source face keys. No empty-selection fallback to all targets. |
| `supportedElementSet` | Captured walls, slabs, columns and beams only. Whole-model requests disclose omitted types and capture completeness. |
| `coverScopeId`, `covers` | Explicit model-state/capture identity and body keys, independent of target/material/region filters. Empty means no covers, never implicit whole model. |
| `measure` | `exposed` (default) or `grossGeometric`. |
| `surfaceAttributeIndices` | Stable captured Archicad surface attribute identities, not names or transient pool indices. |
| `contactToleranceM` | Finite normal contact gap in `[0, 0.001]`; default at the command boundary is `0.001`, low-level C++ default is `0`. |
| `region` | Optional oriented rigid frame and positive half-extents; reserved for S4 and refused before that slice. |
| `boundaryPolicy` | `exactBoxClip` (default when a region exists) or `expandToMaterialPatch`; reserved for S4. |
| `grouping` | Material by default; element and Home Story are additional groupings, never geometric floor splitting. |
| `preset` | Optional preset identifier and version plus its fully expanded ordinary query; reserved for S5. |

The existing `geomsrv::exposed::Request` remains the source-face/cover kernel
input. `coverScopeId`, snapshot/stamp, face and body keys, pool filter and contact
tolerance map directly. Controller-only fields must not be silently discarded by
an adapter.

### Source identity

`BodyKey` is capture-local mesh index plus ModelerAPI 1-based source body.
`FaceKey` adds the ModelerAPI 1-based source polygon. Surface attribute identity,
building-material identity and capture-local material pool index are distinct.
Names are labels only: duplicate names neither merge rows nor establish a match.
Unknown/generated surfaces use explicit identity `0`; they are never guessed.

## 3. Result and status schema

A successful immutable result contains:

- contract and kernel/algorithm versions, result ID/revision and canonical query
  hash (controller-owned), plus document/snapshot/capture identity;
- the canonical target and cover scopes actually used and all applied criteria,
  contact tolerance, region policy and preset metadata;
- source-attributed fragments (`FaceKey`, surface/building-material identity,
  loops, points, triangle indices and area), per-face/material/element totals,
  inclusion/exclusion diagnostics and resolved region-patch membership;
- completeness/freshness, capture restrictions, units and tolerance metadata;
- bounded-work statistics (`candidateCoverPairs`, `sectionCount`, `work` and
  `outputTriangles`).

The controller states are `empty`, `pending`, `current`, `stale`, `refused` and
`cancelled`. Only `current` is exportable. `stale` may retain a clearly labelled
last-good result but cannot present it as aligned with a newer capture.
`refused`, `cancelled`, superseded and stale-capture worker completions publish
no partial quantity.

The S1 `StartExposedSurfaceTakeoff`, `PollExposedSurfaceTakeoff` and
`CancelExposedSurfaceTakeoff` native commands expose this mailbox without an
overlay dependency. Submissions return the generation assigned under the
controller lock. Workers consume typed SDK-free request/result DTOs; GS conversion
is confined to the legacy command and poll boundaries. Queued superseded work is
discarded, cancellation is cooperative, and document close revokes pending work.
`lastGoodResult` is present only when `hasLastGood` is true and is exportable only
when `state == "current"`; `pending` and `stale` results are explicitly not current.
`FreeData` joins the worker on APX unload, including unload without a Quit event.

The legacy C++ API returns `bool` and a human-readable error and atomically clears
`Result` on every failure. The additive `Refusal` overload supplies the following
machine-stable codes and source element/body/polygon IDs; the S1 native
controller maps them to transport failures without removing the legacy API:

| Code | Meaning |
| --- | --- |
| `staleCapture` | Snapshot/stamp/document identity does not match the active capture. |
| `invalidQuery` | Missing scope, invalid IDs/filter/tolerance, unsupported measure or malformed region. |
| `unsupportedTarget` | Non-planar/unrepresentable source or unsupported target type. |
| `invalidCover` | Missing, open, non-manifold, degenerate or inward-wound required cover. |
| `ambiguousOwnership` | Coincident surfaces have conflicting material ownership or ambiguous source identity. |
| `workLimit` | Input, face/body, work, output or time budget exceeded. |
| `cancelled` | Cancellation/supersession; never a partial result. |
| `numericalFailure` | Boolean/triangulation/area reconciliation failure within otherwise supported input. |
| `internalFailure` | Contained exception or invariant failure. |

## 4. Supported target/cover matrix

| Geometry | Target source | Required cover | Outcome |
| --- | --- | --- | --- |
| Planar wall/slab/column/beam polygons, including sloped and rotated planes | Supported | Closed triangulated solid | Calculate in full 3D. |
| Openings and reveal/jamb/head/sill polygons | Supported when capture preserves their source identities | Door/window/frame bodies must be explicitly captured and included | Holes remain holes; only actual overlap/contact is deducted. |
| SEO-generated planar faces | Supported only after capture preserves final operated geometry and provenance | Final captured solids | No second SEO operation or nominal pre-SEO quantity is inferred. |
| Mirrored geometry | Supported with finite coordinates and consistent source/cover winding | Mirrored inward-wound solids refuse | Rigid motion/reflection alone does not change true area. |
| Non-planar, curved or NURBS source polygon | Unsupported | N/A | Typed refusal; never tessellation guessed as a complete planar source. |
| Below-grid/unrepresentable source | Unsupported | N/A | Refusal, not a zero-area result. |
| Duplicate same-material coincident faces | Deterministic exposed ownership | Valid covers | Gross keeps source requests; exposed overlap counted once. |
| Coincident different-material faces | Ambiguous | N/A | Atomic refusal. |
| Open, non-manifold, degenerate or inward-wound cover | N/A | Invalid | Atomic refusal naming the required cover; never omit it. |
| Self-intersecting cover | N/A | Invalid | Bounded local-coordinate crossing/overlap detection refuses atomically; shared edges, T-junctions, point contacts and disconnected nonintersecting components remain valid. |
| Uncaptured cover or unsupported captured element types | Incomplete scope | Missing | Refuse completeness; disclose capture restrictions. |

## 5. Numeric policy and error budgets

The three tolerances have separate jobs and must not be conflated:

1. **Source/plane validation:** `surfacecoverage::kPlaneTolerance = 1e-7 m`.
   It is used for planarity, snapped solid-edge validation and broad-phase plane
   contact. It is not a user contact gap.
2. **Contact gap:** explicit request value in `[0, 0.001] m`, measured only along
   the target normal. It never offsets coverage sideways or welds meshes.
3. **Boolean lattice:** ClipperD precision `7`, whose binary cell is `2^-24 m`.
   Triangulation stays on the resulting binary grid; output is not re-snapped to
   a decimal grid. Large coordinates are projected relative to the source frame
   origin, with absolute world coordinates limited to `1e9 m` by validation.

Regression comparisons use these budgets:

- Ordinary analytic area fixtures: absolute error `<= 1e-6 m²` unless a tighter
  exact-grid expectation is stated in the test.
- Fragment/world-triangle reconciliation: `1e-12 + 1e-6 * planarArea` (the
  existing atomic kernel invariant); tests may require tighter exact-grid equality.
- Non-grid-aligned independent dimensions: half-cell boundary bound
  `0.5 * 2^-24 * perimeter` plus floating-point roundoff. For example, the
  5-by-1.9 m source uses `5 * 2^-25 m²`, not a tolerance copied from observed output.
- Refused/cancelled/budget-exceeded calculations: exactly zero published faces,
  fragments and totals; a caller's previous output object is cleared.

No analytic expectation may be replaced with a value observed from the kernel.
C++ output is the temporary differential oracle for the future Rust lane, not an
independent proof of geometric correctness.

## 6. Independent fixture and differential ledger

| Concern | Independent expectation | C++ differential/regression location |
| --- | --- | --- |
| Opening holes and all reveals | `13.2 + 1.08 = 14.28 m²` from fixture dimensions | `AnalyticWallOpeningAndEveryRevealKeepExactAreaAndHole` |
| Contact gaps | Exact covered/uncovered rectangles at 0, 0.5, 1 and 1.001 mm | `NonSeoOpposingFacesWithinOneMillimetre...`, `NonSeoDoorCasing...` |
| Overlapping covers | `15 - 9 = 6 m²`, union rather than double subtraction | `IntersectingAndOverlappingCoversAreUnionedNotSubtractedTwice` |
| SEO/final operated capture | Live final-geometry metrics remain separate from analytic planar kernel fixtures | `CapturedPlanarSourceFaceMatchesNativeContourArea` plus the surface live suite |
| Sloped/rotated geometry | Rigid transforms preserve `15/9 m²` | `RotatedInclinedFacesKeepTrueThreeDimensionalArea` |
| Mirrored geometry | Reflection with corrected winding preserves analytic area; inward cover refuses | `MirroredGeometryPreservesAreaWithoutAcceptingInsideOutCovers` |
| Duplicate faces | Gross `30`, exposed `15 m²`; requests and covers deduplicate | `DuplicateRequestsAndCoincidentSameMaterialFacesOwnAreaOnce` |
| Duplicate material names | Separate stable attribute IDs survive selection and emitted provenance | `SurfaceAttributeMap.DuplicateDisplayNamesRemainDistinctStableIdentities` |
| Thin slivers | Exact binary-grid area and captured #168 regressions | `CapturedOneGridTriangularContactSliver...`, `AFragmentClipperNeverFinished...` |
| Invalid solids | Atomic refusal with no partial rows | `InwardWoundCover...`, `NonPlanarTargetAndOpenCover...`, `ClosedCoverWithCollinear...` |
| Self-intersecting shells | Overlapping and edge-aligned closed shells refuse with cover identity; separated components, saddle point contacts and near-tie adjacent planes remain valid | `SelfIntersectingCoverShellsRefuseAtomicallyWithSourceIdentity`, `EdgeAlignedCrossingOfSubdividedShellsIsRejected`, `DisconnectedNonintersectingShellsInOneBodyRemainValid`, `SaddlePointContactsAndNearTieAdjacentPlanesRemainValid` |
| Stale captures | Mismatched nonzero snapshot/stamp refuses and clears output | `StaleCaptureRevisionCannotPublishOrRetainPartialOutput` |
| Large survey coordinates | Translation by `(6,000,000, -3,000,000, 1,500)` preserves `9 m²` | `SurveyCoordinatesDoNotInflateOrProjectArea` |

## 7. Work limits and measurement lane

Default low-level limits are 100,000 input triangles across requested meshes,
512 faces, 128 cover bodies, 2,000,000 charged work units, 200,000 output
triangles and 2 seconds. The current calculator raises only cover bodies to 2,048
for library openings. Input/work/time and solid validation remain enforced.
Cover self-intersection validation uses an X-swept AABB broad phase followed by
proper-crossing or positive-area coplanar-overlap checks in body-local coordinates.
Every broad-phase pair scan is charged to the same work/cooperative time budget.

Runtime and peak-memory measurements are environment-specific observations,
never expected areas or cross-machine unit-test constants. Automatic refresh
remains disabled until reproducible baseline results and per-operation budgets
are reviewed.

The opt-in S0 C++ benchmark is `TapiocaExposedSurfaceBench`, enabled with
`-DTAPIOCA_BUILD_EXPOSED_SURFACE_BENCHMARK=ON` (default `OFF`). The target itself
has no Rust dependency; disable the default Rust-linked test project as well for
a Rust-free configure. From `core/`:

```powershell
cmake -S AddOn/EvP/tests/cpp -B build-exposed-surface `
  -DTAPIOCA_LINK_RUST_GEOMETRY=OFF `
  -DTAPIOCA_BUILD_EXPOSED_SURFACE_BENCHMARK=ON
cmake --build build-exposed-surface --config Release --target TapiocaExposedSurfaceBench
build-exposed-surface/Release/TapiocaExposedSurfaceBench.exe --iterations 25
```

The iteration count is bounded to 1–1000. The benchmark reuses the production
exposed-surface kernel and reports CSV for uncovered, partial-contact,
overlapping-cover, opening/reveal and deterministic 32-face fixtures.
Independent analytic gross and exposed areas are checked to `1e-6 m²` before any
timed sample; work/output counts, nearest-rank p50/p95, maximum elapsed
milliseconds and process peak RSS accompany each row. Peak RSS is cumulative for
the process, including preflight and earlier fixtures, not isolated per-fixture
allocation. Timings and memory values are observations for the measured
environment, not correctness thresholds or a substitute for live-model review.

### 2026-11-14 reference baseline

Measured with the final C++ S0 kernel on Windows 11 Pro 10.0.26200 x64, an
i9-12900K exposed as 3 cores/6 logical processors, 8 GiB visible RAM, MSVC
Release, and two independent 25-iteration processes. Values are ranges across
those runs, not expected areas or portable timing assertions.

| Fixture | Input triangles | Charged work | Output triangles | p50 ms | p95 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Uncovered | 12 | 39 | 2 | 0.0094–0.0095 | 0.0107–0.0110 |
| Partial contact | 24 | 180 | 3 | 0.0300–0.0302 | 0.0366–0.0394 |
| Overlapping covers | 36 | 319 | 3 | 0.0483–0.0490 | 0.0546–0.0573 |
| Opening/reveals | 16 | 192 | 20 | 0.0670–0.0698 | 0.0855–0.0907 |
| Multi-face-32 | 768 | 7,248 | 96 | 1.0070–1.0790 | 1.0675–1.1224 |

Final process peak RSS was 5.24–5.26 MiB. The initial **S8a admission budget**
for the synthetic multi-face operation is p95 `<= 10 ms` in Release on this
reference host, with exact analytic reconciliation and unchanged work/output
counts. This is a review budget, not a flaky unit-test deadline. The hard
per-operation limits remain 100,000 input triangles, 512 faces, 128 cover bodies
(2,048 only in the calculator adapter), 2,000,000 work units, 200,000 output
triangles and 2 seconds. S0 does not enable automatic refresh; representative
live-model extraction, calculation and memory evidence is still required.
