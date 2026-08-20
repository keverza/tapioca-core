"""Call a command's `run()` — the ONE place that decides how.

Three runners reach a command: the embedded interpreter (`EvPPy.cpp`'s
`_evp_run_command`), the external subprocess (`_evp_external_main.py`), and the
offline harness (`tests/dryrun_command.py`). Before this module each spelled the
call itself, as `fn(**params)`.

That was fine while there was one calling convention. There are now two — the
signature form and the schema form — and three copies of a two-branch decision
is how `_commandpath` got into trouble: the copies drifted, a helper worked
externally and failed in-process, and nothing in the command could say why. So
the decision lives here and the runners call `invoke()`.

This module must stay importable with nothing installed beyond the stdlib until
it actually touches a model — `import evp` runs in processes with no pydantic
and no transport.
"""

from __future__ import annotations

import os

from . import _planstore
from .context import Context

__all__ = ["invoke", "build_plan", "build_preview", "run_action", "InvokeError"]


class InvokeError(TypeError):
    """The command and the values it was handed do not fit together."""


def _meta(fn):
    meta = getattr(fn, "__evp_command__", None)
    if meta is None:
        raise InvokeError("run() is not decorated with @evp.command")
    return meta


def invoke(fn, params, folder=None, mode="run", previous_plan=None):
    """Run `fn` with `params` (the dict the palette collected).

    Signature-style commands are called `fn(**params)`, exactly as before —
    every existing command goes down this branch and nothing about it changes.

    Schema-style commands are called `fn(ctx, inputs)` with `params` validated
    into the declared Inputs model first, and the return value validated against
    Outputs. Both directions are checked, because a consumer binds to the
    declared output shape and an unvalidated return is a shape that drifts
    silently.
    """
    meta = _meta(fn)
    model = meta.get("inputs")
    if model is None:
        return fn(**params)

    # The previous plan comes from disk, not from the caller: module state does
    # not survive a run (evp._commandpath evicts everything under the scripts
    # root) and the external runner is a fresh process. Without this every run
    # diffs against None and reports its whole plan as additions — seen live.
    if previous_plan is None:
        previous_plan = _planstore.load(folder)

    ctx = Context(name=meta.get("title") or folder or "command",
                  folder=folder, mode=mode, previous_plan=previous_plan,
                  preview_kind=meta.get("preview_kind"))
    inputs = _validate_inputs(model, params)

    # The plan is built ONCE, here, BEFORE run() — and handed to the command as
    # ctx.plan. Two reasons, both learned the hard way:
    #
    #  * it must be computed before the writes. Re-planning afterwards to record
    #    a baseline would re-read a model that now CONTAINS what was just
    #    written, so a command whose plan derives from the model would record a
    #    doubled or empty plan and the next diff would be nonsense.
    #  * computing it twice is computing it twice. A planner reads the model;
    #    that is a bus round trip per run for no gain.
    #
    # A planner that raises takes the run with it, deliberately: the plan IS the
    # command, and a command that cannot say what it would do must not proceed
    # to do it.
    planner = meta.get("plan")
    if planner is not None:
        ctx.plan = planner(inputs, ctx)

    result = fn(ctx, inputs)

    # Saved only AFTER run() returns, so a failed run does not move the baseline:
    # the next run still diffs against the last plan that actually completed.
    if ctx.plan is not None:
        _planstore.save(folder, ctx.plan)

    validated = _validate_outputs(meta.get("outputs"), result, ctx.name)
    # And the result, so an action button has something to export without
    # re-running the command. Same reason as the plan, same failure behaviour:
    # a store that cannot be written costs a button its input, never the run.
    if meta.get("actions"):
        _planstore.save_outputs(folder, validated)
    return validated


def build_plan(fn, params, folder=None, previous_plan=None):
    """Ask a command for its Plan WITHOUT running it.

    This is what the preview band calls. It needs `plan=` on the decorator: a
    command whose plan can only be produced by running it has no preview, and
    saying so is better than running it to find out.
    """
    meta = _meta(fn)
    model = meta.get("inputs")
    planner = meta.get("plan")
    if model is None or planner is None:
        raise InvokeError(
            "%s declares no plan=, so it cannot be previewed. A preview must "
            "not run the command — that is the difference between showing a "
            "change and making one." % (meta.get("title") or folder or "command"))

    if previous_plan is None:
        previous_plan = _planstore.load(folder)
    ctx = Context(name=meta.get("title") or folder or "command",
                  folder=folder, mode="preview", previous_plan=previous_plan,
                  preview_kind=meta.get("preview_kind"))
    # A preview does NOT move the baseline. Only a completed run does — otherwise
    # merely looking at a change would make the next look show nothing.
    return planner(_validate_inputs(model, params), ctx)


