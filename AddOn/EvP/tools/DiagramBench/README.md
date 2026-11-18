# DiagramBench

The viewer's exploded massing diagram, outside Archicad. It replays a diagram capture and renders it
through `DiligentScene::DrawDiagram`, the viewer's own draw. Use it to check a storey gap, a
per-building gap or a colouring without starting Archicad.

## Get a capture

In the Tapioca viewer, open the Massing tab (or the Massing workflow's *Exploded diagram* section)
and press **Save capture (JSON)**. The file lands in `%LOCALAPPDATA%\Tapioca\dumps\diagram-capture-<epoch>.json`.

## Build

```powershell
cmake --build build_29 --config RelWithDebInfo --target TapiocaDiagramBench
```

Run it from `AddOn/EvP`. The target is `EXCLUDE_FROM_ALL`, so the add-on build never builds it.

It reuses RenderBench's renderer list (`tools/RenderBench/Sources.txt`) and stubs
(`BenchStubs.cpp`), and adds the diagram's pure sources (`cmake/DiagramBench.cmake`). Like
RenderBench, it links with `/FORCE:UNRESOLVED`. A call into a layer the bench never feeds crashes;
it never draws a wrong picture.

## Run

```powershell
build_29\DiagramBench\TapiocaDiagramBench.exe --capture diagram-capture.json --out out `
    --gaps 0,3,8 --colour both --views iso,front,side,top --building building:A=12
```

| Option | Meaning |
|---|---|
| `--gaps` | Storey gaps in metres; one picture set per gap. |
| `--building key=m` | A per-building gap. The bench prints every building key at start. |
| `--colour` | `function`, `white` or `both`. |
| `--views` | `iso`, `front`, `side`, `top`. Each view is framed on the spaced diagram's bounds. |
| `--lines` | `on` (default) or `off`: the storey outlines and the dashed corner lines. |
| `--size` | `WxH`, 1280x800 by default. |

Each picture is `<view>_gap<g>_<colour>.png`. The console says how many draws and uploads it took.
Uploads stay at 1 across every gap and colour, because spacing moves translations, not bytes.
