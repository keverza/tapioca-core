# Massing migration corpus v1

`massing-parity-v1.json` contains immutable, explicit inputs and independent
analytic anchors. `massing-parity-v1.expected.json` contains captured outputs of
the existing compiled C++ engines at production-source baseline `63d3cd1` for the
original fourteen cases. Ten added cases name baseline `a3ee1ff` explicitly in
their `baselineCommit` field; two workflow-shell cases name `f4ae4a0`; five
signed-arc cases name `1a8b01b`. None
overwrites the earlier snapshots.
The test-only adapter is `../massing_corpus.cpp`; no production algorithm changed.

## Replay

From `core/`:

```powershell
powershell -NoProfile -File AddOn/EvP/tests/cpp/Invoke-CppTests.ps1 -Filter '*MassingCorpus*'
```

Both files are required committed fixtures: missing, corrupt or unknown-version
data fails; it never skips. Normal replay cannot rewrite either file. A mismatch
prints `CORPUS_ACTUAL <name> <JSON>` as a diagnostic, not an automatic blessing.
Review any proposed baseline change against the analytic anchor, source test,
engine change and geometric intent before committing a curated diff. Do not copy
new measurements over the independent expectations just to obtain a pass.

The diagnostic preserves `3.0` as a double and `3` as an integer. Keep that
distinction in saved expectations. The comparison pins object keys, array sizes
and order, string/boolean values and integral IDs/colours/storeys exactly. Finite
floating results use an absolute `1e-6` tolerance (in the recorded m/m²/m³ units),
never a relative tolerance scaled by survey coordinates. No chain sorting,
rotation, reversal, quantization or hole flattening hides a difference. Repeated
evaluation must match with zero tolerance; input JSON must remain unchanged.

## Coverage and interpretation

Thirty-one cases exercise the existing `MassingSlices`, `MassingFacade`,
`MassingBake`, `SlabSlices`, `MassingBuildings` and `MassingHeadroom` consumers:

- rectangular/courtyard prisms, a thin excluded floor and a half-width envelope;
- grouped, case-sensitive building IDs with per-source floor-domain indices;
- an operated tapered body, an open-body refusal and sloped headroom;
- storey assignment, cut offsets and hole contours, plus missing-storey refusal;
- the recorded micrometre bake-corner defect and unsupported arc cleanup;
- shared-wall facade exclusion and georeferenced/elevated geometry.
- a closed courtyard shell plus a disconnected island, preserving three floor
  contours and two baked pieces per floor rather than inventing an outer box;
- a body-removed top floor: three floor records, two rendered slices, with exact
  empty-floor diagnostic and physical cut elevations retained;
- headroom holes and disconnected regions, a two-eave roof ridge, and a nearer
  low exit ceiling that must not be hidden by a higher disconnected solid;
- exact inclusive `1.6 m` and just-below-threshold floor accounting, including
  bake success versus refusal without changing physical volume or story identity;
- unsorted explicit levels with micron-near duplicates/out-of-range cuts, and
  storey numbering that continues above the project's top level;
- both original `massing-shells.json` workflow payloads, decoded through the
  existing `MassingCalculation::Decode` boundary before ten-floor slicing,
  clearance classification, building previews and bake planning. The original
  shell file is required and unchanged; `envelopeFixture` selects index 0/1,
  never regenerates an envelope or substitutes a box;
- positive and negative two-node/two-half-turn circular slab outlines, a curved
  annulus with a clockwise hole and a major circular segment with a closing chord.

The added anchors are derived independently before snapshot capture. The
courtyard/island fixture has `3*(100-4+16)=336 m²` counted area,
`9*(100-4+16)=1008 m³` ideal floor-prism volume and `9*(40+8+16)=576 m²`
vertical facade. Its measured volume is `1007.999998 m³` because the existing
base-plane tangency nudge affects the first cut, within the **unchanged** generic
`2e-5` analytic bound. The full captured value still uses `1e-6` replay tolerance.
Five closed box shells (including the reversed cavity and stacked solids) are
independently checked for opposing paired edges and signed scalar-triple-product
volumes, without calling the consumers under test or a primitive builder.
The removed-floor fixture deliberately contains only vertical walls, matching
the source regression: useful slice ingress, not valid closed-solid modelling
input. All added meshes remain literal committed coordinates and indices.

The workflow cases reuse the exact committed shell payloads and their requests,
not native SEO captures. Both source slabs are 32 x 32 x 30 m prisms with ten
3 m floors: raw area **10,240 m²**, raw volume **30,720 m³**. For the convex
20 x 20 m parcel, 3 m setbacks give a 14 m square. Above 8.5 m eaves, its
2:1 rise/run roof reaches the 22.5 m apex. At floor base `z`, 1.6 m clearance
leaves square side `max(0, 14 - max(0, z + 1.6 - 8.5))`. Allowed areas are
`196,196,196,141.61,79.21,34.81,8.41,0,0,0` (ideal total **852.04 m²**).
Each section retains the existing `2e-5 m²` analytic bound. The four nonintegral
sections accumulate that error: the total uses their summed `8e-5 m²` bound,
not a relaxed replay tolerance. The measured C++ total is
`852.03997741699277 m²`; its difference from ideal exceeds one section's bound.
Floor-base prism volume (before clearance exclusion) is **2,877.75 m³**; it is
not the continuous roof-solid volume or counted area multiplied by 3 m.

For the concave L parcel, 1 m setbacks leave two 28 x 10 m strips sharing
10 x 10 m: **460 m²**. The first three clearance-qualified floors independently
check that area. Every later area is nonnegative/nonincreasing and the last is
zero. Upper concave roof quantities are saved C++ baselines, **not independent
analytic anchors**. Both cases preserve seven baked floor pieces, all ten floor
identities (including zero-area upper floors), exact row/contour order and raw,
allowed, low-headroom and outside-highlight semantics. Malformed/out-of-range/
fractional selectors, missing or invalid payloads and ambiguous envelope sources
throw fixture errors rather than becoming expected domain-refusal snapshots.

