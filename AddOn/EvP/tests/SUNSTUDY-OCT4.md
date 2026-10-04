# October 4 large-model follow-up

## Latest 1 m / 15-minute run and second follow-up

The newer supplied run has 2,084 meshes, 818,230 triangles, 599,137 samples and
43 daylight steps: **CPU 151,974 ms; GPU 645,917 ms**. Compact statistics agree,
but this still does not establish every-bit live parity. The GPU step logs sum
to 642,184 ms in CPU checks, 31,048 ms in GPU compute (overlapped), and 2,943 ms
waiting for readback. 5,393,327 of 14,010,760 traced GPU rays exhaust the work
limit; numerical ambiguity is zero in this run. Upload/readback tuning alone
cannot solve this bottleneck.

Implemented, without reducing resolution or weakening predicates:

1. `828484e`: CPU workers dynamically claim 256-ray batches instead of static
   surface-ordered shards. Cancellation is polled only on the submitting thread.
   Partition tracing short-circuits Context for rays already blocked by Analysis.
   GPU fallback/validation collects bounded 65,536-ray waves across packets,
   allowing automatic CPU fan-out beyond the former two-thread 4,096-ray chunks.
   The three GPU slots, FP64 guards, work ceiling and whole-step replay remain.
2. `ff6f6ec`: changed old/new geometry bounds invalidate individual sample/time
   pairs, not a receiver's entire day. Exact unchanged position/normal/identity
   checks remain mandatory; unmatched samples recompute every step. Unchanged
   back-facing samples retain their proven self-shadowing. New telemetry reports
   `reusedSampleSteps` and `dirtySampleSteps`; partly reused days also reject
   altered ray bounds. Tests compare complete bitsets against fresh calculations.
3. `50b7586`: **Sunstudy / Shadows / Roles** replace the diagnostic view picker.
   Shared **Off / Cursor / Panel** hover buttons sit directly below those tabs.
   Panel readings reserve a fixed three-line viewport, including no-hit states.
   Cursor reading backgrounds are 70% opaque with 4 x 2 px padding. Analysis
   renders the actual model with matte-white materials and existing lighting/
   shadows beneath a 90% straight-alpha study overlay. Glass is opaque in this
   display-only base pass, preventing order-dependent stacked study colours;
   original material/render settings return when the study is hidden. Selection
   outlines remain. Low-sun blue blends at 60% over the warm ramp instead of
   replacing it, including in the gradient legend. Diagnostic colours stay opaque.

### Why Jifto remains a separate performance target

