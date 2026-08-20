"""evp.webui — Phase 1 of the webui plan: the reusable web-UI helper.

Commands use ~5 lines to get the same behaviour the Phase 0 showcase proved
in Archicad::

    from evp import webui
    html = webui.report([
        webui.section("1", "Metrics", webui.metrics_table(rows)),
        webui.section("2", "Inputs",  webui.form_html([...fields...])),
    ], title="My Report")
    webui.write_share(html, "my_report.html")           # share/export
    submitted = webui.show_and_await(html, title="My Report")
    if submitted and submitted.get("action") == "form_submit":
        # user clicked Submit; the parsed form is in `submitted`
        ...

The contract: ONE palette instance, REPLACED on every ``show`` (Tapir's
``ShowScriptUI`` resets the pending result). The helper enforces the
"current owner" discipline the plan pressure-test calls for.

**Not in scope** (Phase 3 only): page-initiated ``fetch()``, push events,
calling back into the bus from the page. The CORS verdict sealed the
bake-once model for Phases 0–2.

Phase 0 lessons the helper bakes in:

* One palette, replaced — title is the current owner; show_and_await stamps a
  nonce; await_result drops mismatches.
* First poll after ``ShowScriptUI`` is ~4.7 s (CEF warm-up); 300 ms cadence is
  fine. await_result surfaces the count + per-poll timings so spikes are
  visible, not silent.
* Everything the page shows must be BAKED IN — the page origin is
  ``about:blank``/null and every ``fetch()`` is blocked. ``write_share`` is
  the share/export mechanism (plan §1 rules): the same page opens in a
  desktop browser.
* ``import helper`` works in a command, ``from . import helper`` does NOT
  (the runner uses ``importlib.util.spec_from_file_location``, a module
  spec, not a package spec).
"""

import json
import os
import time
from string import Template as _Template

import evp


# =============================================================================
# Low-level Tapir wrappers
# =============================================================================

def nonce():
    """A 16-hex-char nonce for submit round-tripping. ``await_result`` drops
    payloads whose nonce does not match — a stale submit from a previous
    ``show`` is harmless, even though Tapir's C++ side also resets the
    pending flag on each call (belt and braces)."""
    return "%016x" % int.from_bytes(os.urandom(8), "big")


def show(html, *, title=None, auto_height=True, width=None, height=None,
         resizable=True, zoom_enabled=True, scroll_bars_visible=True,
         context_menu_enabled=True, navigation_disabled=False, **extra):
    """Show one page in Tapir's ``ShowScriptUI`` palette.

    ``html`` is the only required parameter; the rest mirror the C++ schema
    and have sensible defaults from the Phase 0 probe. The return is the
    ``evp.Result`` envelope (so a caller can ``.ok``-check or read
    ``.error``); the function does not raise on a Tapir-side failure
    (``raise_on_error=False``) because a missing/old Tapir is a normal
    configuration, not a script bug.
    """
    params = {
        "htmlContent": html,
        "autoHeight":  auto_height,
        "resizable":   resizable,
        "zoomEnabled": zoom_enabled,
        "scrollBarsVisible":    scroll_bars_visible,
        "contextMenuEnabled":   context_menu_enabled,
        "navigationDisabled":   navigation_disabled,
    }
    if title is not None:
        params["title"] = title
    if width is not None:
        params["width"] = width
    if height is not None:
        params["height"] = height
    for k, v in extra.items():
        params[k] = v
    return evp.api.call("Tapir.ShowScriptUI", params, raise_on_error=False)


