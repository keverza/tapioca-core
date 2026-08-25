"""evp._invoke — the one place that decides HOW a command's run() is called.

Kept OUTSIDE PyPackage so it never ships with the add-on. Run:
    python -m pytest AddOn/EvP/tests/test_invoke.py

Three runners reach a command (the embedded interpreter, the external
subprocess, the offline harness) and all three go through invoke(). What is
pinned here is that the two calling conventions stay distinct and that the
signature form — every existing command — is untouched.
"""

import os
import sys

import pytest

pytest.importorskip("pydantic", reason="pydantic ships in the Tapioca runtime baseline")

_PACKAGE = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
if _PACKAGE not in sys.path:
    sys.path.insert(0, _PACKAGE)

import evp  # noqa: E402
from evp import _invoke  # noqa: E402
from evp.schema import Field, NodeModel  # noqa: E402


class Inputs(NodeModel):
    count: int = Field(default=2, ge=1, le=10)
    label: str = "x"


class Outputs(NodeModel):
    total: int = Field(default=0, ge=0)


def _schema_command(**extra):
    @evp.command(title="Schema Cmd", inputs=Inputs, outputs=Outputs, **extra)
    def run(ctx, inputs):
        return Outputs(total=inputs.count)

    return run


# --------------------------------------------------------------------------
# The signature form must not change
# --------------------------------------------------------------------------

def test_a_signature_command_is_called_exactly_as_before():
    seen = {}

    @evp.command(title="Legacy")
    def run(count: evp.Int(minimum=1) = 4, dry_run: bool = True):
        seen.update(count=count, dry_run=dry_run)
        return "done"

    assert _invoke.invoke(run, {"count": 7, "dry_run": False}) == "done"
    assert seen == {"count": 7, "dry_run": False}


def test_a_signature_command_gets_no_ctx():
    # Adding one would change 60+ existing signatures at once.
    @evp.command(title="Legacy")
    def run(count: int = 1):
        return count

    assert _invoke.invoke(run, {"count": 3}) == 3


def test_an_armed_signature_command_persists_and_publishes_its_watch_trace(
        monkeypatch, tmp_path):
    monkeypatch.setenv("EVP_HOME", str(tmp_path))
    folder = str(tmp_path / "Watched")
    sent = []
    monkeypatch.setattr(evp.api, "call", lambda command, payload, **kwargs:
                        sent.append((command, payload, kwargs)))

    @evp.command(title="Watched")
    def run(value: int = 1):
        evp.watch("value", (value, 0, 0))
        return value

    assert _invoke.invoke(run, {"value": 7}, folder=folder, watch_armed=True) == 7

    from evp import _watchstore
    import json

    stored = _watchstore.load(folder)
    node = json.loads(stored["nodes"][0])
    frame = json.loads(node["frames"][0])
    assert frame["primitives"][0]["points"] == [7.0, 0.0, 0.0]
    assert sent == [("Tapioca.SetWatchTrace", stored, {"raise_on_error": False})]


def test_evp_watch_environment_default_arms_capture(monkeypatch, tmp_path):
    monkeypatch.setenv("EVP_HOME", str(tmp_path))
    monkeypatch.setenv("EVP_WATCH", "yes")
    monkeypatch.setattr(evp.api, "call", lambda *args, **kwargs: None)

    @evp.command(title="Environment Watched")
    def run():
        evp.watch.point((1, 2, 3))

    folder = str(tmp_path / "EnvironmentWatched")
    _invoke.invoke(run, {}, folder=folder)

    from evp import _watchstore
    assert _watchstore.load(folder)["version"] == 1


def test_an_unarmed_invocation_does_not_store_or_publish(monkeypatch, tmp_path):
    monkeypatch.setenv("EVP_HOME", str(tmp_path))
    monkeypatch.setenv("EVP_WATCH", "0")
    monkeypatch.setattr(evp.api, "call", lambda *args, **kwargs:
                        pytest.fail("an unarmed invocation must not publish"))

    @evp.command(title="Unwatched")
    def run():
        # This invalid value proves unarmed watch calls skip normalization too.
        evp.watch.point(object())
        return "ok"

    folder = str(tmp_path / "Unwatched")
    assert _invoke.invoke(run, {}, folder=folder) == "ok"

    from evp import _watchstore
    assert _watchstore.load(folder) is None


