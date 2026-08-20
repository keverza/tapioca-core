"""Offline tests for evp.webui — Phase 1 of the webui plan.

Pure-Python; no Archicad, no network, no evp.api. Run from the repo root::

    PYTHONPATH=AddOn/EvP/Sources/PyPackage pytest -q AddOn/EvP/Sources/PyPackage/test_webui.py

The Tapir wrapper functions (``show``, ``await_result``) are tested by the
Phase 0 showcase's in-Archicad run, not here — their bodies are mostly
``evp.api.call`` plumbing, and the only logic worth pinning (nonce check,
poll loop, cancel hook) is exercised by ``await_result``'s nonce-drop
behaviour with a stubbed transport.
"""

import json
import sys
import os
import types

import pytest

import evp
from evp import webui


# ---- nonce -----------------------------------------------------------------

def test_nonce_is_16_hex():
    n = webui.nonce()
    assert len(n) == 16
    assert all(c in "0123456789abcdef" for c in n)


def test_nonce_is_unique():
    seen = {webui.nonce() for _ in range(64)}
    assert len(seen) == 64


# ---- _esc ------------------------------------------------------------------

def test_esc_handles_none():
    assert webui._esc(None) == ""


def test_esc_escapes_html_metachars():
    assert webui._esc("<script>alert(1)</script>") == (
        "&lt;script&gt;alert(1)&lt;/script&gt;"
    )


def test_esc_preserves_lithuanian():
    assert webui._esc("\u0160\u0117ima") == "\u0160\u0117ima"


# ---- _json_for_script ------------------------------------------------------

def test_json_for_script_breaks_script_close():
    out = webui._json_for_script({"x": "</script><img onerror=alert(1)>"})
    assert "</script>" not in out
    assert "<\\/script>" in out


def test_json_for_script_preserves_unicode():
    out = webui._json_for_script({"label": "Sklypo plotas \u0161\u0117ima"})
    parsed = json.loads(out)
    assert parsed["label"] == "Sklypo plotas \u0161\u0117ima"


# ---- markdown --------------------------------------------------------------

def test_markdown_headings():
    assert "<h1>H1</h1>" in webui.markdown("# H1")
    assert "<h2>H2</h2>" in webui.markdown("## H2")
    assert "<h3>H3</h3>" in webui.markdown("### H3")


def test_markdown_bold_and_inline_code():
    out = webui.markdown("a **b** c `d`")
    assert "<strong>b</strong>" in out
    assert "<code>d</code>" in out


def test_markdown_lists():
    out = webui.markdown("- one\n- two\n")
    assert "<ul>" in out and "</ul>" in out
    assert out.count("<li>") == 2
    out = webui.markdown("1. one\n2. two\n")
    assert "<ol>" in out and "</ol>" in out
    assert out.count("<li>") == 2


def test_markdown_blockquote():
    assert "<blockquote>x</blockquote>" in webui.markdown("> x")


def test_markdown_code_fence():
    out = webui.markdown("```python\nx = 1\n```")
    assert 'class="lang-python"' in out
    assert "x = 1" in out


def test_markdown_xss_is_escaped():
    out = webui.markdown("<script>alert(1)</script>")
    assert "<script>" not in out
    assert "&lt;script&gt;" in out


# ---- blocks ----------------------------------------------------------------

def test_section_renders_numbered_heading():
    out = webui.section("1", "Title", "<p>body</p>")
    assert "<h2>1. Title</h2>" in out
    assert "<p>body</p>" in out
    assert out.startswith("<section>") and out.endswith("</section>")


def test_section_with_note():
    out = webui.section("2", "Inputs", "<p>body</p>",
                        note="a short sentence")
    assert 'class="muted"' in out
    assert "a short sentence" in out
    # body comes after the note
    assert out.index("a short sentence") < out.index("<p>body</p>")


def test_section_note_escapes_html():
    out = webui.section("3", "X", "<p>body</p>", note="<script>")
    assert "&lt;script&gt;" in out


def test_metrics_table_renders_three_cells():
    out = webui.metrics_table([("label", "value", "unit")])
    assert "<th>label</th>" in out
    assert '<td class="num">value</td>' in out
    assert '<td class="unit">unit</td>' in out