def await_result(nonce_value, *, poll=0.3, timeout=None):
    """Poll ``Tapir.GetScriptUIResult`` until the page calls
    ``ACAPI.SubmitResult`` with a payload whose nonce matches.

    Returns ``(parsed_dict, polls_count, timings_ms_list)`` on a submit, or
    ``(None, polls_count, timings_ms_list)`` on timeout. ``evp.runtime.check_cancel``
    runs every tick — closing the palette / pressing Stop raises
    ``evp.Cancelled`` within one poll interval, which is the clean-exit the
    plan pressure-test calls for.

    Stale submits (nonce mismatch) are dropped silently — the next page
    sends a new nonce, the old one cannot win.
    """
    deadline = None if timeout is None else (time.monotonic() + timeout)
    polls = 0
    timings = []
    while True:
        evp.runtime.check_cancel()
        if deadline is not None and time.monotonic() > deadline:
            return None, polls, timings
        t0 = time.perf_counter()
        r = evp.api.call("Tapir.GetScriptUIResult", {}, raise_on_error=False)
        timings.append((time.perf_counter() - t0) * 1000.0)
        polls += 1
        if r.ok and (r.data or {}).get("hasResult"):
            payload = r.data.get("result", "")
            try:
                parsed = json.loads(payload) if payload else {}
            except Exception:
                parsed = {"_raw": payload, "_parse_error": True}
            if isinstance(parsed, dict) and parsed.get("nonce") != nonce_value:
                continue
            return parsed, polls, timings
        time.sleep(poll)


def show_and_await(html, *, title=None, poll=0.3, timeout=None, **show_opts):
    """One call: show + wait for submit. Returns the parsed submit dict, or
    ``None`` on a failed ``show`` or timeout. Raises ``evp.Cancelled`` on a
    palette close / Stop (the caller decides whether to catch and re-show
    a confirmation)."""
    n = nonce()
    shown = show(html, title=title, **show_opts)
    if not shown.ok:
        return None
    submitted, _polls, _timings = await_result(n, poll=poll, timeout=timeout)
    return submitted


def write_share(html, filename):
    """Save the page to ``evp.paths.output_path(filename)``.

    That is the share/export mechanism the plan §1 rules call for: a
    self-contained HTML file openable in any desktop browser, sendable to
    anyone, with no Archicad in the loop. Returns the absolute path written.
    """
    path = evp.paths.output_path(filename)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(html)
    return path


# =============================================================================
# Blocks — one piece of the report() / page() body
# =============================================================================

def section(num, title, body_html, *, note=None):
    """A numbered section. ``num`` is a free string ('1', '2a', 'NOW LOOK').
    ``note`` is rendered above the body in muted text — one sentence of
    context the user reads first."""
    parts = ['<section><h2>%s. %s</h2>' % (_esc(num), _esc(title))]
    if note:
        parts.append('<p class="muted">%s</p>' % _esc(note))
    parts.append(body_html)
    parts.append('</section>')
    return ''.join(parts)


def metrics_table(rows):
    """``rows``: ``[(label, value, unit), ...]`` — same shape
    ``MassingFeasibility.massingcalc.metrics_rows`` returns, so a real
    command can pass its metrics straight through.

    The CSS is baked in; the Lithuanian strings come through ``_esc`` which
    is encoding-safe (the Phase 0 probe proved UniString survives the bus)."""
    parts = ['<table class="metrics"><tbody>']
    for label, value, unit in rows:
        parts.append(
            '<tr><th>%s</th><td class="num">%s</td><td class="unit">%s</td></tr>'
            % (_esc(label), _esc(value), _esc(unit))
        )
    parts.append('</tbody></table>')
    return ''.join(parts)


def image(data_uri, *, alt="Image", caption=None):
    """Inline image. ``data_uri`` is a ``data:image/...;base64,...`` string
    (the command built it; the page never opened a socket). When ``None`` a
    note explains how to get a screenshot — Phase 0's behaviour, kept."""
    if not data_uri:
        return (
            '<div class="note">No image available. Take a screenshot (press '
            '<b>Screenshots</b> in the EvP palette while a 3D view is front), '
            'then run this command again.</div>'
        )
    out = ('<figure><img class="shot" alt="%s" src="%s">'
           % (_esc(alt), _esc(data_uri)))
    if caption:
        out += '<figcaption>%s</figcaption>' % _esc(caption)
    out += '</figure>'
    return out


def markdown(text):
    """Render a small subset of markdown — enough to display a command
    README / HANDOFF.md / a help page in the palette. Not a product; if a
    command needs more, render markdown to HTML upstream and pass via
    ``html()`` instead.

    Handles: ATX headings, paragraphs, blockquotes, unordered + ordered
    lists, fenced code blocks, inline code, bold.
    """
    return _markdown_to_html(text)


def note(text):
    """A short highlighted callout — for "this section requires X first"
    hints and warnings."""
    return '<div class="note">%s</div>' % _esc(text)


