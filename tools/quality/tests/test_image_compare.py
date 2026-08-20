"""Offline and negative tests for the reference-image comparison gate."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

from tools.quality import image_compare

FIXTURE_MANIFEST = Path(__file__).parents[1] / "fixtures" / "image_compare" / "regions.json"


def _write_image(path: Path, pixels: np.ndarray) -> None:
    Image.fromarray(pixels, mode="RGB").save(path)


def test_identical_capture_passes_each_region(tmp_path: Path) -> None:
    pixels = np.full((16, 16, 3), 96, dtype=np.uint8)
    reference = tmp_path / "reference.png"
    candidate = tmp_path / "candidate.png"
    _write_image(reference, pixels)
    _write_image(candidate, pixels)

    report = image_compare.compare_images(reference, candidate, FIXTURE_MANIFEST)

    assert report.passed
    assert len(report.regions) == 6
    assert all(region.ssim == 1.0 and region.delta_e == 0.0 for region in report.regions)


def test_regression_is_failed_in_only_the_perturbed_region(tmp_path: Path) -> None:
    reference_pixels = np.full((16, 16, 3), 96, dtype=np.uint8)
    candidate_pixels = reference_pixels.copy()
    candidate_pixels[:, 12:16] = (255, 0, 0)
    reference = tmp_path / "reference.png"
    candidate = tmp_path / "candidate.png"
    _write_image(reference, reference_pixels)
    _write_image(candidate, candidate_pixels)

    report = image_compare.compare_images(reference, candidate, FIXTURE_MANIFEST)

    assert not report.passed
    assert [region.name for region in report.regions if not region.passed] == ["interior_contact"]


def test_cli_returns_nonzero_for_a_regressed_capture(tmp_path: Path) -> None:
    pixels = np.full((16, 16, 3), 96, dtype=np.uint8)
    candidate_pixels = pixels.copy()
    candidate_pixels[:, 12:16] = (255, 0, 0)
    reference = tmp_path / "reference.png"
    candidate = tmp_path / "candidate.png"
    _write_image(reference, pixels)
    _write_image(candidate, candidate_pixels)

    result = subprocess.run(
        [
            sys.executable,
            str(Path(image_compare.__file__).resolve()),
            str(reference),
            str(candidate),
            "--regions",
            str(FIXTURE_MANIFEST),
        ],
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode != 0
    assert "FAIL interior_contact" in result.stdout


def test_png_mask_must_match_capture_dimensions(tmp_path: Path) -> None:
    pixels = np.full((8, 8, 3), 96, dtype=np.uint8)
    reference = tmp_path / "reference.png"
    candidate = tmp_path / "candidate.png"
    _write_image(reference, pixels)
    _write_image(candidate, pixels)
    Image.fromarray(np.ones((4, 4), dtype=np.uint8) * 255, mode="L").save(tmp_path / "mask.png")
    manifest = tmp_path / "regions.json"
    manifest.write_text(
        '{"regions": [{"name": "all", "mask": "mask.png"}]}',
        encoding="utf-8",
    )

    try:
        image_compare.compare_images(reference, candidate, manifest)
    except image_compare.ImageComparisonError as exc:
        assert "expected (8, 8)" in str(exc)
    else:
        raise AssertionError("a mismatched mask must fail the comparison")
