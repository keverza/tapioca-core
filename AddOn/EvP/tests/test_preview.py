"""evp.preview — the fragment the palette's preview band shows, and its budget.

Kept OUTSIDE PyPackage so it never ships with the add-on. Run:
    python -m pytest AddOn/EvP/tests/test_preview.py

The budget tests assert the MESSAGE, not just the exception. A cap that raises
without naming itself sends the author looking in the wrong place, and the whole
reason the cap is a refusal rather than a truncation is that the author has to be
told what to do instead.
"""

import os
import sys

import pytest

_PACKAGE = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage")
if _PACKAGE not in sys.path:
    sys.path.insert(0, _PACKAGE)

from evp import preview as preview_module  # noqa: E402
from evp.preview import PreviewBudgetError, PreviewScene  # noqa: E402

# A unit tetrahedron: four corners, four faces, every vertex shared.
TETRA_VERTICES = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
TETRA_TRIANGLES = [(0, 2, 1), (0, 1, 3), (0, 3, 2), (1, 2, 3)]


def _big_mesh(triangles):
    """A fan with `triangles` faces, for the budget tests."""
    vertices = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0)]
    faces = []
    for index in range(triangles):
        vertices.append((1.0, float(index + 1), 0.0))
        faces.append((0, len(vertices) - 2, len(vertices) - 1))
    return vertices, faces


# ---------------------------------------------------------------------------
# Building
# ---------------------------------------------------------------------------

def test_rows_flat_and_numpy_like_all_arrive_as_the_same_flat_list():
    class FakeArray:  # numpy's duck type, without importing numpy
        def tolist(self):
            return [[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]]

    from_rows = PreviewScene().mesh(TETRA_VERTICES, TETRA_TRIANGLES)
    from_flat = PreviewScene().mesh([c for v in TETRA_VERTICES for c in v],
                                    [i for t in TETRA_TRIANGLES for i in t])
    from_array = PreviewScene().mesh(FakeArray(), TETRA_TRIANGLES)

    assert (from_rows.meshes[0]["vertices"] == from_flat.meshes[0]["vertices"]
            == from_array.meshes[0]["vertices"])


def test_normals_are_computed_when_none_are_given_and_are_unit_length():
    scene = PreviewScene().mesh(TETRA_VERTICES, TETRA_TRIANGLES)
    normals = scene.meshes[0]["normals"]

    assert len(normals) == len(scene.meshes[0]["vertices"])
    for start in range(0, len(normals), 3):
        length = sum(normals[start + axis] ** 2 for axis in range(3)) ** 0.5
        assert length == pytest.approx(1.0, abs=1e-9)


def test_an_index_past_the_last_vertex_is_caught_here():
    # Out of range reads as scrambled geometry on screen, never as an error, so
    # it has to be refused at the point it is written.
    with pytest.raises(ValueError) as caught:
        PreviewScene().mesh(TETRA_VERTICES, [(0, 1, 9)])
    assert "9" in str(caught.value)


def test_an_empty_mesh_is_a_planner_bug_and_says_so():
    with pytest.raises(ValueError) as caught:
        PreviewScene().mesh([], [])
    assert "note()" in str(caught.value)


def test_an_unknown_role_names_the_ones_that_exist():
    with pytest.raises(ValueError) as caught:
        PreviewScene().mesh(TETRA_VERTICES, TETRA_TRIANGLES, role="green")
    message = str(caught.value)
    assert "green" in message
    assert "add" in message and "guide" in message


def test_a_one_point_polyline_is_refused():
    with pytest.raises(ValueError):
        PreviewScene().lines([(0, 0, 0)])


def test_bounds_cover_meshes_and_polylines_together():
    scene = PreviewScene()
    scene.mesh(TETRA_VERTICES, TETRA_TRIANGLES)
    scene.lines([(0, 0, -5), (0, 0, 9)])

    low, high = scene.bounds()
    assert low == (0.0, 0.0, -5.0)
    assert high == (1.0, 1.0, 9.0)


def test_an_empty_scene_has_no_bounds_rather_than_a_zero_box():
    # A zero box would frame the camera on the origin and look like a broken
    # render; None lets the band say it has nothing to show.
    assert PreviewScene().bounds() is None
    assert PreviewScene().is_empty


# ---------------------------------------------------------------------------
# The budget
# ---------------------------------------------------------------------------

def test_over_the_triangle_cap_refuses_and_names_the_cap():
    vertices, faces = _big_mesh(preview_module.MAX_TRIANGLES + 1)

    with pytest.raises(PreviewBudgetError) as caught:
        PreviewScene().mesh(vertices, faces)

    message = str(caught.value)
    assert str(preview_module.MAX_TRIANGLES) in message
    assert "triangles" in message
    assert "note()" in message, "the refusal must say what to do instead"


def test_the_budget_is_cumulative_across_calls():
    # One mesh under the cap plus another under the cap can still be over it —
    # a per-call check would let the band be flooded a slice at a time.
    scene = PreviewScene()
    half = preview_module.MAX_TRIANGLES // 2 + 1
    vertices, faces = _big_mesh(half)
    scene.mesh(vertices, faces)
    with pytest.raises(PreviewBudgetError):
        scene.mesh(vertices, faces)


def test_the_mesh_count_is_capped_independently_of_the_triangle_count():
    scene = PreviewScene()
    for _ in range(preview_module.MAX_MESHES):
        scene.mesh(TETRA_VERTICES, TETRA_TRIANGLES)

    with pytest.raises(PreviewBudgetError) as caught:
        scene.mesh(TETRA_VERTICES, TETRA_TRIANGLES)
    assert "meshes" in str(caught.value)


def test_nothing_is_recorded_when_a_call_is_refused():
    # A refused mesh that had already been counted would make the NEXT, legal
    # call fail too — and that second failure would name the wrong cause.
    scene = PreviewScene()
    vertices, faces = _big_mesh(preview_module.MAX_TRIANGLES + 1)
    with pytest.raises(PreviewBudgetError):
        scene.mesh(vertices, faces)

    scene.mesh(TETRA_VERTICES, TETRA_TRIANGLES)
    assert scene.triangle_count == 4


# ---------------------------------------------------------------------------
# The wire shape
# ---------------------------------------------------------------------------

def test_meshes_and_lines_cross_as_json_strings():
    # ⚠️ A bare nested array does not survive ObjectState on the C++ side. This
    # is the same shape evp.ui.table already uses for its rows, and the test
    # exists so a "tidier" payload cannot be introduced by accident.
    import json

    scene = PreviewScene("plan2d")
    scene.mesh(TETRA_VERTICES, TETRA_TRIANGLES, role="add", label="one of twelve")
    scene.lines([(0, 0, 0), (1, 1, 0)], role="guide")
    scene.note("12 planned, 1 shown")

    payload = scene.to_payload()

    assert payload["kind"] == "plan2d"
    assert all(isinstance(item, str) for item in payload["meshes"])
    assert all(isinstance(item, str) for item in payload["lines"])
    assert payload["notes"] == ["12 planned, 1 shown"]
    assert json.loads(payload["meshes"][0])["label"] == "one of twelve"
    assert payload["boundsMin"] == [0.0, 0.0, 0.0]


def test_an_empty_scene_sends_no_bounds_key_at_all():
    assert "boundsMin" not in PreviewScene().to_payload()