def test_a_failed_armed_invocation_preserves_the_prior_trace(monkeypatch, tmp_path):
    monkeypatch.setenv("EVP_HOME", str(tmp_path))
    monkeypatch.setattr(evp.api, "call", lambda *args, **kwargs: None)
    folder = str(tmp_path / "Watched")

    @evp.command(title="Watched")
    def succeeds():
        evp.watch.point((1, 0, 0), name="prior")

    @evp.command(title="Watched")
    def fails():
        evp.watch.point((2, 0, 0), name="replacement")
        raise RuntimeError("boom")

    _invoke.invoke(succeeds, {}, folder=folder, watch_armed=True)
    from evp import _watchstore
    prior = _watchstore.load(folder)

    with pytest.raises(RuntimeError, match="boom"):
        _invoke.invoke(fails, {}, folder=folder, watch_armed=True)
    assert _watchstore.load(folder) == prior


def test_watch_store_and_transport_errors_never_fail_a_command(monkeypatch, tmp_path):
    monkeypatch.setenv("EVP_HOME", str(tmp_path))
    from evp import _watchstore
    monkeypatch.setattr(_watchstore, "save", lambda *args, **kwargs:
                        (_ for _ in ()).throw(OSError("disk")))
    monkeypatch.setattr(evp.api, "call", lambda *args, **kwargs:
                        (_ for _ in ()).throw(RuntimeError("transport")))

    @evp.command(title="Resilient")
    def run():
        evp.watch.point((0, 0, 0))
        return "completed"

    assert _invoke.invoke(run, {}, folder=str(tmp_path / "Resilient"),
                          watch_armed=True) == "completed"


# --------------------------------------------------------------------------
# The schema form
# --------------------------------------------------------------------------

def test_a_schema_command_receives_ctx_and_a_validated_model():
    captured = {}

    @evp.command(title="Schema Cmd", inputs=Inputs, outputs=Outputs)
    def run(ctx, inputs):
        captured["ctx"] = ctx
        captured["inputs"] = inputs
        return Outputs(total=inputs.count)

    result = _invoke.invoke(run, {"count": 5}, folder="SchemaCmd")
    assert result.total == 5
    assert isinstance(captured["inputs"], Inputs)
    assert captured["inputs"].label == "x"          # default applied
    assert captured["ctx"].name == "Schema Cmd"
    assert captured["ctx"].mode == "run"
    assert captured["ctx"].is_preview is False


def test_a_value_outside_its_bound_names_the_field():
    run = _schema_command()
    with pytest.raises(_invoke.InvokeError) as excinfo:
        _invoke.invoke(run, {"count": 0})
    message = str(excinfo.value)
    assert "Inputs" in message
    assert "count" in message


def test_an_unknown_port_is_refused():
    # extra="forbid" turns a stale or misspelled port name into an error at the
    # boundary instead of a control that silently does nothing.
    run = _schema_command()
    with pytest.raises(_invoke.InvokeError, match="couunt|Inputs"):
        _invoke.invoke(run, {"couunt": 2})


def test_the_return_value_is_validated_against_outputs():
    @evp.command(title="Bad Output", inputs=Inputs, outputs=Outputs)
    def run(ctx, inputs):
        return {"total": -1}                     # ge=0 refuses this

    with pytest.raises(_invoke.InvokeError, match="declared Outputs"):
        _invoke.invoke(run, {})


def test_a_plain_dict_return_is_coerced_into_outputs():
    @evp.command(title="Dict Output", inputs=Inputs, outputs=Outputs)
    def run(ctx, inputs):
        return {"total": 4}

    result = _invoke.invoke(run, {})
    assert isinstance(result, Outputs)
    assert result.total == 4


