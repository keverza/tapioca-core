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

The legacy C++ API returns `bool` and a human-readable error and atomically clears
`Result` on every failure. The version-1 native boundary must add machine-stable
refusal codes and structured source IDs without removing that API:

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
| Stale captures | Mismatched nonzero snapshot/stamp refuses and clears output | `StaleCaptureRevisionCannotPublishOrRetainPartialOutput` |
| Large survey coordinates | Translation by `(6,000,000, -3,000,000, 1,500)` preserves `9 m²` | `SurveyCoordinatesDoNotInflateOrProjectArea` |

## 7. Work limits and measurement lane

Default low-level limits are 100,000 input triangles across requested meshes,
512 faces, 128 cover bodies, 2,000,000 charged work units, 200,000 output
triangles and 2 seconds. The current calculator raises only cover bodies to 2,048
for library openings. Input/work/time and solid validation remain enforced.

Runtime and peak-memory measurements are environment-specific observations,
never expected areas or cross-machine unit-test constants. Automatic refresh
remains disabled until reproducible baseline results and per-operation budgets
are reviewed.
