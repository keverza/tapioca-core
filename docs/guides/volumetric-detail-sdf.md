# Volumetric detail SDF contract

**Status:** SDF-0 contract, frozen 2026-11-03 for the minimal landscape slice (#140).
This document governs the first implementation. It does not claim that the later
stages are implemented.

## 1. Product scope and ownership

A `VolumetricDetail` authors non-single-valued landscape geometry: rock
outcrops, boulders, caves, overhangs, berms and sculptural landform massing. It
is not a general modelling application, a replacement for exact hardscape, or a
terrain Boolean engine.

The durable record is owned by `LandscapeSession.volumetricDetails`, alongside
sources, planar operations, hardscape objects and edges. It is not a
`HardscapeObject` footprint and not a standalone document. Its operation stack
is the authoring authority; evaluated meshes are disposable derived snapshots.

Three geometry authorities remain separate:

1. Terrain owns the ordered 2.5D `FieldPlan` and published TIN (#112).
2. Editable polygon meshes own persistent topology and stable authored part IDs
   (#139).
3. Volumetric details own an ordered 3D field definition and its derived
   triangle-mesh snapshots (#140).

They share publication, selection, materials, provenance, undo and the future
native Accept lane. They do not share an evaluation stack or become one another's
source of truth.

## 2. Units, transforms and identity

- Positions, dimensions, radii, blend radii, bounds and `voxelSize` are world
  metres. Positions remain `f64` through evaluation and meshing.
- Angles are radians in the data model. Conversion to degrees is UI-only.
- The frame is right-handed and Z-up, matching `geomsrv::Mesh`.
- SDF-1 primitives carry direct metric dimensions and rigid local frames. There
  is no hidden normalized/local-unit scale and no non-uniform coordinate warp.
  Resizing edits the primitive's metric dimensions.
- Durable detail and operation IDs come from the existing editor document
  allocator. Mesh indices, content hashes and component numbers are derived and
  are never durable identity.
- Authored `voxelSize` is positive and finite. Raw NX/NY/NZ resolution is never
  persisted or exposed as the authoring control.

## 3. Ordered stack, enable and solo

The first field vocabulary is sphere, rounded box, capsule and exact 2D-profile
extrusion. Operations are `Union`, `Subtract` and `Intersect`, evaluated in
stack order. The first blend profiles are Sharp and Round; zero blend radius is
a hard `min`/`max` Boolean for either profile.

For accumulated distance `a`, operand distance `b` and blend radius `k > 0`:

| Operation | Sharp | Round |
|---|---|---|
| Union | `min(a,b) - k*h^3/6`, `h=max(k-abs(a-b),0)/k` | `h=clamp(0.5+0.5*(b-a)/k,0,1)`; `mix(b,a,h)-k*h*(1-h)` |
| Subtract | `max(a,-b) + k*h^3/6`, `h=max(k-abs(a+b),0)/k` | `h=clamp(0.5+0.5*(a+b)/k,0,1)`; `mix(-b,a,h)+k*h*(1-h)` |
| Intersect | `max(a,b) + k*h^3/6`, `h=max(k-abs(a-b),0)/k` | `h=clamp(0.5+0.5*(b-a)/k,0,1)`; `mix(a,b,h)+k*h*(1-h)` |

At `k = 0`, these reduce respectively to `min(a,b)`, `max(a,-b)` and
`max(a,b)`. No arbitrary positive epsilon replaces an authored zero radius.

Disabled operations are omitted without changing the order of the remainder.
Solo is a transient diagnostic preview cutoff: evaluate the enabled stack from
its beginning through the selected operation, inclusive. At most one operation
can be soloed. Solo does not create history and is not a durable visibility or
selection state.

After enable/solo filtering, the first operation of the root and of every
nonempty group must be `Union`. A first `Subtract` or `Intersect` refuses with
`FirstOperationNotUnion`; it is never silently normalized and never evaluated
against an artificial empty distance. Empty/all-disabled groups are skipped. An
empty resulting root refuses with `EmptyStack`.

## 4. Group headers and isolation

A group is started by an explicit header; subsequent operations belong to that
header until the next header. SDF-1 has no nested groups. A header owns the
group's enabled state, isolation flag and the blend used when an isolated group
joins the scene.

- **Isolated group:** compile and evaluate a local field from the group's
  operations, then union that completed field into the parent exactly once, at
  the header's position. A subtract inside the group cannot cut an earlier group
  or anything outside it.
- **Non-isolated group:** the header is organizational; its operations join the
  parent stack at the same position. Their effects are intentionally not
  isolated. The positive-seed rule still applies within every declared group so
  a subtract-only group can never blank a scene.
- A group whose first enabled operation is not `Union` refuses even when other
  groups are valid. Failure discards the entire candidate result.
- The first isolated group seeds an otherwise empty parent. Later isolated
  groups use their header's Sharp/Round union blend. Group blend cannot turn a
  group into a scene-wide subtract or intersect.

## 5. Field capabilities and correctness

The pure Rust `geometry::sdf` module owns field compilation/evaluation, sampling
and meshing. It has no host, renderer, filesystem, UI or FFI dependency. Every
field node exposes the following capabilities; missing support weakens the
sampling schedule rather than permitting an optimistic guess:

| Capability | Contract |
|---|---|
| Distance | Negative inside, positive outside, zero at the requested iso surface. |
| Bounds | Conservative finite bounds, or `None` for an unbounded/unknown field. |
| Interval | A closed range containing every distance in a closed sampling region. |
| Lipschitz | A conservative finite upper bound, or positive infinity when unknown. |
| Gradient | Analytic gradient where available; otherwise the documented finite-difference fallback. |
| Batch | Exactly the same values as scalar evaluation in the same point order. |

A region-pruned expression must be bit-identical to the original expression at
every point in that region. Tests compare `f64::to_bits`, not a loose tolerance.
Analytic and seeded property tests must prove that intervals contain samples and
that finite Lipschitz bounds are conservative. A too-small bound is a
correctness defect because it can remove surface.

SDF-1 excludes cheap approximate ellipsoids, Chamfer, shell/hollow, noise,
deformers, layout repetition, lathe, curves, TPMS, tree references and GPU
evaluation. Each requires a later contract and fixtures.

## 6. Bounds, sampling and diagnostics

Finite prepared bounds are padded conservatively for blend reach and by at least
two cells on every side. A closed detail must not touch the meshing domain. An
unbounded field requires an explicit authoritative user box; otherwise it
refuses with `BoundsRequired`.

For each axis, `cellCount = ceil(extent / voxelSize)` and the node grid has
`cellCount + 1` points. The bounds remain exact, so actual axis spacing is
`extent / cellCount` and is reported in diagnostics. All count and byte-size
arithmetic is checked before allocation.

- Finite bounds default to sparse 8-sample-block scheduling with conservative
  interval/Lipschitz pruning.
- Dense sampling is the fallback for an unbounded field with an authoritative
  user box, and remains available as a parity oracle.
- Skipped sparse blocks retain only an unambiguous above/below-iso state. They
  are not presented as exact evaluated distances.
- Narrow-band/sparse and dense parity fixtures must produce the same canonical
  mesh, not merely similar pictures.

Every result reports total grid points, evaluated points, stored sparse points,
visited/skipped regions, sampling strategy, bounds/Lipschitz availability,
requested and actual cell sizes, vertices, triangles and elapsed worker time.
Cancellation is checked during subdivision, leaf evaluation and meshing.

## 7. Marching Cubes, materials and determinism

SDF-1 uses one deterministic CPU Marching Cubes implementation with a
shared-grid-edge vertex cache. It produces indexed f64 world-metre positions,
per-vertex normals, per-triangle signed material IDs and source remaps suitable
for `PublishedMesh`/`geomsrv::Mesh`. The mesher does not implement a second
repair or decimation pipeline; #139 owns Stats/Clean and any later LOD.

Material ownership is evaluated with the field, never inferred from vertex
colour:

- Union chooses the operand with the smaller unblended distance; an exact tie
  chooses the later stack operand.
- Subtract chooses the cutter when `-b >= a`, otherwise the accumulated field.
  Thus newly exposed cut surfaces can carry the cutter's material while the
  retained outer surface keeps its original material.
- Intersect chooses the operand with the larger unblended distance; an exact tie
  chooses the later stack operand.
- Round/Sharp blending changes distance, not this hard material-owner choice.
  Isolated-group union follows the same Union rule.
- Each emitted triangle receives the material owner returned by the same full
  field evaluator at its deterministic centroid. Materials are not interpolated
  into gradients. A material ID must be positive; zero remains unknown and is not
  a guessed catalog reference.

Normals come from the same field gradient, normalized only when finite and
nonzero. A failure to produce finite geometry, normals or material ownership
refuses the complete result.

Mesh order cannot depend on worker count, hash iteration or scheduling. SDF-1
uses lexicographic cell traversal, fixed 64-cell X slabs, stable edge keys and a
canonical triangle order. The existing published-mesh content hash covers
positions, normals, triangles, materials and source remaps in that canonical
order; the worker schedule is not hashed. Repeating a request with different
worker counts must produce identical arrays and content hash.

## 8. Budgets and refusal

The initial hard ceilings are:

| Resource | Ceiling |
|---|---:|
| Authored field operations, including group headers | 256 |
| Dense grid points | 64,000,000 |
| Stored sparse grid points | 8,000,000 |
| Emitted triangles per detail | 2,000,000 |

A job may use lower limits. Exceeding a limit returns a typed budget refusal
before allocation where possible, or discards the complete mesh when the triangle
limit is discovered during emission. SDF-1/SDF-2 do not silently coarsen,
decimate, partially publish or substitute another result. The measured
chunked/capacity ladder belongs to SDF-3.

No LOD is generated by the SDF track. Add one only through #139 M1 after a
measured scene budget requires it.

## 9. Async publication and terrain anchor (SDF-2 gate)

SDF-1 is offline pure geometry plus its C++ record/facade; it is not a
main-thread interactive mesher milestone. Interactive work starts only with the
SDF-2 worker contract:

- A worker consumes an immutable detail definition, revision, content hash,
  voxel size and terrain anchor identity.
- Coarse preview and final mesh use the same compiled field, operator order,
  material rule and bounds policy. Only voxel size changes.
- Only a complete current result can replace the published snapshot in
  `EditorMeshStore`. Cancellation, stale completion, invalid output and capacity
  refusal publish nothing and retain the last valid snapshot.
- Native-capture `MeshStore::current/shared` is never overwritten.
- Preview/worker completion creates no history. One completed authoring gesture
  or Apply is one existing-session undo entry.

A terrain-placed detail anchors to a specific published terrain revision and
content hash plus authored XY and Z offset. Terrain edits mark that anchor stale;
they never silently move the detail or retarget an in-flight job. The detail does
not enter `FieldPlan`, does not change the TIN and does not implicitly carve or
union terrain. Any future terrain effect is a separate explicit authoring
contract.

## 10. Native Accept boundary

Offline field evaluation and derived preview do not depend on a native element
decision. When the shared Accept lane is later gated, the only initial native
candidate for a closed volumetric detail is one native **Mesh** element created
from the complete published triangle snapshot. SDF does not automatically choose
Morph or Slab, and it does not flatten a field into construction-exact geometry.

Open, non-finite, over-budget, stale or otherwise ambiguous results refuse
Accept. Exact slabs, walls, kerbs, paving joints and thin edging remain in their
existing exact-geometry lanes. Accept is one shared native write and one native
undo scope; there is no SDF-specific bake, snapshot-to-native shortcut or second
undo stack.

## 11. Minimal acceptance gates

SDF-1 is complete only when analytic/capability tests, a rock/outcrop, a
box-minus-capsule cave and a basic extrusion pass offline with:

- deterministic repeated and worker-count-independent meshes;
- first-operation and subtract-only-group refusal;
- proven isolated-group subtraction and enable/solo behavior;
- finite output, valid indices, positive material IDs and consistent winding;
- watertight padded closed fixtures and no duplicate shared-edge vertices;
- sparse/dense parity and enforced operation/grid/triangle budgets;
- cancellation with no partial result.

SDF-2 additionally requires Rock and Cave presets, place/edit/undo/redo/reload,
preview/final field parity, terrain-anchor staleness, last-good async publication
and diagnostics. The terrain definition and published TIN remain unchanged.