def html(raw):
    """Raw HTML passthrough. The caller has already escaped; useful for
    embedding complex fragments (e.g. an SVG chart) that the helper
    doesn't need to know about."""
    return raw


# =============================================================================
# Form — text/number/select/checkbox inputs that submit via ACAPI
# =============================================================================

def form_html(fields, *, submit_label="Submit", action="form_submit"):
    """Render a ``<form>`` block. The page-side JS (auto-included by
    ``page()`` / ``report()``) reads ``FormData``, builds an object, and
    calls ``ACAPI.SubmitResult(JSON.stringify({nonce, action, ...fields}))``.

    Each field is a dict:

        {"name": "label",  "type": "text",   "label": "Label",
         "default": "slope symbol", "required": True}
        {"name": "count",  "type": "int",    "label": "Count",
         "default": 3, "minimum": 1, "maximum": 9}
        {"name": "ratio",  "type": "float",  "label": "Ratio",
         "default": 0.75}
        {"name": "scope",  "type": "select", "label": "Scope",
         "options": ["selection", "all", "storey"], "default": "all"}
        {"name": "enabled","type": "bool",   "label": "Enabled",
         "default": True}
    """
    rows = []
    for f in fields:
        rows.append(_render_field(f))
    return (
        '<form id="submitForm" autocomplete="off" data-action="%s">'
        '<div class="form-fields">%s</div>'
        '<div class="form-row"><button type="submit" data-label="%s">%s</button>'
        '<span id="submitStatus" class="muted">no submit yet</span></div>'
        '</form>'
    ) % (_esc(action), ''.join(rows), _esc(submit_label), _esc(submit_label))


def _render_field(f):
    name = f["name"]
    label = f.get("label", name)
    default = f.get("default")
    t = f.get("type", "text")
    if t == "text":
        attrs = " required" if f.get("required") else ""
        return _form_row(label,
            '<input type="text" name="%s" value="%s"%s>'
            % (_esc(name), _esc("" if default is None else default), attrs))
    if t in ("int", "float"):
        a = []
        if "minimum" in f:
            a.append('min="%s"' % f["minimum"])
        if "maximum" in f:
            a.append('max="%s"' % f["maximum"])
        a.append('step="%s"' % ("any" if t == "float" else "1"))
        return _form_row(label,
            '<input type="number" name="%s" value="%s" %s>'
            % (_esc(name), _esc(default if default is not None else 0), ' '.join(a)))
    if t == "select":
        opts = ''.join(
            '<option value="%s"%s>%s</option>'
            % (_esc(o), ' selected' if o == default else '', _esc(o))
            for o in f.get("options", [])
        )
        return _form_row(label, '<select name="%s">%s</select>' % (_esc(name), opts))
    if t == "bool":
        checked = " checked" if default else ""
        return _form_row(label,
            '<label class="checkbox"><input type="checkbox" name="%s"%s> %s</label>'
            % (_esc(name), checked, _esc(label)))
    if t == "textarea":
        return _form_row(label,
            '<textarea name="%s" rows="%d">%s</textarea>'
            % (_esc(name), int(f.get("rows", 4)),
               _esc("" if default is None else default)))
    raise ValueError("evp.webui.form_html: unknown field type %r" % t)


def _form_row(label, control_html):
    return ('<div class="form-row"><label>%s %s</label></div>'
            % (_esc(label), control_html))


# =============================================================================
# High-level page renderers
# =============================================================================

def report(blocks, *, title="EvP Web UI", nonce_value=None, data=None,
            extra_js=""):
    """Render a multi-section page. ``blocks`` is a list of HTML section
    strings (use ``section(num, title, body, note=...)`` to make them).

    The page always includes the form JS handler — it is a no-op if no
    ``<form id="submitForm">`` is on the page, so it is safe to include
    unconditionally. ``data`` is merged into the data block (alongside
    ``{"nonce": nonce}``) so page-side JS can read custom values without
    rebuilding the page. ``extra_js`` is appended as a second ``<script>``
    block (raw, not escaped — caller is responsible) for page-specific
    behaviour the helper does not provide, e.g. a CORS-probe snippet.
    """
    if nonce_value is None:
        nonce_value = nonce()
    body = "\n".join(b for b in blocks if b)
    merged = {"nonce": nonce_value}
    if data:
        merged.update(data)
    return _render_page(body, title=title, nonce=nonce_value,
                        data=merged, extra_js=extra_js)