def test_inputs_are_frozen():
    @evp.command(title="Mutating", inputs=Inputs, outputs=Outputs)
    def run(ctx, inputs):
        inputs.count = 99                        # frozen=True refuses this
        return Outputs(total=0)

    # A command that edited its inputs would preview one thing and commit
    # another, since the same values are sent for both.
    from pydantic import ValidationError

    with pytest.raises(ValidationError):
        _invoke.invoke(run, {})


# --------------------------------------------------------------------------
# Preview
# --------------------------------------------------------------------------

def test_build_plan_runs_the_planner_not_the_command():
    ran = []

    def planner(inputs, ctx):
        return evp.Plan("P", [evp.ElementSpec(key="a", command="X")])

    @evp.command(title="Previewable", inputs=Inputs, outputs=Outputs,
                 plan=planner, needs_preview=True)
    def run(ctx, inputs):
        ran.append(True)
        return Outputs(total=0)

    plan = _invoke.build_plan(run, {"count": 3}, folder="Previewable")
    assert len(plan) == 1
    assert ran == [], "a preview must not run the command"


def test_the_planner_sees_preview_mode():
    seen = {}

    def planner(inputs, ctx):
        seen["mode"] = ctx.mode
        return evp.Plan("P", [evp.ElementSpec(key="a", command="X")])

    @evp.command(title="Previewable", inputs=Inputs, plan=planner)
    def run(ctx, inputs):
        return None

    _invoke.build_plan(run, {})
    assert seen["mode"] == "preview"


def test_a_command_without_plan_cannot_be_previewed():
    run = _schema_command()
    # Saying so beats running the command to find out — that is the whole
    # difference between showing a change and making one.
    with pytest.raises(_invoke.InvokeError, match="cannot be previewed"):
        _invoke.build_plan(run, {})


def test_an_undecorated_function_is_refused():
    def run(ctx, inputs):
        return None

    with pytest.raises(_invoke.InvokeError, match="not decorated"):
        _invoke.invoke(run, {})


# --------------------------------------------------------------------------
# Context
# --------------------------------------------------------------------------