def test_metrics_table_escapes():
    out = webui.metrics_table([("<x>", "1", "m\u00b2")])
    assert "&lt;x&gt;" in out
    assert "m\u00b2" in out


def test_image_with_data_uri():
    out = webui.image("data:image/png;base64,AAAA", alt="My shot")
    assert 'src="data:image/png;base64,AAAA"' in out
    assert 'alt="My shot"' in out


def test_image_with_caption():
    out = webui.image("data:image/png;base64,AAAA", alt="x", caption="from CEF")
    assert "<figcaption>from CEF</figcaption>" in out


def test_image_none_shows_note():
    out = webui.image(None)
    assert "No image available" in out
    assert "<img" not in out


def test_note_block():
    assert 'class="note"' in webui.note("a hint")
    assert "&lt;script&gt;" in webui.note("<script>")  # escapes


def test_html_passthrough():
    assert webui.html("<custom>x</custom>") == "<custom>x</custom>"


# ---- form ------------------------------------------------------------------

def test_form_text_field():
    out = webui.form_html([
        {"name": "label", "type": "text", "label": "Label",
         "default": "slope symbol", "required": True}
    ])
    assert 'type="text"' in out
    assert 'name="label"' in out
    assert 'value="slope symbol"' in out
    assert "required" in out


def test_form_int_field_with_min_max():
    out = webui.form_html([
        {"name": "count", "type": "int", "label": "Count",
         "default": 3, "minimum": 1, "maximum": 9}
    ])
    assert 'type="number"' in out
    assert 'min="1"' in out
    assert 'max="9"' in out
    assert 'step="1"' in out
    assert 'value="3"' in out


def test_form_float_field_uses_step_any():
    out = webui.form_html([
        {"name": "ratio", "type": "float", "label": "R", "default": 0.5}
    ])
    assert 'step="any"' in out


def test_form_select_field():
    out = webui.form_html([
        {"name": "scope", "type": "select", "label": "Scope",
         "options": ["selection", "all", "storey"], "default": "all"}
    ])
    assert "<select" in out
    assert "<option" in out
    assert 'value="selection"' in out
    # 'all' is selected, 'selection' and 'storey' are not
    assert 'value="all" selected' in out
    assert 'value="storey"' in out
    assert 'value="storey" selected' not in out


def test_form_bool_field():
    out = webui.form_html([
        {"name": "enabled", "type": "bool", "label": "Enabled", "default": True}
    ])
    assert 'type="checkbox"' in out
    assert "checked" in out
    out2 = webui.form_html([
        {"name": "enabled", "type": "bool", "label": "Enabled", "default": False}
    ])
    assert 'type="checkbox"' in out2
    assert "checked" not in out2


def test_form_textarea_field():
    out = webui.form_html([
        {"name": "notes", "type": "textarea", "label": "Notes",
         "default": "a line", "rows": 6}
    ])
    assert "<textarea" in out
    assert 'rows="6"' in out
    assert "a line" in out


def test_form_submit_button_uses_label():
    out = webui.form_html([{"name": "x", "type": "text"}], submit_label="Go")
    assert 'data-label="Go"' in out
    assert ">Go</button>" in out


def test_form_data_action_default_and_custom():
    out1 = webui.form_html([{"name": "x", "type": "text"}])
    assert 'data-action="form_submit"' in out1
    out2 = webui.form_html([{"name": "x", "type": "text"}], action="approve")
    assert 'data-action="approve"' in out2


def test_form_unknown_type_raises():
    with pytest.raises(ValueError):
        webui.form_html([{"name": "x", "type": "weird"}])


def test_form_label_escapes():
    out = webui.form_html([{"name": "x", "type": "text", "label": "<x>"}])
    assert "&lt;x&gt;" in out


# ---- report / page / confirm -----------------------------------------------

def test_report_is_full_document():
    html = webui.report([webui.section("1", "T", "body")], title="My Report")
    assert html.startswith("<!doctype html>")
    assert "</html>" in html
    assert "<title>My Report</title>" in html
    assert "1. T" in html
    assert "body" in html


def test_report_stamps_nonce_in_data_block():
    html = webui.report([], title="x", nonce_value="deadbeef")
    assert "deadbeef" in html
    assert "window.__EVPDATA__" in html