def page(body_html, *, title="EvP Web UI", nonce_value=None, data=None,
         extra_js=""):
    """Render a page from a single body block. Use ``report`` for a list of
    sections; use ``page`` for a hand-built body. ``data`` is merged into
    the data block; ``extra_js`` is appended as a second ``<script>``
    block after the helper's own JS."""
    if nonce_value is None:
        nonce_value = nonce()
    merged = {"nonce": nonce_value}
    if data:
        merged.update(data)
    return _render_page(body_html, title=title, nonce=nonce_value,
                        data=merged, extra_js=extra_js)


def confirm(submitted, summary, *, title="Got it", nonce_value=None,
             extra_js=""):
    """A small confirmation page after a submit. ``submitted`` is the parsed
    payload the command received; ``summary`` is any short text the command
    wants to show the user (timings, counts, what it did). The page has a
    Close button that submits ``{action: "closed"}`` so the calling
    command's await loop ends cleanly."""
    if nonce_value is None:
        nonce_value = nonce()
    pretty = json.dumps(submitted, indent=2, ensure_ascii=False)
    body = (
        '<p class="muted">The command side received the payload below. '
        'Close to finish.</p>'
        '<h2>Submitted</h2>'
        '<pre class="probe"><code>%s</code></pre>'
        '<h2>Run summary</h2>'
        '<pre class="probe"><code>%s</code></pre>'
        '<p><button id="closeBtn" data-label="Close palette">'
        'Close palette</button></p>'
    ) % (_esc(pretty), _esc(summary))
    return _render_page(body, title=title, nonce=nonce_value,
                        extra_js=extra_js)


# =============================================================================
# Internals — escaping, CSS, JS, page template, markdown converter
# =============================================================================

def _esc(s):
    """html.escape with quotes=True. Used everywhere a string lands in
    markup. Lithuanian (and any other non-ASCII) round-trips intact."""
    import html as _html_lib
    return _html_lib.escape("" if s is None else str(s), quote=True)


def _json_for_script(obj):
    """A JSON literal safe to drop between ``<script>...</script>``.

    ``json.dumps`` escapes string contents but does NOT break the
    ``</script>`` sequence; the standard fix is to replace ``</`` with
    ``<\\/`` so the parser cannot close the script tag from inside a
    string. Tested in test_webui.py — this is the one place that touches
    JSON in HTML, the one place that knows to do the fix."""
    return json.dumps(obj, ensure_ascii=False).replace("</", "<\\/")