def test_say_collects_lines_and_flush_writes_them_once(tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("EVP_HOME", str(tmp_path))

    ctx = evp.Context("Template V2 Probe")
    ctx.say("first")
    ctx.say("second")
    path = ctx.flush()

    assert os.path.basename(path) == "template_v2_probe.log"
    body = open(path, encoding="utf-8").read()
    assert "first" in body and "second" in body

    # say() prints too — the console is what the user watches while it runs.
    assert "first" in capsys.readouterr().out

    # A second flush must not repeat the run: the lines were consumed.
    ctx.flush()
    assert open(path, encoding="utf-8").read().count("first") == 1


# --------------------------------------------------------------------------
# The result panel: text, and the questions last
# --------------------------------------------------------------------------

def test_show_uses_ui_text_and_never_ui_table(monkeypatch, tmp_path):
    """⚠️ ui.table drops or misplaces cell text after the first fill.

    CMD-ResultsTableColumns, confirmed in Archicad on 2026-08-19. The panel is
    also a proportional font, so text is the whole answer, not a stopgap.
    """
    monkeypatch.setenv("EVP_HOME", str(tmp_path))
    from evp import ui

    written = []
    monkeypatch.setattr(ui, "text", lambda value: written.append(value))
    monkeypatch.setattr(ui, "table", lambda *a, **k: pytest.fail("ui.table is unusable"))

    ctx = evp.Context("Probe")
    ctx.show("Add: one\nAdd: two", questions=["Did it appear?", "Which way?"])

    assert len(written) == 1, "ui.text replaces the panel, so one call carries it all"
    panel = written[0]
    assert panel.index("Add: one") < panel.index("NOW LOOK / REPORT")
    assert panel.rstrip().endswith("2. Which way?")


def test_show_numbers_the_questions_and_puts_them_last(monkeypatch, tmp_path):
    monkeypatch.setenv("EVP_HOME", str(tmp_path))
    from evp import ui

    written = []
    monkeypatch.setattr(ui, "text", lambda value: written.append(value))

    ctx = evp.Context("Probe")
    ctx.show("body", questions=["a", "b", "c"])
    lines = [line for line in written[0].splitlines() if line.strip()]
    assert lines[-3:] == ["1. a", "2. b", "3. c"]


def test_show_echoes_into_the_log(monkeypatch, tmp_path):
    monkeypatch.setenv("EVP_HOME", str(tmp_path))
    from evp import ui

    monkeypatch.setattr(ui, "text", lambda value: None)
    ctx = evp.Context("Probe")
    ctx.show("the body", questions=["a question"])
    path = ctx.flush()
    body = open(path, encoding="utf-8").read()
    # The panel is transient; the log survives the session and can be pasted.
    assert "the body" in body and "a question" in body


# --------------------------------------------------------------------------
# ctx.plan and the previous-run baseline
# --------------------------------------------------------------------------

def _planned_command(values):
    def planner(inputs, ctx):
        return evp.Plan("P", [
            evp.ElementSpec(key="k%d" % i, command="X", detail={"v": v})
            for i, v in enumerate(values)
        ])

    @evp.command(title="Planned", inputs=Inputs, outputs=Outputs, plan=planner)
    def run(ctx, inputs):
        return Outputs(total=len(ctx.plan))

    return run


def test_the_command_receives_the_plan_the_runtime_already_built(monkeypatch, tmp_path):
    monkeypatch.setenv("EVP_HOME", str(tmp_path))
    seen = {}

    def planner(inputs, ctx):
        seen.setdefault("calls", []).append(1)
        return evp.Plan("P", [evp.ElementSpec(key="a", command="X")])

    @evp.command(title="Planned", inputs=Inputs, outputs=Outputs, plan=planner)
    def run(ctx, inputs):
        seen["plan"] = ctx.plan
        return Outputs(total=0)

    _invoke.invoke(run, {}, folder=str(tmp_path / "Planned"))
    assert len(seen["plan"]) == 1
    # Built ONCE. A second call after the writes would read a model that already
    # contains them.
    assert len(seen["calls"]) == 1


def test_the_next_run_diffs_against_the_last_one(monkeypatch, tmp_path):
    """The failure seen live: change an input, run again, get a fresh Add list.

    Module state cannot carry this — evp._commandpath evicts every module under
    the scripts root after each run, and the external runner is a new process.
    """
    monkeypatch.setenv("EVP_HOME", str(tmp_path))
    folder = str(tmp_path / "Planned")

    _invoke.invoke(_planned_command([1, 2]), {}, folder=folder)

    captured = {}

    def planner(inputs, ctx):
        captured["previous"] = ctx.previous_plan
        return evp.Plan("P", [
            evp.ElementSpec(key="k0", command="X", detail={"v": 1}),
            evp.ElementSpec(key="k1", command="X", detail={"v": 99}),
            evp.ElementSpec(key="k2", command="X", detail={"v": 3}),
        ])

    @evp.command(title="Planned", inputs=Inputs, outputs=Outputs, plan=planner)
    def run(ctx, inputs):
        captured["diff"] = ctx.plan.diff(ctx.previous_plan)
        return Outputs(total=0)

    _invoke.invoke(run, {}, folder=folder)

    assert captured["previous"] is not None, "the baseline must survive the run"
    diff = captured["diff"]
    assert [s.key for s in diff.added] == ["k2"]
    assert [(o.key, n.key) for o, n in diff.changed] == [("k1", "k1")]
    assert [s.key for s in diff.unchanged] == ["k0"]


def test_a_failed_run_does_not_move_the_baseline(monkeypatch, tmp_path):
    monkeypatch.setenv("EVP_HOME", str(tmp_path))
    folder = str(tmp_path / "Planned")

    _invoke.invoke(_planned_command([1, 2]), {}, folder=folder)

    def planner(inputs, ctx):
        return evp.Plan("P", [evp.ElementSpec(key="zz", command="X")])

    @evp.command(title="Planned", inputs=Inputs, outputs=Outputs, plan=planner)
    def run(ctx, inputs):
        raise RuntimeError("boom")

    with pytest.raises(RuntimeError):
        _invoke.invoke(run, {}, folder=folder)

    from evp import _planstore

    # Still the plan of the run that actually completed.
    assert [s.key for s in _planstore.load(folder)] == ["k0", "k1"]


def test_a_preview_does_not_move_the_baseline(monkeypatch, tmp_path):
    monkeypatch.setenv("EVP_HOME", str(tmp_path))
    folder = str(tmp_path / "Planned")
    run = _planned_command([1, 2])
    _invoke.invoke(run, {}, folder=folder)

    def planner(inputs, ctx):
        return evp.Plan("P", [evp.ElementSpec(key="preview-only", command="X")])

    @evp.command(title="Planned", inputs=Inputs, plan=planner)
    def previewable(ctx, inputs):
        return None

    _invoke.build_plan(previewable, {}, folder=folder)

    from evp import _planstore

    # Merely LOOKING at a change must not make the next look show nothing.
    assert [s.key for s in _planstore.load(folder)] == ["k0", "k1"]


def test_two_folders_sharing_a_name_keep_separate_baselines(monkeypatch, tmp_path):
    monkeypatch.setenv("EVP_HOME", str(tmp_path))
    from evp import _planstore

    repo = str(tmp_path / "repo" / "Planned")
    deployed = str(tmp_path / "deployed" / "Planned")
    _invoke.invoke(_planned_command([1]), {}, folder=repo)
    _invoke.invoke(_planned_command([1, 2, 3]), {}, folder=deployed)

    assert len(_planstore.load(repo)) == 1
    assert len(_planstore.load(deployed)) == 3


# --------------------------------------------------------------------------
# build_preview — everything the preview band shows, without running anything
# --------------------------------------------------------------------------

def _planning_command(preview=None, **extra):
    def plan(inputs, ctx):
        return evp.Plan("Preview Cmd", [
            evp.ElementSpec(key="k%d" % index, command="Tapioca.CreateColumn",
                            params={"x": [float(index)]}, label="col %d" % index)
            for index in range(inputs.count)])

    @evp.command(title="Preview Cmd", inputs=Inputs, outputs=Outputs,
                 plan=plan, preview=preview, **extra)
    def run(ctx, inputs):
        raise AssertionError("run() must not be called to build a preview")

    return run


def test_build_preview_returns_the_plan_the_diff_and_the_scene(tmp_path):
    run = _planning_command()

    plan, diff, scene = _invoke.build_preview(run, {"count": 3},
                                              folder=str(tmp_path))

    assert len(plan) == 3
    # No baseline yet, so everything reads as an addition — the honest answer on
    # a first look, not a special case.
    assert len(diff.added) == 3
    assert scene.is_empty, "no preview= means the band falls back to the text diff"


def test_a_preview_function_fills_the_scene_and_run_is_never_called(tmp_path):
    def preview(inputs, ctx):
        ctx.scene.mesh([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 1, 2)], role="add")
        ctx.scene.note("%d planned, 1 shown" % inputs.count)

    run = _planning_command(preview=preview)

    plan, _diff, scene = _invoke.build_preview(run, {"count": 5},
                                               folder=str(tmp_path))

    assert len(plan) == 5
    assert scene.triangle_count == 1
    assert scene.notes == ["5 planned, 1 shown"]


def test_declaring_preview_gives_the_3d_band_without_saying_so_twice():
    run = _planning_command(preview=lambda inputs, ctx: None)
    assert run.__evp_command__["preview_kind"] == "3d"
    assert run.__evp_command__["needs_preview"] is True


def test_a_command_with_no_plan_refuses_to_be_previewed_rather_than_running():
    run = _schema_command()

    with pytest.raises(_invoke.InvokeError) as caught:
        _invoke.build_preview(run, {"count": 2})

    assert "plan=" in str(caught.value)


def test_a_preview_does_not_move_the_diff_baseline(tmp_path):
    # Merely LOOKING at a change must not make the next look show nothing.
    run = _planning_command()
    _invoke.build_preview(run, {"count": 3}, folder=str(tmp_path))

    _plan, diff, _scene = _invoke.build_preview(run, {"count": 3},
                                                folder=str(tmp_path))
    assert len(diff.added) == 3
