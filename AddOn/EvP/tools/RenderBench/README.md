# RenderBench

The viewer's renderer outside Archicad: load an OBJ scene, render it at fixed cameras in several
display styles, and compare each picture against a converged path-traced reference.

It compiles the add-on's own `DiligentScene` sources (listed in `Sources.txt`) and drives a frame
in `DiligentViewport`'s order, so a fault in a bench picture is a fault in the viewer. The scene's
materials go through Archicad's channels (shine factor, specular amount and colour, opacity), so
the bench renders only what Archicad could send.

## Build

```powershell
cmake --build build_29 --config RelWithDebInfo --target TapiocaRenderBench
```

From `AddOn/EvP`. The target is `EXCLUDE_FROM_ALL`; the add-on build never builds it. It links
with `/FORCE:UNRESOLVED` for the feature layers the bench never feeds (see `cmake/RenderBench.cmake`);
a call into one crashes with a symbolised stack rather than drawing a wrong picture.

## Run

```powershell
python tools/RenderBench/run_suite.py --scenes C:/Tests/extracted --out C:/Tests/bench
```

Use the Tapioca runtime's Python (numpy and Pillow). `--scenes` holds McGuire's Computer Graphics
Archive OBJ scenes, one extracted folder each: `CornellBox`, `salle_de_bain`, `living_room` and
`fireplace_room`. They are not in the repository. A missing scene is skipped.

The `glazing` case is the bench's own fixture, in `scenes/` (regenerate it with
`scenes/make_glazing.py`): a room with a glazed front, a red back wall behind the pane and a white
block outside it. The archive scenes have no architectural glass, so it is the only case that
compares how the styles draw a pane (#231). Its pane is not in the G-buffer, so `glossy MAE` is
empty there; read the montage and the whole-image error.

Textures: the raster styles and the Ray traced reference both draw the scene's `map_Kd` images
(#252, #231), gated by the same switch, so `--extra "--off textures"` turns them off on both sides.
The log's `textured materials N` says how many materials the traced passes sampled.

Each scene folder gets these files:

- `<view>_<style>.png` for every style.
- `<view>_compare.png`: the style, the reference, and the error ×4.
- `compare.json`.

The whole run's summary goes to `suite.json` in `--out`.

| Style | What it is |
|---|---|
| `ssr` | Rendered with screen-space reflections: what every non-DXR machine sees. |
| `traced` | Rendered with traced reflections (DXR 1.1). |
| `reference` | The Ray traced style, converged (`--samples`). |
| `architecture` | The Architecture preset. |
| `clay` | The Clay preset: Architecture's light over the clay material, TAA on. |
| `technical` | The Technical preset: clay in the Fast light, no AO or shadows. OBJ scenes carry no Archicad edge data, so its edges do not draw here. |
| `mask` | The G-buffer material view, which `compare.py` uses to find the glossy pixels. |
| `shadow` | The raster sun visibility alone (the shadow map's `shadowsOnly` view): white lit, black occluded. |
| `sunvis` | The sun visibility traced on the CPU (`SunVisibility.cpp`): 255 lit, 0 occluded, 128 not a shadow question (sky, glazing, surfaces facing away from the sun). Needs no GPU ray tracing. |
| `sunsoft` | `sunvis` with the sun as a disk of the PCSS sun diameter (`--shadow-sun-size`, 64 rays): 0..250 the fraction of the disk in view, 255 not a shadow question. The truth for a finite sun, penumbra included. |

`shadow` and `sunvis` match pixel for pixel, so a pixel `sunvis` calls occluded and `shadow` calls
lit is a shadow-map leak (#234). This is the shadow reference on machines without DXR, where
`reference` reports "unavailable".

`glossy MAE` is the mean absolute error in linear light over pixels with roughness < 0.35. It is
the reflection number; `MAE` over the whole image also carries the ambient-light differences.

## Isolating an effect

Pass bench arguments through `run_suite.py --extra "..."`, or call the exe directly:

- `--off ao,ssr,taa,shadows,env,converge` switches effects off in every style. Turn one off and watch
  whether an artefact goes with it. `converge` is the still-camera mean (#247/#248): off, TAA alone
  presents the image, as before it existed. `overlays` keeps the jitter on everything drawn after the
  resolve (tint, section, wireframe, edges, outlines) instead of redrawing the depth unjittered for
  them (#248). `idle` keeps a converged view rendering every pass instead of presenting the mean
  without drawing the scene (#247).
- `--temporal-change K` moves the white balance to 4500 K from the K-th `--temporal` frame on: a
  content change made while converged and idle, which must restart the mean and reach the picture
  one frame later.
- `--edges` turns the technical edges on, so `--temporal` sees the lines #248 reports moving.
- `--temporal N` measures a still camera: after the picture is saved, N more frames are rendered and
  read back, and the log line `bench: temporal` reports the pixels changing by more than 24 levels
  between consecutive frames (worst, after 16 frames, last pair) and the frame from which nothing
  changes at all, plus how many frames were idle (presented the converged mean without drawing the
  scene) and how many times a still image restarted, with the last reason.
  `<view>_<style>_temporal.csv` has one row per frame (changed pixels, largest change, mean level,
  idle) and `<view>_<style>_still.png` is the last frame. Use this, not `--frames N`
  sweeps, to look at history over time: without a readback per frame the frames are queued, and
  effects that build their pipelines asynchronously switch on after a wall-clock delay, not a frame
  count.
- `--rt-budget MS` sets the Ray traced style's GPU time per frame (#231; default 8, 0 = one sample a
  frame). The reference's log line reports the frames it took to converge and the measured
  samples/frame and ms/sample; the picture is the same at any budget, only the frame count changes.
- `--rt-fade S` fades the Ray traced image in over S seconds of wall clock (#14; default 0). The
  viewer also holds it back until the view has been still 0.2 s and fades over 0.2 s; the bench
  sets both to 0 by default because its frames are not paced, so timing would decide the picture.
- `--converge-settle N` and `--converge-samples N` override the still-camera mean's settle frames and
  sample target (`TemporalConvergenceSettings`).
- `--shadow-bias`, `--shadow-clamp`, `--shadow-filter` and `--shadow-pcss 0|1` override the
  shadow map's fixed depth bias, receiver-plane clamp, filter size in metres, and PCSS.
- `--shadow-resolution N` sets the shadow map's size; `--shadow-fit 0` splits the cascades over the
  projection's whole near..far, as before #234, for an A/B against the scene-fitted range.
- `--shadow-sun-size DEG` and `--shadow-search M` set the PCSS sun diameter and the blocker search's
  cap; `--shadow-diag N` makes the `shadow` style show one stage of the PCSS lookup instead of its
  light amount (`DiligentShadowSettings::diagnostic`: the hard comparison, the blocker search, the
  filter radius, the cascade, or 7 = the search before #234).
- `--section x|y|z,POS[,HATCH_MM[,ANGLE]]` turns on the viewer's section plane, with caps, at POS
  (0..1 across the model's bounds), and hatches the caps when a spacing is given (#251). It is set as
  HUD state, because each frame applies the HUD's look, which owns the plane. The caps are cut on a
  worker, so the bench keeps rendering until they land, and logs how many elements were cut, closed,
  left open or refused.

## Adding a source

When the link reports a new unresolved renderer symbol, `grow_sources.py` finds the source that
defines it. Its matching is loose (bare names), so read what it proposes. A source that reaches the
DevKit never belongs in the list; stub the service in `BenchStubs.cpp` instead, and only with an
answer that is a real state of the viewer.
