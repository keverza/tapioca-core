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

The single-day and point-playback clocks are interactive: clicking a radial bar selects that measured
step; dragging clockwise selects a half-open time slice. A single-day brush drives the single-shadow
view or time fan, while a period-point brush filters only the independent point player. The shared
shadow-hours range is non-wrapping, so a drag that reaches the top from the left is explicitly
constrained at 24:00 rather than silently wrapping into a second interval.

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
