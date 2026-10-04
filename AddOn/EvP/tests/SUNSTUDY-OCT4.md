# October 4 large-model follow-up

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
Final artifact: `AddOn/EvP/build_29/EvP.apx`, SHA-256
`874439AE5D4673645E481D33A3D8A9349B29470528F777071067C239BACCFB3F`.