def build_preview(fn, params, folder=None, previous_plan=None):
    """Everything the preview band shows, WITHOUT running the command.

    Returns `(plan, diff, scene)`. The scene is empty unless the command declares
    `preview=` — which is the normal case and not a shortfall: the band then shows
    the text diff, which every planning command gets for free.

    ⚠️ THE PLANNER RUNS, the command does not. `plan()` reads the model; `run()`
    writes it. That distinction is the entire reason a preview is possible, and it
    is why a command whose plan can only be produced by running it has no preview
    (build_plan says so by name rather than running it to find out).
    """
    meta = _meta(fn)
    plan = build_plan(fn, params, folder=folder, previous_plan=previous_plan)

    ctx = Context(name=meta.get("title") or folder or "command",
                  folder=folder, mode="preview",
                  previous_plan=previous_plan
                  if previous_plan is not None else _planstore.load(folder),
                  preview_kind=meta.get("preview_kind"))
    ctx.plan = plan

    painter = meta.get("preview")
    if painter is not None:
        painter(_validate_inputs(meta["inputs"], params), ctx)

    return plan, plan.diff(ctx.previous_plan), ctx.scene


def _validate_inputs(model, params):
    try:
        return model.model_validate(params or {})
    except Exception as exc:
        # pydantic's ValidationError already names the field and the rule. What
        # it does not say is which command, and the palette shows one message.
        raise InvokeError(
            "the values the palette sent do not fit %s:\n%s"
            % (model.__name__, exc)) from exc


def _validate_outputs(model, result, name):
    if model is None:
        return result
    if isinstance(result, model):
        return result
    try:
        return model.model_validate(result)
    except Exception as exc:
        raise InvokeError(
            "%s returned something that is not its declared %s:\n%s"
            % (name, model.__name__, exc)) from exc


def run_action(fn, action, folder=None):
    """Execute one of a command's declared actions on its LAST result.

    ⚠️ THE COMMAND IS NOT RE-RUN. Its Outputs and Plan come from the run store,
    because a run that had to happen twice to be exported is a run that performed
    every one of its writes twice. This is the whole reason the store keeps the
    result and not just the plan.

    A NAMED action (evp.outputs.STANDARD_ACTIONS) is executed by the framework
    from the declared Outputs. A command's OWN action is its function, called with
    the same stored result — the flexible half, so the standard set never has to
    grow to cover one command's special case.
    """
    from . import outputs as _outputs

    meta = _meta(fn)
    declared = meta.get("actions") or []
    custom = _custom_actions(fn)
    if action not in declared and action not in custom:
        raise InvokeError(
            "%s declares no action %r. Declared: %s. An action the palette offers "
            "and the command does not know is a button that can only fail."
            % (meta.get("title") or folder or "command", action,
               ", ".join(sorted(set(declared) | set(custom))) or "none"))

    stored = _planstore.load_outputs(folder)
    if stored is None and action in _outputs.STANDARD_ACTIONS:
        raise InvokeError(
            "%r has no stored result to act on. Run the command first — an action "
            "exports what the last run produced, it does not produce it."
            % action)

    if action in custom:
        ctx = Context(name=meta.get("title") or folder or "command",
                      folder=folder, mode="action",
                      previous_plan=_planstore.load(folder))
        return custom[action](ctx, stored)

    name = os.path.basename(os.path.normpath(folder)) if folder else "tapioca"
    return _outputs.run_action(action, name, outputs_obj=stored,
                               plan=_planstore.load(folder),
                               schema=_planstore.load_outputs_schema(folder))


def _custom_actions(fn):
    """{name: function} for every @evp.action in the command's own module.

    Discovered through `sys.modules[fn.__module__]` rather than kept on `run`,
    because the decorator is applied to a DIFFERENT function and has no way to
    reach the command's from there. The module is the thing both belong to.
    """
    import sys

    module = sys.modules.get(fn.__module__)
    if module is None:
        return {}
    found = {}
    for value in vars(module).values():
        record = getattr(value, "__evp_action__", None)
        if record is not None and callable(value):
            found[record["name"]] = value
    return found
