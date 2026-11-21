"""Render the reflection test suite with TapiocaRenderBench and compare every picture.

The scenes are NOT in the repository (licences vary, sizes reach hundreds of MB): point --scenes
at a folder holding McGuire's Computer Graphics Archive OBJ scenes, extracted one folder each
(CornellBox/, salle_de_bain/, living_room/, fireplace_room/). A missing scene is skipped.

    python run_suite.py --scenes C:/Tests/extracted --out C:/Tests/bench [--only cornell-mirror]
        [--samples 512] [--size 960x540] [--styles mask,ssr,traced,reference]
Then compare.py runs on each output folder; the summary lands in <out>/suite.json.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
EVP = HERE.parents[1]
BENCH = EVP / "build_29" / "RenderBench" / "TapiocaRenderBench.exe"

sys.path.insert(0, str(HERE))
import compare  # noqa: E402

# Each case: the file, the scale to metres, and the views -- name:eye:target:fov, Z-up metres
# after the Y-up rotation. Picked so each view holds one reflection failure the viewer showed:
#   cornell-mirror   a mirror facing the camera: its reflection lies behind the camera (offscreen)
#   cornell-glossy   a glossy floor under objects: contact and occluded reflections
#   bathroom         a real wall mirror and glossy tiles, reflecting what is behind the camera
#   living           glossy floor, long room: far objects in the reflection
#   fireplace        polished floor, small room, mixed materials
#   cornell-sphere   two glossy spheres (#231): a tessellated curve, faceted wherever a pass
#                    shades with the flat triangle normal instead of the vertex normals
#   louvres          30 mm slats at 60 mm centres over a glazed box (#14): sub-pixel detail that
#                    shimmers while the camera moves; score it with --extra "--motion 48".
#   glazing          a glazed front (#231): the sky and a block in the pane, a red wall through it.
#                    The bench's own fixture (scenes/, from make_glazing.py); a path starting with
#                    "@" is relative to that folder, not to --scenes.
SCENES = {
    "cornell-mirror": (
        "CornellBox/CornellBox-Mirror.obj",
        1.0,
        [
            "front:0,-3.6,1:0,0,1:40",
            "oblique:0.8,-2.2,1.5:-0.2,0.2,0.8:50",
        ],
    ),
    "cornell-glossy": (
        "CornellBox/CornellBox-Glossy-Floor.obj",
        1.0,
        [
            "front:0,-3.6,1:0,0,1:40",
            "low:0.3,-2.0,0.35:0,0.4,0.4:55",
        ],
    ),
    "cornell-sphere": (
        "CornellBox/CornellBox-Sphere.obj",
        1.0,
        [
            "front:0,-3.6,1:0,0,1:40",
            "close:0.2,-1.4,0.6:0,0.3,0.4:50",
        ],
    ),
    "bathroom": (
        "salle_de_bain/salle_de_bain.obj",
        0.1,
        [
            "mirror:0,-1.9,1.6:0,2.3,1.4:65",
            "tiles:1.2,-1.6,1.5:-0.6,1.5,0.6:65",
        ],
    ),
    "living": (
        "living_room/living_room.obj",
        1.0,
        [
            "long:0.3,0.1,1.4:0.3,-6.0,0.9:60",
            "floor:2.5,-1.0,1.0:-1.5,-5.5,0.3:60",
        ],
    ),
    "fireplace": (
        "fireplace_room/fireplace_room.obj",
        1.0,
        [
            "room:4.8,1.5,1.5:0,1.5,1.0:65",
        ],
    ),
    "louvres": (
        "@louvres.obj",
        1.0,
        [
            "aerial:-22,-30,20:0,2,3.5:45",
            "street:-9,-14,4:0,0,4:50",
        ],
    ),
    "glazing": (
        "@glazing.obj",
        1.0,
        [
            "facade:5,-8,1.7:-1,2,1.4:55",
            "front:0,-6,1.5:0,3,1.4:50",
        ],
    ),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenes", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--samples", default="512")
    ap.add_argument("--frames", default="48")
    ap.add_argument("--size", default="960x540")
    ap.add_argument("--styles", default="mask,ssr,traced,reference")
    ap.add_argument("--sun", default="250,30")
    ap.add_argument("--extra", default="", help="extra bench arguments, space separated")
    args = ap.parse_args()
    only = set(filter(None, args.only.split(",")))
    summary = {}
    for name, (rel, scale, views) in SCENES.items():
        if only and name not in only:
            continue
        scene = HERE / "scenes" / rel[1:] if rel.startswith("@") else Path(args.scenes) / rel
        if not scene.exists():
            print(f"skip {name}: {scene} not found")
            continue
        out = Path(args.out) / name
        out.mkdir(parents=True, exist_ok=True)
        for old in out.glob("*.png"):
            old.unlink()
        cmd = (
            [
                str(BENCH),
                "--scene",
                str(scene),
                "--yup",
                "--scale",
                str(scale),
                "--out",
                str(out),
                "--size",
                args.size,
                "--samples",
                args.samples,
                "--frames",
                args.frames,
                "--styles",
                args.styles,
                "--sun",
                args.sun,
            ]
            + [a for v in views for a in ("--view", v)]
            + args.extra.split()
        )
        run = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        lines = [l for l in run.stderr.splitlines() if l.startswith("bench:") and "material" not in l]
        print("\n".join(lines[-len(views) * 4 :]))
        if run.returncode != 0:
            print(f"{name}: bench exit {run.returncode}")
            print("\n".join(run.stderr.splitlines()[-20:]))
            continue
        summary[name] = compare.compare(out)
    (Path(args.out) / "suite.json").write_text(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