def test_report_includes_form_js_so_form_works():
    # The form needs ACAPI.SubmitResult; if the JS is missing, the form is
    # decorative only. The cost of always including the JS is ~50 lines of
    # dead code on form-less pages — cheap.
    html = webui.report([], title="x")
    assert "ACAPI.SubmitResult" in html


def test_report_breaks_script_in_data():
    kwargs_block = {
        "probe": "http://x/?a=</script><img onerror=alert(1)>"
    }
    html = webui.report([], title="x")
    # Even though the showcase bakes a probe URL via DATA, a future caller
    # might pass a string that contains </script>. Pin the escape.
    out = webui._json_for_script(kwargs_block)
    assert "</script>" not in out
    # The data block is well-formed
    assert 'window.__EVPDATA__ =' in html


def test_report_omits_empty_blocks():
    # An empty list still produces a valid (empty-body) page; an explicit
    # empty string in a list is filtered.
    html = webui.report(["", webui.section("1", "T", "body"), ""],
                        title="x")
    assert "1. T" in html
    # No leftover blank-line spam
    assert "\n\n\n" not in html


def test_page_renders_body_directly():
    html = webui.page("<p>custom</p>", title="x")
    assert "<p>custom</p>" in html


def test_confirm_shows_payload_pretty_printed():
    payload = {"nonce": "n", "name": "Sklypo plotas", "count": 3}
    summary = "polls=1"
    html = webui.confirm(payload, summary, title="Got it")
    assert "Sklypo plotas" in html
    assert "polls=1" in html
    # The pretty-printed JSON is the key thing the user can copy out; the
    # helper HTML-escapes it (so the page can render the block safely),
    # so the assertions look for the escaped form.
    assert "&quot;nonce&quot;" in html
    assert "&quot;name&quot;" in html
    assert "closeBtn" in html


# ---- write_share -----------------------------------------------------------

def test_write_share_writes_to_output_path(monkeypatch, tmp_path):
    captured = {}
    def fake_output_path(name):
        captured["name"] = name
        return str(tmp_path / name)
    monkeypatch.setattr(evp.paths, "output_path", fake_output_path)

    out = webui.write_share("<html>x</html>", "page.html")
    assert captured["name"] == "page.html"
    assert out == str(tmp_path / "page.html")
    with open(out, "r", encoding="utf-8") as f:
        assert f.read() == "<html>x</html>"


# ---- await_result nonce + cancel + poll ------------------------------------

class _FakeResult:
    """Minimal stand-in for evp.api.Result.ok / .data used by await_result."""
    def __init__(self, ok=True, data=None, error=None):
        self.ok = ok
        self.data = data
        self.error = error


def _patched_call(responses, *, cancel_after=None):
    """Replace ``evp.api._transport`` with a command-dispatching stub.

    The transport is module-level cached in ``evp.api._resolved``; we
    replace the whole ``_transport`` accessor (not just ``call``) so the
    lazy init does not need an Archicad transport to fall back on.

    The stub dispatches by command name: ``EvP.PollCancel`` returns
    ``ok=False`` (no cancel) so the bus-cancel check is free, and
    ``Tapir.GetScriptUIResult`` returns the next scripted ``_FakeResult``
    in ``responses``. Other commands (a stray ``EvP.SetTracing`` from
    ``debug``) also get ``ok=False`` — tests don't care.

    ``cancel_after`` is an integer index (1-based, counts every transport
    call — both ``PollCancel`` and ``GetScriptUIResult`` — so a tightly
    bounded value is enough) after which the mocked ``check_cancel`` raises
    ``evp.Cancelled``. ``None`` disables the cancel. The helper's
    ``check_cancel`` is the in-test counter, not the real one.
    """
    import evp.api as _api
    it = iter(responses)
    state = {"n": 0, "cancelled": False}

    def transport(command, params_json):
        state["n"] += 1
        if command == "Tapir.GetScriptUIResult":
            try:
                r = next(it)
            except StopIteration:
                return json.dumps({"ok": False})
            return json.dumps({"ok": r.ok, "data": r.data, "error": r.error,
                               "meta": {}})
        return json.dumps({"ok": False})

    def check_cancel():
        if cancel_after is not None and state["n"] >= cancel_after:
            raise evp.Cancelled("test")

    return transport, check_cancel, state


