"""evp.plan — the intended-writes record that sits between compute and commit.

Kept OUTSIDE PyPackage so it never ships with the add-on. Run:
    python -m pytest AddOn/EvP/tests/test_plan.py

`commit()` is exercised against the REAL evp.transaction, with only
`evp.api._transport` faked — the same seam AddOn/EvP/tests/dryrun_command.py
uses. So step recording, Handle/Ref binding extraction and the single-batch
replay are genuine here; only the wire is not.
"""

import os
import sys

import pytest

_PACKAGE = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage")
if _PACKAGE not in sys.path:
    sys.path.insert(0, _PACKAGE)

from evp import plan as plan_module  # noqa: E402
from evp.plan import ElementSpec, FromStep, Plan, PlanError  # noqa: E402


def _spec(key, angle=10.0, command="Tapioca.PlaceObject"):
    return ElementSpec(
        key=key,
        command=command,
        params={"angle": angle},
        label="Slope symbol on %s" % key,
        detail={"angle": angle},
    )


# --------------------------------------------------------------------------
# Identity and refusals
# --------------------------------------------------------------------------

def test_duplicate_keys_are_refused():
    with pytest.raises(PlanError, match="share the key"):
        Plan("Place", [_spec("roof-1"), _spec("roof-1")])


def test_a_spec_without_a_key_is_refused():
    with pytest.raises(PlanError, match="needs a key"):
        ElementSpec(key="", command="Tapioca.PlaceObject")


def test_an_unnamed_plan_is_refused():
    # The name becomes the undo step; "Undo" with nothing after it says nothing.
    with pytest.raises(PlanError, match="needs a name"):
        Plan("", [_spec("roof-1")])


def test_an_unknown_op_is_refused():
    with pytest.raises(PlanError, match="create, modify or delete"):
        ElementSpec(key="a", command="X", op="upsert")


def test_committing_an_empty_plan_is_refused():
    with pytest.raises(PlanError, match="empty plan"):
        Plan("Place", []).commit()


# --------------------------------------------------------------------------
# Diff — the preview's whole content
# --------------------------------------------------------------------------

def test_first_run_is_all_additions():
    current = Plan("Place", [_spec("roof-1"), _spec("roof-2")])
    diff = current.diff(None)
    assert len(diff.added) == 2
    assert not diff.changed and not diff.removed
    assert not diff.is_empty


def test_diff_separates_added_changed_removed_and_unchanged():
    before = Plan("Place", [_spec("roof-1", 10.0), _spec("roof-2", 20.0),
                            _spec("roof-3", 30.0)])
    after = Plan("Place", [_spec("roof-1", 10.0),      # unchanged
                           _spec("roof-2", 25.0),      # changed
                           _spec("roof-4", 40.0)])     # added; roof-3 removed

    diff = after.diff(before)
    assert [s.key for s in diff.added] == ["roof-4"]
    assert [s.key for s in diff.removed] == ["roof-3"]
    assert [(o.key, n.key) for o, n in diff.changed] == [("roof-2", "roof-2")]
    assert [s.key for s in diff.unchanged] == ["roof-1"]
    assert diff.summary() == "1 added, 1 changed, 1 removed, 1 unchanged"


def test_rerunning_an_unchanged_plan_diffs_to_nothing():
    before = Plan("Place", [_spec("roof-1"), _spec("roof-2")])
    after = Plan("Place", [_spec("roof-1"), _spec("roof-2")])
    assert after.diff(before).is_empty


def test_change_rows_name_only_the_field_that_moved():
    before = Plan("Place", [_spec("roof-1", 10.0)])
    after = Plan("Place", [_spec("roof-1", 25.0)])
    headers, rows, colors = after.diff(before).rows()

    assert headers == ["Change", "Element", "Detail"]
    assert rows[0][0] == "Change"
    assert rows[0][2] == "angle: 10.0 -> 25.0"
    # Colours go out as ui.TABLE_COLORS names; ui.table resolves them.
    assert colors[0] == "amber"


def test_row_counts_and_colors_stay_in_step():
    before = Plan("Place", [_spec("roof-1", 10.0), _spec("roof-3")])
    after = Plan("Place", [_spec("roof-1", 99.0), _spec("roof-2"), _spec("roof-4")])
    _, rows, colors = after.diff(before).rows()
    # One colour per row, or ui.table pairs the wrong colour with the wrong row.
    assert len(rows) == len(colors)


# --------------------------------------------------------------------------
# Commit — one transaction, real Handle/Ref machinery
# --------------------------------------------------------------------------

@pytest.fixture
def bus(monkeypatch):
    """Fake ONLY the transport, exactly as dryrun_command.py does.

    `_transport` is a FACTORY returning the sender, and the sender takes a JSON
    string and returns a JSON string — everything above it (Transaction, Handle,
    Ref, binding extraction, replay) is the real code.
    """
    import json

    from evp import api

    sent = []

    def send(command, params_json):
        params = json.loads(params_json)
        sent.append((command, params))
        steps = params.get("steps") or []
        # The envelope is ok/data/error/meta, and a transaction's `results` come
        # back as an array of JSON STRINGS — the same shape the steps go out in,
        # because arrays of scalars are the proven wire shape here.
        return json.dumps({
            "ok": True,
            "data": {"results": [json.dumps({"guid": "G%d" % i})
                                 for i in range(len(steps))]},
            "meta": {"backend": "test"},
        })

    monkeypatch.setattr(api, "_transport", lambda: send, raising=False)
    return sent