_CSS = """\
:root { color-scheme: light; }
* { box-sizing: border-box; }
body { font: 14px/1.45 -apple-system, "Segoe UI", system-ui, sans-serif;
       margin: 0; padding: 0 16px 24px; color: #1d1f23; background: #fafaf8; }
h1 { font-size: 18px; margin: 16px 0 4px; }
h2 { font-size: 14px; text-transform: uppercase; letter-spacing: .08em;
     color: #6a6f78; margin: 24px 0 6px; padding-bottom: 4px;
     border-bottom: 1px solid #e5e7eb; }
h3 { font-size: 13px; margin: 14px 0 4px; color: #2c3038; }
p  { margin: 6px 0; }
code { font: 12px/1.4 "JetBrains Mono", Consolas, "Courier New", monospace;
       background: #eef0f3; padding: 1px 5px; border-radius: 3px; }
pre  { background: #1f2228; color: #e7e9ee; padding: 10px 12px; border-radius: 4px;
       overflow-x: auto; font: 12px/1.45 "JetBrains Mono", Consolas, monospace; }
pre code { background: transparent; padding: 0; color: inherit; }
blockquote { margin: 6px 0; padding: 4px 12px; border-left: 3px solid #cdd1d8;
             color: #4a4f58; background: #f3f4f6; }
ul, ol { margin: 4px 0 4px 22px; padding: 0; }
li { margin: 2px 0; }
strong { color: #1d1f23; }

table.metrics { width: 100%; border-collapse: collapse; margin: 4px 0 0; }
table.metrics th, table.metrics td { text-align: left; padding: 5px 8px;
                                     border-bottom: 1px solid #e9ebef; }
table.metrics th { font-weight: 500; color: #4a4f58; }
table.metrics td.num { text-align: right; font-variant-numeric: tabular-nums;
                      font-family: "JetBrains Mono", Consolas, monospace; }
table.metrics td.unit { color: #6a6f78; width: 1%; white-space: nowrap; }

img.shot { display: block; max-width: 100%; height: auto;
           border: 1px solid #d8dbe1; border-radius: 4px; margin: 6px 0; }
figure { margin: 0; }
figcaption { font-size: 12px; color: #6a6f78; margin-top: 4px; }

form { background: #ffffff; border: 1px solid #d8dbe1; border-radius: 4px;
       padding: 10px 12px; margin: 6px 0; }
form .form-fields { display: flex; flex-direction: column; gap: 6px; }
form .form-row { display: flex; gap: 12px; align-items: center; margin: 6px 0;
                 flex-wrap: wrap; }
form label { display: inline-flex; align-items: center; gap: 6px;
             font-size: 13px; color: #2c3038; }
form input[type=text], form input[type=number], form select, form textarea {
  font: 13px inherit; padding: 4px 8px; border: 1px solid #c8ccd4;
  border-radius: 3px; background: #fff; }
form input[type=number] { width: 90px; }
form input[type=checkbox] { transform: scale(1.1); }
form label.checkbox { font-weight: 500; }
form button { font: 13px inherit; padding: 6px 14px; background: #2563eb;
              color: #fff; border: none; border-radius: 3px; cursor: pointer; }
form button:hover { background: #1d4ed8; }
form button[disabled] { background: #94a3b8; cursor: default; }

.note  { background: #fff8e1; border: 1px solid #f0d791; padding: 8px 10px;
         border-radius: 4px; margin: 6px 0; font-size: 13px; }
.muted { color: #6a6f78; font-size: 12px; }
.kv    { font: 12px "JetBrains Mono", Consolas, monospace; color: #2c3038; }
.kv b  { color: #6a6f78; font-weight: 500; }
.probe { background: #0f1115; color: #d8dde5; padding: 10px 12px;
         border-radius: 4px; font: 12px/1.5 "JetBrains Mono", Consolas, monospace;
         white-space: pre-wrap; word-break: break-word; }
.probe .ok { color: #6ee7a8; }
.probe .err { color: #fca5a5; }
"""


_JS = r"""
(function () {
  var DATA = window.__EVPDATA__ || {};

  // ---- form: bundle the inputs and call the page-bridge API --------------
  var form = document.getElementById('submitForm');
  if (form) {
    form.addEventListener('submit', function (e) {
      e.preventDefault();
      var action = form.getAttribute('data-action') || 'form_submit';
      var payload = { nonce: DATA.nonce, action: action };
      var inputs = form.querySelectorAll('input, select, textarea');
      for (var i = 0; i < inputs.length; i++) {
        var el = inputs[i];
        if (!el.name) continue;
        if (el.type === 'checkbox') {
          payload[el.name] = !!el.checked;
        } else if (el.type === 'number') {
          payload[el.name] = Number(el.value);
        } else {
          payload[el.name] = el.value;
        }
      }
      submit(payload, form.querySelector('button'));
    });
  }

  // ---- close button on the confirmation page ------------------------------
  var closeBtn = document.getElementById('closeBtn');
  if (closeBtn) {
    closeBtn.addEventListener('click', function () {
      submit({ nonce: DATA.nonce, action: 'closed' }, closeBtn);
    });
  }

  function submit(payload, btn) {
    if (btn) { btn.disabled = true; btn.textContent = 'Submitting...'; }
    try {
      if (!window.ACAPI || !ACAPI.SubmitResult) {
        render('ACAPI.SubmitResult is not available -- this page is not running in Tapir\u2019s Script UI palette.', true);
        if (btn) { btn.disabled = false; btn.textContent = btn.dataset.label || 'Submit'; }
        return;
      }
      ACAPI.SubmitResult(JSON.stringify(payload));
    } catch (err) {
      render(String(err), true);
      if (btn) { btn.disabled = false; btn.textContent = btn.dataset.label || 'Submit'; }
    }
  }

  function render(text, isErr) {
    var box = document.getElementById('submitStatus');
    if (!box) return;
    box.textContent = text;
    box.className = isErr ? 'probe err' : 'probe';
  }
})();
"""


