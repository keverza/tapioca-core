# October 4 large-model follow-up

## Live evidence supplied by the user

`Z:/logs/sun_study_native_smoke.log` compares the same large-model, early,
1 m patch-grid, hourly study: GPU advancement approximately 151 s, CPU 58 s.
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