### Signed-arc inputs and independent anchors

Slab input objects may supply `outerArcs` and `holeArcs`, in signed **radians**
for the edge leaving each stored vertex. `holeArcs`, when present, has one array
per `holes` entry in the same order. A ring's array may be empty or shorter than
its vertex count: omitted trailing sweeps are straight, matching `SlabSlices::Ring`.
More sweeps than vertices, malformed arrays, non-finite values and nonzero sweeps
of magnitude **>= 2π** throw fixture errors. XY must have complete pairs when arc
metadata is supplied. This is a bounded test-input policy, not a new production
arc validator or a repair algorithm. A circle uses two half-turns or four quarter
turns, never a single full-turn edge. Old arc-free inputs retain their exact path.

The adapter passes those values unchanged to the existing C++ consumers; it does
not tessellate, change signs or infer hole winding. Production still uses
`SlabSlices::Contour` / `PlanOverlayContent::TessellateEdgeDouble`. The inherited
0.05 m **arc-length-per-segment target** (not sagitta tolerance) determines the
chord polygons. Independent inscribed-polygon formulas anchor the actual model:

| Case | Stored contour | Chord polygon / independent area |
| --- | --- | --- |
| `ArcDiscSlices` | 2 positive half-turns, radius 1 m | 126 vertices, `63*sin(π/63)` m² |
| `ClockwiseArcDiscSlices` | 2 negative half-turns, radius 1 m | same unsigned area, negative signed area / clockwise order |
| `ArcAnnularPrism` | outer 4 positive quarter-turns, radius 1 m; hole 2 negative half-turns, radius 0.5 m | 128 / 64 vertices, `64*sin(π/64)-8*sin(π/32)` m² |
| `MajorArcSegmentSlices` | +270° from (1,0) to (0,-1), then a straight closing edge | 96 vertices, `(95*sin(3π/190)+1)/2` m² |
| `ArcAnnularFacade` | same annulus, with 4 negative quarter-turns for the hole | same 128 / 64 chord polygons; facade `9*(256*sin(π/128)+64*sin(π/64))` m² |

The circular slice cases retain three cut levels (including 0.25 m offset from
physical floor bases). The annulus has three 3 m floors: **7.068582102954805 m²**
counted area and **21.205746308864413 m³** floor-prism volume. The separate
four-node-hole facade case independently anchors **84.80597092938268 m²**
chordal vertical facade. These are **tessellated-prism**
answers, not ideal circle/cylinder answers. Independent radius, count, winding
and triangle-fan area checks do not call the production tessellator/area helper.
All three baked annulus pieces must retain one hole; full saved snapshots retain
the actual ordered coordinates, source/storey identity and diagnostics. Existing
absolute `2e-5` analytic and `1e-6` replay bounds stay unchanged. This is neither
native curved-slab capture nor full ArcPolyline metadata/identity interoperability.

**Known production limitation, not fixed here:** the facade's prism-footprint
ingress requires at least three **stored** vertices per ring before tessellation.
It therefore refuses the geometrically valid two-node/two-half-turn hole in
`ArcAnnularPrism`, while slice/headroom/bake planning accept it. The build result
must keep `hasFacade=false` and the exact `Facade area unavailable: Invalid facade
prism contour.` note; its saved numeric zero is **unavailable**, not an independent
zero-area answer. `ArcAnnularFacade` uses equivalent four-node quarter-turns and
does compute the analytic chord-perimeter area. No node padding or rewrite hides
the two-node limitation. An arc-aware facade-ingress repair needs a separate
production change and reviewed baseline update before claiming that case supported.

The snapshots retain raw/allowed/excluded areas and volumes, all five row-contour
categories (including closed flags and winding/order), source GUIDs, storeys,
functions/colours, section parts/spans, building membership/preview sections,
height-control counts, bake request contours/elevations and diagnostic text.
Without an envelope, allowed-area/volume totals remain zero even when display
chains show the raw floor. Those distinct existing semantics are recorded, not
reinterpreted as a missing value or a new fallback rule.
The adapter calls production computations; it does not duplicate their geometry.
All mesh coordinates/indices are committed data, not regenerated by a primitive
builder that might be ported at the same time. GUIDs are synthetic test labels,
not claims of native model capture or authored D14 identity.

The analytic anchors normally allow `2e-5` absolute numerical error. The tapered
fixture alone allows `1e-4 m³` for its ideal floor-base prism-volume anchor, as in
the source regression: `SliceEngine` nudges a cut off the first coplanar base face
by `1e-6 m`, shifting the sum about `3.3e-5 m³`. Its captured snapshot still uses
the unchanged `1e-6` comparison tolerance. The ideal volume is
`3*(100 + 625/9 + 400/9) = 1925/3 m³`: **floor-prism accounting, not the true
frustum volume**. Headroom exclusion changes counted floor area independently.

## Acceptance limits

This is an initial, bounded regression corpus, not full S2b or a Rust parity run.
When G2/G3 implementations and their flags arrive, replay these same inputs
against both compiled backends before any production family flip. Full migration
acceptance still needs real-building/native SEO captures, native/operated curved
body fixtures and broader
disconnected-body coverage beyond these synthetic/workflow fixtures, fresh Python
workflow generation parity (the committed shell payloads are replayed, not rebuilt),
complete preview rendering,
baked native GUID/Undo stability, the live consumer matrix and reproducible
latency/allocation/copy/peak-memory measurements. The existing tests remain in
place; this corpus neither replaces them nor creates a second geometry authority.
