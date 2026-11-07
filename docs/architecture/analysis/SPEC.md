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

## Direct-Sun Period Result

A period study measures direct sun over a date range within one calendar year (or the whole
year) on **one** scene: the snapshot, element roles, analysis cylinder and place settings are
captured once, and each traced day recaptures only its sun positions. A day that differs in its
samples from the first is refused, never mixed. A failed host sun-position call refuses the
period; a gap is never read as shade.

Schedules (`rust/sunstudy/src/annual.rs`):

- **Preview** traces one representative day per covered month, the middle covered day, weighted
  by the covered days it stands for. Partial months weigh only their covered days. It is labelled
  an estimate wherever it is shown.
- **Every day** traces each covered day at weight one. It is the reference, still limited by the
  timestep, the time slice and the sample grid; it is not called exact.

Quantities, per sample and month, from one schedule:

`0 <= unoccluded <= surface facing <= site daylight`

Site daylight is the sun above the altitude floor inside the daily time slice (one scalar per
month). Surface facing uses the day study's own incidence cull. Obstruction loss is
`surface facing - unoccluded`; back-facing time is never reported as shade. Sunless days count
at zero hours. Totals are `f64`, summed in day order, stored sample-major `[sample][month]` for
unoccluded and facing, plus the 14 daily-hours histogram bins: 24 `f64` and 14 `f32` values,
**248 bytes a sample** before allocator overhead, whatever the period. No step mask is retained by
default; the optional whole-map playback archive is additional.

Playback has two explicitly different scopes. **Keep whole-map days for playback** opts into a whole-map,
in-memory archive of each traced day's lit/facing bits; it is not a file and is released with the
retained period. Its bytes must be included in the run's memory estimate. The low-memory alternative
is **Detailed point playback**: on demand, re-trace only the placed study point's cells against the
retained period's sun directions and CPU traversal, then retain the averaged per-step series for
that point. It can play every step, a fixed clock time, or day by day without archiving the whole
map. It cannot provide whole-map historical frames, and a Preview profile still represents only
the days actually traced.

**Free period cache** releases the retained totals, carrier record, point profile and optional
whole-map archive when no run or period worker is active. Releasing a period also erases its exact
carrier revision from `SunStudyStore`; it never clears unrelated studies or hides the overlay. The
renderer keeps its published frame and GPU textures, while period browsing, playback and point reads
become unavailable until another run. Repeated clears are idempotent, and cancellation or display
failure after retention uses the same cleanup.

The single-day and point-playback clocks are interactive: hovering outlines a radial bar, clicking selects
that measured step, and dragging clockwise selects a half-open time slice. A single-day gesture updates the
step/range **without changing the current Sunstudy/Shadows/Obstruction/Roles view**; a period-point brush filters
only the independent point player. The main single-day clock has a 352 px cap (1.6× the original 220 px), while
period and seasonal clocks retain their compact size. With no placed point, the exact measured hover cell supplies
the single-day clock and remains latched while the pointer enters the panel; a placed point pins its cell sphere
and takes precedence. The day is cached by displayed-result version and ordered samples, with transient read
failures retried rather than negatively cached. The shared shadow-hours range is non-wrapping, so a drag that
reaches the top from the left is explicitly constrained at 24:00 rather than silently wrapping into a second
interval. For a multi-cell study point,
the centre remains mean direct hours and the summary reports the exact per-cell minimum–maximum total.
Single-day ranges come from each cell's measured steps; period playback computes the range from each cell's
re-traced daily total before averaging. Back-facing time contributes no direct hours, and averaged step shares
are never used to infer this range.

A completed period point profile can additionally show display-only **Seasonal clocks**: fixed 2×2 DJF/MAM/JJA/SON
small multiples, including empty-season placeholders. Centres are weighted mean direct hours per traced day;
Preview retains its representative-day weights and is labelled an estimate. Each clock step is averaged only over
days that measured that sun time. Weights do not create extra frames, and seasonal aggregates make no per-day cell-range
claim. They use no whole-map archive and have no independent brush/selection state.

A single-day placed study point also offers **exact obstruction attribution** through the dedicated
**Obstruction** view beside Sunstudy/Shadows/Roles. **Time step** re-traces the selected cells at the clock's selected measured step; **Whole day** re-traces every measured
step. Face culling happens before the ray query. Analysis and context `QueryEngine`s are queried separately
and the nearer hit wins; the result identifies the source object and mesh-local triangle rather than merely
tinting a whole object. Custom ray bounds are refused until their numeric values are retained. Requests are
revision/snapshot/sample/mode/step keyed, cancellable and latest-wins; an edited scene cannot display a
historical result.

Every blocking object is drawn once as a source/CAD-boundary wireframe (all triangle edges are the fallback
when that metadata is absent). Only exact hit triangles are filled: pink for Time step, and a 00:00–24:00
colour ramp for Whole day. Whole-day fills retain distinct timed hits, deduplicated across selected cells;
multiple times on one part are drawn chronologically with the latest foremost. This is nearest-first
attribution, not a claim to enumerate every downstream blocker.

Presentation: the viewer shows the **mean unoccluded hours a represented day** over the period,
drawn through the last traced day's samples and atlas, so the day study's hour bands apply. That
day's step masks remain the shadow views' source and are labelled with their date. A period is
never re-run by the edit follower. Cancellation or failure publishes nothing and erases every
day record it created; the previously shown study stays shown.

Not yet in this contract: per-element/area-weighted summaries, named regulatory evaluation,
historical time-zone/DST schedules (times are the project clock as the host gives it), date
queries against a retained period scene, sky-component studies, and exports.

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
