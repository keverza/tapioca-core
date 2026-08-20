"""Compare renderer captures with region-specific image tolerances.

The comparison is deliberately offline. A JSON manifest names the masks and
thresholds, so a renderer change can be checked without Archicad or a GPU.
The reported Delta E is CIE76 in CIELAB space; it is simple, deterministic,
and sufficient for the coarse regression gate this harness provides.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image, ImageDraw


class ImageComparisonError(ValueError):
    """Raised when a capture or region manifest cannot be compared."""


@dataclass(frozen=True)
class RegionResult:
    name: str
    pixels: int
    ssim: float
    ssim_min: float
    delta_e: float
    delta_e_max: float
    passed: bool


@dataclass(frozen=True)
class ComparisonReport:
    reference: str
    candidate: str
    passed: bool
    regions: tuple[RegionResult, ...]

    def as_dict(self) -> dict[str, Any]:
        return {
            "reference": self.reference,
            "candidate": self.candidate,
            "passed": self.passed,
            "regions": [asdict(region) for region in self.regions],
        }


def _load_rgb(path: Path) -> np.ndarray:
    try:
        with Image.open(path) as image:
            result = np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0
    except OSError as exc:
        raise ImageComparisonError(f"cannot read image {path}: {exc}") from exc
    if result.ndim != 3 or result.shape[2] != 3:
        raise ImageComparisonError(f"image {path} is not an RGB image")
    return result


def _normalized_rect(rect: Any, width: int, height: int) -> tuple[int, int, int, int]:
    if not isinstance(rect, list) or len(rect) != 4:
        raise ImageComparisonError("a mask rect must contain [left, top, right, bottom]")
    try:
        left, top, right, bottom = (float(value) for value in rect)
    except (TypeError, ValueError) as exc:
        raise ImageComparisonError("mask rect coordinates must be numeric") from exc
    if not 0.0 <= left < right <= 1.0 or not 0.0 <= top < bottom <= 1.0:
        raise ImageComparisonError("mask rect coordinates must be normalized to 0..1")
    return (
        max(0, int(np.floor(left * width))),
        max(0, int(np.floor(top * height))),
        min(width, int(np.ceil(right * width))),
        min(height, int(np.ceil(bottom * height))),
    )


def _mask_from_data(data: Any, width: int, height: int) -> np.ndarray:
    if not isinstance(data, dict):
        raise ImageComparisonError("a JSON mask must be an object")

    if "rect" in data:
        left, top, right, bottom = _normalized_rect(data["rect"], width, height)
        mask = np.zeros((height, width), dtype=bool)
        mask[top:bottom, left:right] = True
        return mask

    if "polygons" in data:
        polygons = data["polygons"]
        if not isinstance(polygons, list) or not polygons:
            raise ImageComparisonError("mask polygons must be a non-empty list")
        mask_image = Image.new("1", (width, height), 0)
        draw = ImageDraw.Draw(mask_image)
        for polygon in polygons:
            if not isinstance(polygon, list) or len(polygon) < 3:
                raise ImageComparisonError("each mask polygon needs at least three points")
            points = []
            for point in polygon:
                if not isinstance(point, list) or len(point) != 2:
                    raise ImageComparisonError("mask polygon points must contain [x, y]")
                x, y = (float(value) for value in point)
                if not 0.0 <= x <= 1.0 or not 0.0 <= y <= 1.0:
                    raise ImageComparisonError("mask polygon coordinates must be normalized to 0..1")
                points.append((x * width, y * height))
            draw.polygon(points, fill=1)
        return np.asarray(mask_image, dtype=bool)

    if "pixels" in data:
        pixels = data["pixels"]
        if not isinstance(pixels, list) or len(pixels) != height:
            raise ImageComparisonError("pixel mask height does not match the image")
        rows = []
        for row in pixels:
            if isinstance(row, str):
                values = [character not in {"0", ".", " "} for character in row]
            elif isinstance(row, list):
                values = [bool(value) for value in row]
            else:
                raise ImageComparisonError("pixel mask rows must be strings or lists")
            if len(values) != width:
                raise ImageComparisonError("pixel mask width does not match the image")
            rows.append(values)
        return np.asarray(rows, dtype=bool)

    raise ImageComparisonError("mask JSON must define rect, polygons, or pixels")


def _load_mask(mask_spec: Any, manifest_dir: Path, width: int, height: int) -> np.ndarray:
    if isinstance(mask_spec, dict):
        return _mask_from_data(mask_spec, width, height)
    if not isinstance(mask_spec, str) or not mask_spec:
        raise ImageComparisonError("region mask must be a path or an inline object")

    path = (manifest_dir / mask_spec).resolve()
    if path.suffix.lower() == ".json":
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise ImageComparisonError(f"cannot read mask {path}: {exc}") from exc
        return _mask_from_data(data, width, height)

    try:
        with Image.open(path) as image:
            mask = np.asarray(image.convert("L"), dtype=np.uint8) > 0
    except OSError as exc:
        raise ImageComparisonError(f"cannot read mask {path}: {exc}") from exc
    if mask.shape != (height, width):
        raise ImageComparisonError(f"mask {path} has shape {mask.shape}, expected {(height, width)}")
    return mask


def _load_regions(path: Path, width: int, height: int) -> list[tuple[str, np.ndarray, float, float]]:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ImageComparisonError(f"cannot read region manifest {path}: {exc}") from exc
    if not isinstance(manifest, dict):
        raise ImageComparisonError("region manifest must be an object")

    defaults = manifest.get("defaults", {})
    if not isinstance(defaults, dict):
        raise ImageComparisonError("region manifest defaults must be an object")
    default_ssim = float(defaults.get("ssim_min", 0.98))
    default_delta = float(defaults.get("delta_e_max", 2.0))
    raw_regions = manifest.get("regions")
    if isinstance(raw_regions, dict):
        raw_regions = [{"name": name, **value} for name, value in raw_regions.items()]
    if not isinstance(raw_regions, list) or not raw_regions:
        raise ImageComparisonError("region manifest must contain a non-empty regions list")

    regions = []
    names = set()
    for entry in raw_regions:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            raise ImageComparisonError("each region needs a string name")
        name = entry["name"]
        if name in names:
            raise ImageComparisonError(f"duplicate region name: {name}")
        names.add(name)
        mask = _load_mask(entry.get("mask"), path.parent, width, height)
        if not np.any(mask):
            raise ImageComparisonError(f"region {name} has an empty mask")
        regions.append(
            (
                name,
                mask,
                float(entry.get("ssim_min", default_ssim)),
                float(entry.get("delta_e_max", default_delta)),
            )
        )
    return regions


def _ssim(reference: np.ndarray, candidate: np.ndarray) -> float:
    reference_luma = reference @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    candidate_luma = candidate @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    reference_mean = float(np.mean(reference_luma))
    candidate_mean = float(np.mean(candidate_luma))
    reference_variance = float(np.mean((reference_luma - reference_mean) ** 2))
    candidate_variance = float(np.mean((candidate_luma - candidate_mean) ** 2))
    covariance = float(np.mean((reference_luma - reference_mean) * (candidate_luma - candidate_mean)))
    c1 = 0.01**2
    c2 = 0.03**2
    score = ((2.0 * reference_mean * candidate_mean + c1) * (2.0 * covariance + c2)) / (
        (reference_mean**2 + candidate_mean**2 + c1)
        * (reference_variance + candidate_variance + c2)
    )
    return float(np.clip(score, -1.0, 1.0))


def _rgb_to_lab(rgb: np.ndarray) -> np.ndarray:
    linear = np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)
    xyz = linear @ np.array(
        [
            [0.4124564, 0.3575761, 0.1804375],
            [0.2126729, 0.7151522, 0.0721750],
            [0.0193339, 0.1191920, 0.9503041],
        ],
        dtype=np.float32,
    ).T
    xyz /= np.array([0.95047, 1.0, 1.08883], dtype=np.float32)
    epsilon = 216.0 / 24389.0
    kappa = 24389.0 / 27.0
    f = np.where(xyz > epsilon, np.cbrt(xyz), (kappa * xyz + 16.0) / 116.0)
    return np.stack(
        (116.0 * f[..., 1] - 16.0, 500.0 * (f[..., 0] - f[..., 1]), 200.0 * (f[..., 1] - f[..., 2])),
        axis=-1,
    )


def _delta_e(reference: np.ndarray, candidate: np.ndarray) -> float:
    difference = _rgb_to_lab(reference) - _rgb_to_lab(candidate)
    return float(np.mean(np.sqrt(np.sum(difference**2, axis=1))))


def compare_images(reference_path: Path, candidate_path: Path, regions_path: Path) -> ComparisonReport:
    """Compare two images and return one result for every configured region."""
    reference = _load_rgb(reference_path)
    candidate = _load_rgb(candidate_path)
    if reference.shape != candidate.shape:
        raise ImageComparisonError(
            f"image shapes differ: reference {reference.shape[:2]}, candidate {candidate.shape[:2]}"
        )
    height, width = reference.shape[:2]
    region_specs = _load_regions(regions_path, width, height)
    results = []
    for name, mask, ssim_min, delta_e_max in region_specs:
        reference_region = reference[mask]
        candidate_region = candidate[mask]
        ssim = _ssim(reference_region, candidate_region)
        delta_e = _delta_e(reference_region, candidate_region)
        results.append(
            RegionResult(
                name=name,
                pixels=int(np.count_nonzero(mask)),
                ssim=ssim,
                ssim_min=ssim_min,
                delta_e=delta_e,
                delta_e_max=delta_e_max,
                passed=ssim >= ssim_min and delta_e <= delta_e_max,
            )
        )
    return ComparisonReport(
        reference=str(reference_path),
        candidate=str(candidate_path),
        passed=all(result.passed for result in results),
        regions=tuple(results),
    )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path, help="reference capture")
    parser.add_argument("candidate", type=Path, help="candidate capture")
    parser.add_argument("--regions", required=True, type=Path, help="JSON region manifest")
    parser.add_argument("--json", action="store_true", help="emit the report as JSON")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        report = compare_images(args.reference, args.candidate, args.regions)
    except (ImageComparisonError, OSError, ValueError) as exc:
        print(f"FAIL image comparison: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(report.as_dict(), indent=2, sort_keys=True))
    else:
        for region in report.regions:
            status = "PASS" if region.passed else "FAIL"
            print(
                f"{status} {region.name}: pixels={region.pixels} "
                f"ssim={region.ssim:.6f} (min {region.ssim_min:.6f}) "
                f"delta_e={region.delta_e:.4f} (max {region.delta_e_max:.4f})"
            )
        print(f"{'PASS' if report.passed else 'FAIL'} image comparison: {len(report.regions)} regions")
    return 0 if report.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
