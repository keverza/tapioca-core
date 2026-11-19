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

- `--off ao,ssr,taa,shadows,env` switches effects off in every style. Turn one off and watch
  whether an artefact goes with it.
- `--shadow-bias`, `--shadow-clamp`, `--shadow-filter` and `--shadow-pcss 0|1` override the
  shadow map's fixed depth bias, receiver-plane clamp, filter size in metres, and PCSS.
- `--shadow-resolution N` sets the shadow map's size; `--shadow-fit 0` splits the cascades over the
  projection's whole near..far, as before #234, for an A/B against the scene-fitted range.
- `--shadow-sun-size DEG` and `--shadow-search M` set the PCSS sun diameter and the blocker search's
  cap; `--shadow-diag N` makes the `shadow` style show one stage of the PCSS lookup instead of its
  light amount (`DiligentShadowSettings::diagnostic`: the hard comparison, the blocker search, the
  filter radius, the cascade, or 7 = the search before #234).

## Adding a source

When the link reports a new unresolved renderer symbol, `grow_sources.py` finds the source that
defines it. Its matching is loose (bare names), so read what it proposes. A source that reaches the
DevKit never belongs in the list; stub the service in `BenchStubs.cpp` instead, and only with an
answer that is a real state of the viewer.
