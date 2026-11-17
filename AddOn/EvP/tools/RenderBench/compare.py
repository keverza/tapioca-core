"""Compare RenderBench pictures against the converged ray-traced reference.

For every <view>_<style>.png beside a <view>_reference.png in a bench output folder:
  * the mean absolute error in LINEAR light (sRGB decoded), over all pixels and over the pixels
    the reference calls reflective-relevant (where it differs from the 'architecture' look, when
    that style was rendered -- otherwise all pixels);
  * a montage: the style, the reference, and a heat map of |style - reference| (x4, so a 25 %
    error saturates), written as <view>_compare.png.
Writes compare.json beside them and prints one line per image.

    python compare.py <bench-output-dir> [<dir> ...]
Runs on the Tapioca runtime's Python (numpy + Pillow).
"""

import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def linear(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)


def heat(err):
    e = np.clip(err.mean(axis=2) * 4.0, 0.0, 1.0)
    r = np.clip(e * 2.0, 0, 1)
    g = np.clip(e * 2.0 - 0.5, 0, 1)
    b = np.clip(1.0 - e * 3.0, 0, 1) * (e > 0.002)
    return (np.stack([r, g, b], axis=2) * 255).astype(np.uint8)


def label(img, text):
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 8 + 7 * len(text), 18], fill=(0, 0, 0))
    d.text((4, 3), text, fill=(255, 255, 255))
    return img


def compare(folder):
    folder = Path(folder)
    results = []
    for ref_path in sorted(folder.glob("*_reference.png")):
        view = ref_path.name[: -len("_reference.png")]
        ref = linear(ref_path)
        # The glossy mask: the G-buffer material view, roughness in red written LINEAR into the
        # sRGB target -- so decoded it is roughness. Background is all zero and excluded.
        mask_path = folder / f"{view}_mask.png"
        glossy = None
        if mask_path.exists():
            m = linear(mask_path)
            glossy = (m[..., 0] < 0.35) & (m.sum(axis=2) > 1.0e-4)
        rows = []
        for style_path in sorted(folder.glob(view + "_*.png")):
            style = style_path.stem[len(view) + 1:]
            if style in ("reference", "compare", "mask"):
                continue
            img = linear(style_path)
            if img.shape != ref.shape:
                continue
            if glossy is not None and glossy.shape != img.shape[:2]:
                glossy = None
            err = np.abs(img - ref)
            mae = float(err.mean())
            p95 = float(np.percentile(err.mean(axis=2), 95))
            gloss = float(err[glossy].mean()) if glossy is not None and glossy.any() else float("nan")
            share = float(glossy.mean()) if glossy is not None else 0.0
            results.append({"view": view, "style": style, "mae": round(mae, 5), "p95": round(p95, 5),
                            "glossyMae": round(gloss, 5), "glossyShare": round(share, 4)})
            print(f"{folder.name:20s} {view:10s} {style:10s} MAE {mae:.4f}  p95 {p95:.4f}  "
                  f"glossy MAE {gloss:.4f} over {share * 100:.1f}% of pixels")
            rows.append([
                label(Image.open(style_path).convert("RGB"), f"{style}  MAE {mae:.4f}  glossy {gloss:.4f}"),
                label(Image.open(ref_path).convert("RGB"), "reference (converged path trace)"),
                label(Image.fromarray(heat(err)), "|error| x4"),
            ])
        if not rows:
            continue
        w, h = rows[0][0].size
        scale = 0.5 if w > 900 else 1.0
        tw, th = int(w * scale), int(h * scale)
        sheet = Image.new("RGB", (tw * 3, th * len(rows)), (40, 40, 40))
        for r, row in enumerate(rows):
            for c, tile in enumerate(row):
                sheet.paste(tile.resize((tw, th)), (c * tw, r * th))
        sheet.save(folder / f"{view}_compare.png")
    (folder / "compare.json").write_text(json.dumps(results, indent=2))
    return results


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        compare(arg)