[Jifto's documentation](https://jifto.com/docs/) identifies NVIDIA OptiX as its
sunlight/shadow tracer. Our current D3D11 FP64 compute traversal does **not** use
RTX ray-tracing hardware. An OptiX/RTX backend with conservative acceleration
bounds and an exact intersection/fallback strategy is the next substantial
architecture option, not an implemented feature or a promised speed factor.
Neither the current changes nor that option justify coarser samples, removing
ambiguous-ray checks, or silently changing manual CPU/GPU or Early/Late choices.

### Latest validation and acceptance

- 2,071 C++ tests pass; two existing tests remain disabled. New tests cover CPU
  batch coverage/cancellation, multi-packet fallback waves, per-time reuse across
  64-bit word boundaries, custom-bound refusal, actual ImGui hover controls and
  fixed reading space, and real D3D readback of overlay alpha/blue blending.
- 51 focused Python tests pass. Full add-on pytest: 576 passed, 6 skipped, the
  same three previously confirmed baseline failures (scene-text seam, Dynamo
  packaging, preview routing). Updated Python tests pass lint/format checks.
- 35 HLSL stages compile; C++ architecture and touched-file style checks pass.
- Final native `RelWithDebInfo` `EvPAddOn` build succeeds. Artifact:
  `AddOn/EvP/build_29/EvP.apx`, SHA-256
  `812350B2CE2D8D8C84471D0B1A2BDF90C048DADF76FD64873D7E21EC57FB6100`.
- Evidence: `C:/Users/Dever/AppData/Local/Temp/opencode/sun-oct4b-*.log`.

No deployment, Rescan, push or new live benchmark was performed. Preserve
Context/Ignored picks. Live acceptance must repeat the identical workload and
compare **all per-step bits**, timing/fallback counts, per-time edit reuse,
cancellation/navigation and the new opacity/white-model/DPI/hover appearance.
The 53 missing solid meshes and dense atlas coverage concerns remain unresolved.

## Earlier follow-up (2 m / hourly)

## Live evidence supplied by the user

`Z:/logs/sun_study_native_smoke.log` compares the same large-model, early,
2 m patch-grid, hourly study: GPU advancement 150,968 ms, CPU 57,912 ms.
Both report 542,971 samples, 12 daylight steps and the same compact statistics;
aggregate agreement is not evidence of every-bit parity.
`archviz.1.log` records GPU rays exhausting the 4,096-work budget and spending
almost all step time in exact CPU fallback/validation. Shared capture, compact
results and cached display preparation are no longer the principal cost.
The supplied capture still reports 53 missing solid meshes; none of these
changes establishes whole-model accuracy.

## Traversal follow-up

- CPU occlusion now returns on the first blocker, using the existing nanort BVH,
  its double-precision slab and watertight triangle predicates, and the same
  inclusive lower/strict upper distance bounds. Closest-hit queries are unchanged.
- GPU branches visit the direction-near child first, instead of always left first.
  The work ceiling, numerical guards, exact CPU resolution and mismatch replay
  remain. Stack overflow also requests exact CPU fallback.
- Offline validation: 28 CPU/GPU traversal tests pass, including 2,000 randomized
  georeferenced multi-branch reference comparisons and interval-boundary cases.
  D3D validation uses the available offline adapter, not live Archicad timing.

Rebuild the native add-on before deployment. Compare every CPU/GPU result bit and
fallback counts on the user's identical model/settings before claiming a speedup.
No deployment or live run is part of this follow-up's offline validation.

## Preview and palette follow-up

- The viewer's persistent **Analysis** tab contains **Sun hours / Shadows**.
- Sunlight duration uses one two-handle, full-day range. Outside values remain
  neutral; the ineffective "hide the rest" UI has been removed.
- Optional **Low-sun blue** draws values strictly below the threshold in
  `#28536B`, initially 2.5 h. Drag its blue box along the gradient in quarter
  hours. This changes the display only, not samples or calculation.
- The main multiple-shadow preview weights the purple morning and salmon evening
  colours by the fraction of each measured half-day shadowed, split at solar
  noon. A selectable clock-time range limits that preview and single-time
  playback. Clock ranges use an exclusive end, like the calculation inputs.
  The old last-shadow time fan remains labelled as diagnostic.
- Measured cursor tooltips contain only `H:MMh`, black on rounded grey. Unmeasured
  and diagnostic explanations remain available through the panel inspector.
- `Calendar` is the native DG calendar with canonical local `YYYY-MM-DD` readback,
  not a raw GSTime epoch. Supported full years are 1902..2037 (DG Int32 limit).
  Native `Hour(maximum=24)` explicitly supports an end-of-day endpoint; ordinary
  `Hour` remains 0..23. Smoke inputs now use these controls and send date year,
  month and day explicitly. Manual Early/Late and CPU/GPU options are unchanged.

Live acceptance must still verify native calendar behaviour, DPI-scaled dragging,
hover accuracy, the morning/evening visual distinction and dense-model coverage.

## Offline validation

- 2,060 C++ tests pass; two pre-existing tests are disabled. Tests include real
  ImGui handle/threshold dragging, full tint-shader compilation, and D3D readback
  of the exact blue cutoff, interval shadow fractions and morning/evening colours.
- 62 focused Python tests pass. Add-on pytest: 575 passed, 6 skipped, 3 failed. All
  three failures reproduce unchanged at pre-task `5e9592b`: the relocated scene-
  text live-check seam, Dynamo template packaging and preview input routing.
- 35 embedded HLSL stages compile. Native `RelWithDebInfo` add-on build passes.
- Command roots scan successfully; CPU/compact and GPU/fresh/detailed smoke dry
  runs each complete twice, and UIShowcase Temporal executes without native calls.
- C++ architecture/style checks pass. Python lint passes on new code and updated
  command/test files. The legacy `evp/__init__.py` import ordering and `_scanner.py`
  B905/B904 findings reproduce at `5e9592b`; legacy formatting debt in these and
  schema/ports/UIShowcase is retained rather than mass-formatted.

Validation logs: `C:/Users/Dever/AppData/Local/Temp/opencode/sun-oct4-*.log`.
Earlier artifact: `AddOn/EvP/build_29/EvP.apx`, SHA-256
`874439AE5D4673645E481D33A3D8A9349B29470528F777071067C239BACCFB3F`.
