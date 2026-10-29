# Independent region boundary terrain regression

`region-boundary-20261029.debug.json` retains the exact worker input values from
`virtual-editor__20261029_084613_540727Z_0.json`, exported by the user on
2026-10-29. Only display caches, UI/status metadata and the obsolete failed
outcome are omitted. The original export is not changed.

- Original export SHA256:
  `bb45e8294b2f173f4bc2473d986c414816bbb1bff0335a87794f9fa659d0cbd7`.
- Native Mesh: 24 render vertices, 12 triangles; Rust upward extraction compacts
  it to four vertices and two triangles at Z=-0.2 m.
- Terrain owner: path object 11 / operation 10. The independent region belongs
  to object 36 / operation 35, has target Z=0 m, and uses daylight transitions.
- Before the fix, generated derived-ring constraints named terrain owner 11 but
  only operation 35. Since 35 does not descend from 10, strict provenance
  validation correctly refused `ProvenanceUnattributable` before splitting.
- The repair retains both the terrain owner's operation and the boundary's
  operation, matching existing authored-property preparation. It does not invent
  a DAG dependency, change native capture identity, or relax Rust validation.

`EditRegionTerrain.CapturedNativeTerrainWithIndependentBoundaryBuildsBothRegionAndPath`
tests region-only and combined path/region output, target/baseline heights,
retained native capture, stage-input provenance and deterministic replay. The
original combined scene produces 9,626 preview triangles after the fix; the test
checks geometry rather than freezing that triangulation count. The existing
outline lane remains preview-only: this does not certify final path-face
provenance or native output. The separate #82 zero-length splitter fixture still
refuses atomically and is not repaired by this change.