_PAGE_TPL = _Template("""<!doctype html><html><head><meta charset='utf-8'>
<title>$title</title><style>$css</style></head><body>
<h1>$title</h1>
<p class='muted'>nonce: <span class='kv'>$nonce</span></p>
$body
<script>window.__EVPDATA__ = $data;</script>
<script>$js</script>
$extra_js_block
</body></html>""")


_EXTRA_JS_TPL = _Template("<script>$extra_js</script>")


def _render_page(body, *, title, nonce, data=None, extra_js=""):
    extra_js_block = (_EXTRA_JS_TPL.safe_substitute(extra_js=extra_js)
                      if extra_js else "")
    merged = {"nonce": nonce}
    if data:
        merged.update(data)
    return _PAGE_TPL.safe_substitute(
        title=_esc(title),
        css=_CSS,
        nonce=_esc(nonce),
        body=body,
        data=_json_for_script(merged),
        js=_JS,
        extra_js_block=extra_js_block,
    )


# ---- markdown (~40-line minimal converter) ----------------------------------

def _inline_md(text):
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == "`":
            j = text.find("`", i + 1)
            if j < 0:
                out.append(_esc(c))
                i += 1
                continue
            out.append("<code>%s</code>" % _esc(text[i + 1:j]))
            i = j + 1
        elif c == "*" and i + 1 < len(text) and text[i + 1] == "*":
            j = text.find("**", i + 2)
            if j < 0:
                out.append(_esc(c))
                i += 1
                continue
            out.append("<strong>%s</strong>" % _esc(text[i + 2:j]))
            i = j + 2
        else:
            out.append(_esc(c))
            i += 1
    return "".join(out)


def _markdown_to_html(text):
    lines = text.splitlines()
    out = []
    i = 0
    in_ul = in_ol = False
    in_code = False
    code_buf = []
    code_lang = ""

    def close_lists():
        nonlocal in_ul, in_ol
        if in_ul:
            out.append("</ul>")
            in_ul = False
        if in_ol:
            out.append("</ol>")
            in_ol = False

    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        if in_code:
            if stripped.startswith("```"):
                out.append(
                    '<pre><code class="lang-%s">%s</code></pre>'
                    % (_esc(code_lang), _esc("\n".join(code_buf)))
                )
                in_code = False
                code_buf = []
                code_lang = ""
            else:
                code_buf.append(line)
            i += 1
            continue

        if stripped.startswith("```"):
            close_lists()
            in_code = True
            code_lang = stripped[3:].strip()
            i += 1
            continue

        if not stripped:
            close_lists()
            i += 1
            continue

        if stripped.startswith("#"):
            close_lists()
            n = 0
            while n < len(stripped) and stripped[n] == "#":
                n += 1
            level = min(n, 6)
            out.append(
                "<h%d>%s</h%d>"
                % (level, _inline_md(stripped[level:].strip()), level)
            )
            i += 1
            continue

        if stripped.startswith(">"):
            close_lists()
            out.append("<blockquote>%s</blockquote>" % _inline_md(stripped[1:].strip()))
            i += 1
            continue

        if stripped.startswith("- "):
            if not in_ul:
                close_lists()
                out.append("<ul>")
                in_ul = True
            out.append("<li>%s</li>" % _inline_md(stripped[2:]))
            i += 1
            continue

        digits = 0
        while digits < len(stripped) and stripped[digits].isdigit():
            digits += 1
        ordered = (1 <= digits <= 2 and digits < len(stripped)
                   and stripped[digits] == "." and stripped[digits + 1] == " ")
        if ordered:
            if not in_ol:
                close_lists()
                out.append("<ol>")
                in_ol = True
            out.append("<li>%s</li>" % _inline_md(stripped[digits + 2:]))
            i += 1
            continue

        close_lists()
        out.append("<p>%s</p>" % _inline_md(stripped))
        i += 1

    if in_code:
        out.append('<pre><code>%s</code></pre>' % _esc("\n".join(code_buf)))
    close_lists()
    return "".join(out)