def test_commit_sends_one_batch(bus):
    results = Plan("Place slope symbols",
                   [_spec("roof-1"), _spec("roof-2")]).commit()
    # One bus call, not one per element: the whole point of a transaction is a
    # single gate batch and a single undo step.
    assert len(bus) == 1
    assert len(results) == 2


def test_commit_uses_the_plan_name_as_the_undo_step(bus):
    Plan("Place slope symbols", [_spec("roof-1")]).commit()
    _, params = bus[0]
    assert "Place slope symbols" in str(params)


def test_from_step_resolves_to_a_real_ref(bus):
    plan = Plan("Place + tag", [
        _spec("sym-1"),
        ElementSpec(key="tag-1", command="Tapioca.SetElementDetails",
                    op="modify",
                    params={"element": FromStep("sym-1", "guid")}),
    ])
    plan.commit()

    _, params = bus[0]
    steps = params["steps"]
    # The Ref does not travel inside params — the transaction lifts it into a
    # bindings array of dot-paths, which is what the replay resolves.
    assert len(steps) == 2
    assert "bindings" in str(steps[1]) or "bindings" in str(params)


def test_from_step_naming_a_later_spec_is_refused(bus):
    plan = Plan("Bad order", [
        ElementSpec(key="tag-1", command="Tapioca.SetElementDetails",
                    params={"element": FromStep("sym-1", "guid")}),
        _spec("sym-1"),
    ])
    with pytest.raises(PlanError, match="does not run before it"):
        plan.commit()


def test_from_step_inside_a_list_is_refused(bus):
    plan = Plan("Bad shape", [
        _spec("sym-1"),
        ElementSpec(key="tag-1", command="Tapioca.SetElementDetails",
                    params={"elements": [FromStep("sym-1", "guid")]}),
    ])
    # The binding protocol addresses object fields, not array indices, so there
    # is no path this could resolve through.
    with pytest.raises(PlanError, match="inside a list"):
        plan.commit()


def test_plan_rows_render_without_a_comparison():
    headers, rows = Plan("Place", [_spec("roof-1"), _spec("roof-2")]).rows()
    assert headers == ["Action", "Element", "Detail"]
    assert [r[0] for r in rows] == ["Create", "Create"]
    assert rows[0][1] == "Slope symbol on roof-1"


def test_notes_survive_on_the_plan():
    plan = Plan("Place", [_spec("roof-1")],
                notes=["roof-3 skipped: slantAngle is 0 on a poly roof"])
    # A plan that silently drops an element is worse than one that says it did.
    assert plan.notes == ("roof-3 skipped: slantAngle is 0 on a poly roof",)


def test_module_exports_what_a_command_needs():
    assert set(plan_module.__all__) == {
        "ElementSpec", "Plan", "PlanDiff", "PlanError", "FromStep"}


# --------------------------------------------------------------------------
# Text rendering — ui.table is unusable, so text is THE path
# --------------------------------------------------------------------------

def test_diff_text_names_only_what_moved_and_pads_nothing():
    before = Plan("Place", [_spec("roof-1", 10.0), _spec("roof-2", 20.0)])
    after = Plan("Place", [_spec("roof-1", 10.0), _spec("roof-2", 25.0),
                           _spec("roof-3", 30.0)])

    body = after.diff(before).as_text()
    assert "Add: Slope symbol on roof-3" in body
    assert "Change: Slope symbol on roof-2 (angle: 20.0 -> 25.0)" in body
    # Unchanged is off by default: burying three real changes under forty
    # "no change" lines is how a diff stops being read.
    assert "roof-1" not in body
    # ⚠️ NO COLUMN PADDING. The panel is a proportional font, so padded columns
    # and dot leaders read ragged — both were tried live and rejected.
    for line in body.splitlines():
        assert "  " not in line, "padded columns read ragged in a proportional font"


def test_diff_text_says_so_when_nothing_changed():
    plan = Plan("Place", [_spec("roof-1")])
    assert "No change" in plan.diff(plan).as_text()


def test_diff_text_can_include_unchanged_on_request():
    before = Plan("Place", [_spec("roof-1")])
    after = Plan("Place", [_spec("roof-1"), _spec("roof-2")])
    body = after.diff(before).as_text(unchanged=True)
    assert "Same: Slope symbol on roof-1" in body


def test_plan_text_lists_notes():
    plan = Plan("Place", [_spec("roof-1")],
                notes=["roof-3 skipped: slantAngle is 0 on a poly roof"])
    assert "Note: roof-3 skipped" in plan.as_text()


# --------------------------------------------------------------------------
# Persistence — what makes a diff possible at all
# --------------------------------------------------------------------------

def test_a_plan_survives_a_json_round_trip():
    original = Plan("Place", [_spec("roof-1", 10.0), _spec("roof-2", 20.0)],
                    notes=["one note"])
    restored = Plan.from_json(original.to_json())

    assert restored.name == original.name
    assert restored.notes == original.notes
    # Equality is what the diff depends on: a round trip that changed anything
    # would make every re-run report every element as changed.
    assert restored.diff(original).is_empty


def test_a_from_step_survives_a_json_round_trip():
    original = Plan("Place + tag", [
        _spec("sym-1"),
        ElementSpec(key="tag-1", command="Tapioca.SetElementDetails",
                    params={"element": FromStep("sym-1", "guid")}),
    ])
    restored = Plan.from_json(original.to_json())
    reference = restored.by_key()["tag-1"].params["element"]

    # A FromStep is the only non-JSON value params hold; without an encoding it
    # would round-trip as a dict and stop resolving.
    assert isinstance(reference, FromStep)
    assert (reference.step_key, reference.path) == ("sym-1", "guid")
    assert restored.diff(original).is_empty