def test_await_result_drops_stale_nonce(monkeypatch):
    # First payload: wrong nonce -> dropped, keep polling.
    # Second payload: correct nonce -> returned.
    responses = [
        _FakeResult(ok=True, data={"hasResult": True,
                                   "result": json.dumps({"nonce": "old", "x": 1})}),
        _FakeResult(ok=True, data={"hasResult": True,
                                   "result": json.dumps({"nonce": "right", "y": 2})}),
    ]
    transport, _cc, _ = _patched_call(responses)
    monkeypatch.setattr(evp.api, "_resolved", ("stub", transport))

    parsed, polls, timings = webui.await_result("right", poll=0.001)
    assert parsed == {"nonce": "right", "y": 2}
    assert polls == 2
    assert len(timings) == 2
    assert all(isinstance(t, float) for t in timings)


def test_await_result_returns_none_on_timeout(monkeypatch):
    transport, _cc, _ = _patched_call([])  # never returns hasResult
    monkeypatch.setattr(evp.api, "_resolved", ("stub", transport))

    parsed, polls, timings = webui.await_result("n", poll=0.001, timeout=0.01)
    assert parsed is None
    assert polls >= 1
    assert len(timings) == polls


def test_await_result_raises_cancelled(monkeypatch):
    transport, cc, _ = _patched_call([], cancel_after=3)
    monkeypatch.setattr(evp.api, "_resolved", ("stub", transport))
    # The real check_cancel goes through the bus and never raises in this
    # stub (PollCancel returns ok=False), so we replace it with the helper's
    # counter. The user's command in Archicad imports evp.runtime directly
    # and gets the real one.
    monkeypatch.setattr(evp.runtime, "check_cancel", cc)

    with pytest.raises(evp.Cancelled):
        webui.await_result("n", poll=0.001)


def test_await_result_handles_non_json_payload(monkeypatch):
    responses = [
        _FakeResult(ok=True, data={"hasResult": True,
                                   "result": "not json at all"}),
        _FakeResult(ok=True, data={"hasResult": True,
                                   "result": json.dumps({"nonce": "n", "ok": 1})}),
    ]
    transport, _cc, _ = _patched_call(responses)
    monkeypatch.setattr(evp.api, "_resolved", ("stub", transport))

    parsed, _p, _t = webui.await_result("n", poll=0.001)
    # First payload: parse failed but a dict with the _parse_error marker
    # is still produced; since the dict has nonce=None, it does not match
    # 'n', so we keep polling and get the second one.
    assert parsed == {"nonce": "n", "ok": 1}


# ---- show_and_await end-to-end (stubbed) ----------------------------------

def test_show_and_await_returns_none_on_show_failure(monkeypatch):
    # show() returns ok=False, await_result never runs.
    def fake_show(html, **kw):
        return _FakeResult(ok=False, data=None,
                           error={"code": "NoTapir",
                                  "message": "no Tapir installed"})

    monkeypatch.setattr(webui, "show", fake_show)

    out = webui.show_and_await("<html></html>", title="x")
    assert out is None


def test_show_and_await_round_trip(monkeypatch):
    shown = []
    def fake_show(html, **kw):
        shown.append((html, kw))
        return _FakeResult(ok=True, data={"ok": True})
    monkeypatch.setattr(webui, "show", fake_show)

    def fake_await(nonce_value, **kw):
        return ({"nonce": nonce_value, "action": "form_submit", "x": 1},
                1, [5.0])
    monkeypatch.setattr(webui, "await_result", fake_await)

    out = webui.show_and_await("<html>x</html>", title="Round trip",
                               poll=0.05, auto_height=True)
    assert out["action"] == "form_submit"
    assert out["x"] == 1
    # Title is passed through.
    assert shown[0][1].get("title") == "Round trip"
    # The nonce in the returned payload matches the one we sent.
    # (show_and_await creates its own nonce; we only check the round-trip
    # shape — the nonce is internal.)
    assert isinstance(out["nonce"], str) and len(out["nonce"]) == 16
