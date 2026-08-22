"""Layer 2 — what a result can BECOME. One library, named, never re-implemented.

    from tapioca import outputs

    outputs.csv_file("areas", ["Zone", "m2"], rows)     # -> output\areas.csv
    outputs.capture("view")                             # -> output\view.png
    outputs.bake(ctx.plan)                              # one transaction, one undo
    outputs.to_worksheet("Checked zones")

`evp.paths` settled WHERE output goes. This module settles WHAT a result can turn
into, because that is the other half of the same problem: `ElementInfoPanel`
hand-rolls a CSV, `MassingFeasibility` hand-rolls a PDF, three commands each
hand-roll the capture-then-fetch dance, and every one of them re-decides the
encoding, the filename and the failure behaviour. A sink is written once here.

EVERY SINK RETURNS AN `Artifact`, never a bare path — so a command can say what it
produced without inventing a record shape, and the palette can offer the same
"where did that go" affordance for all of them.

⚠️ THIS IS A FACADE, NOT NEW CAPABILITY. Every sink routes to something already
proven: `evp.paths` for the buckets, `Tapioca.CaptureScreenshot` for images,
`evp.drawings` / `evp.layouts` for the navigator, `Plan.commit` for the writes,
`evp.properties` / `evp.elements` for the edits. If a sink here starts growing its
own Archicad logic, it belongs in the Layer 2 module for that domain instead.

⚠️ IMPORTABLE WITH NOTHING INSTALLED. `import evp` runs in processes with no
pydantic and no reportlab, so the only imports at module scope are stdlib and
sibling `evp` modules. reportlab is imported inside `pdf()`.
"""

from __future__ import annotations

import csv as _csv
import json as _json
import os
import time as _time
import urllib.request as _request

from . import paths

__all__ = ["Artifact", "OutputError", "STANDARD_ACTIONS",
           "csv_file", "json_file", "text_file", "image", "capture", "diligent_capture", "pdf",
           "to_layout", "to_worksheet", "to_3d_document",
           "bake", "set_properties", "set_geometry",
           "run_action", "table_of"]


class OutputError(RuntimeError):
    """A sink could not produce what it was asked for, and says which and why."""


class Artifact:
    """One thing a run produced: a file on disk, or elements in the model.

    Deliberately NOT a pydantic model — this module has to import in processes
    with no pydantic (see the header). A command that wants an artifact inside a
    declared Outputs model converts with `as_dict()`.
    """

    __slots__ = ("kind", "label", "path", "guids", "size", "note")

    def __init__(self, kind, label, path=None, guids=(), size=0, note=""):
        #: "csv" | "json" | "text" | "png" | "pdf" | "layout" | "worksheet" |
        #: "3d-document" | "elements" | "properties" | "geometry"
        self.kind = kind
        #: What to show the user. One line, no path — the path is `path`.
        self.label = label
        self.path = path
        self.guids = tuple(guids)
        #: Bytes on disk, or the number of elements touched. Whichever the kind
        #: makes meaningful; both are "how much did this produce".
        self.size = size
        self.note = note

    def __repr__(self):
        where = self.path if self.path else "%d element(s)" % len(self.guids)
        return "<Artifact %s %s>" % (self.kind, where)

    def as_dict(self):
        return {"kind": self.kind, "label": self.label, "path": self.path,
                "guids": list(self.guids), "size": self.size, "note": self.note}


# ---------------------------------------------------------------------------
# Files
# ---------------------------------------------------------------------------

def csv_file(name, headers, rows):
    """Write `output\\<name>.csv`. Latest wins — a second run overwrites it.

    ⚠️ utf-8-SIG, not utf-8. Excel opens a plain-UTF-8 CSV as the system codepage
    and turns every non-ASCII name into mojibake; the BOM is what makes it read
    the file as Unicode. Every CSV this project has shipped learned that
    separately, which is the reason this function exists.
    """
    path = paths.output_path(_with_ext(name, ".csv"))
    with open(path, "w", newline="", encoding="utf-8-sig") as handle:
        writer = _csv.writer(handle)
        if headers:
            writer.writerow(list(headers))
        for row in rows:
            writer.writerow(list(row))
    return _file_artifact("csv", path, "%d row(s)" % len(list(rows) or []))


def json_file(name, obj, indent=2):
    """Write `output\\<name>.json`, UTF-8, non-ASCII kept as itself."""
    path = paths.output_path(_with_ext(name, ".json"))
    with open(path, "w", encoding="utf-8") as handle:
        _json.dump(obj, handle, ensure_ascii=False, indent=indent)
    return _file_artifact("json", path)


def text_file(name, body):
    """Write `output\\<name>.txt`. `body` is a string or a sequence of lines.

    NOT a log: this is the current artifact, overwritten each run. A running
    commentary the user follows belongs in `evp.paths.append_log`, which rotates.
    """
    text = body if isinstance(body, str) else "\n".join(str(line) for line in body)
    path = paths.output_path(_with_ext(name, ".txt"))
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    return _file_artifact("text", path)


def image(name, png_bytes):
    """Write PNG bytes to `output\\<name>.png`."""
    path = paths.output_path(_with_ext(name, ".png"))
    with open(path, "wb") as handle:
        handle.write(png_bytes)
    return _file_artifact("png", path)


def capture(name, view="current", save=True):
    """Capture a window and return its PNG — `view` is "current" or "top".

    Returns `(artifact, png_bytes)`. `save=False` skips the file and returns
    `(None, png_bytes)`, for a caller that only wants the pixels (a PDF page, an
    HTML data URI).

    ⚠️ TWO STEPS, AND THE SECOND ONE IS HTTP. The native command renders the view
    and publishes the PNG to the loopback data plane; the bytes come back over
    that URL because a JSON envelope cannot carry them. That means THE SERVER MUST
    BE RUNNING — a capture with the server stopped fails at the fetch, not at the
    render, which reads like a rendering problem and is not. The refusal says so.
    """
    from .api import call  # deferred: `import evp` must not need a transport

    result = call("Tapioca.CaptureScreenshot", {"view": view}, raise_on_error=False)
    if not result.ok:
        raise OutputError("capture(%r) failed: %s"
                          % (view, (result.error or {}).get("message") or "no reason given"))
    url = (result.data or {}).get("url")
    if not url:
        raise OutputError("capture(%r) rendered but returned no data-plane url" % view)
    try:
        with _request.urlopen(url) as response:  # 127.0.0.1, loopback only
            png = response.read()
    except Exception as exc:  # the reason matters more than the type
        raise OutputError(
            "capture(%r) rendered, but fetching the bytes from %s failed: %s. The "
            "image travels over the loopback data plane, so the Tapioca server has "
            "to be running." % (view, url, exc)) from exc

    return (image(name, png) if save else None), png


def diligent_capture(name, camera, width, height, render_quality="realistic",
                      save=True, timeout=300.0, poll_interval=0.1):
    """Render a fixed-size Diligent frame and return ``(artifact, png_bytes)``.

    Native extraction and rendering are asynchronous. This wrapper polls their
    non-blocking state command and downloads the completed PNG from loopback.
    """
    from .api import call

    started = call("Tapioca.StartDiligentCapture", {
        "width": width,
        "height": height,
        "renderQuality": render_quality,
        "camera": dict(camera),
    }, raise_on_error=False)
    if not started.ok:
        raise OutputError("Diligent capture did not start: %s" % _result_error(started))

    capture_id = (started.data or {}).get("id")
    deadline = _time.monotonic() + float(timeout)
    while True:
        state_result = call("Tapioca.DiligentCaptureState", {"id": capture_id},
                            raise_on_error=False)
        if not state_result.ok:
            raise OutputError("Diligent capture state failed: %s" % _result_error(state_result))
        state = state_result.data or {}
        status = state.get("status")
        if status == "completed":
            url = state.get("url")
            try:
                with _request.urlopen(url) as response:
                    png = response.read()
            except Exception as exc:
                raise OutputError("Diligent capture completed, but fetching %s failed: %s"
                                  % (url, exc)) from exc
            return (image(name, png) if save else None), png
        if status in ("failed", "cancelled"):
            raise OutputError("Diligent capture %s during %s: %s"
                              % (status, state.get("stage"),
                                 state.get("failureMessage") or "no reason given"))
        if _time.monotonic() >= deadline:
            call("Tapioca.CancelDiligentCapture", {"id": capture_id}, raise_on_error=False)
            raise OutputError("Diligent capture timed out after %.1f seconds" % float(timeout))
        _time.sleep(float(poll_interval))


def _result_error(result):
    return (result.error or {}).get("message") or "no reason given"


def pdf(name, title, headers=(), rows=(), images=(), footer="", landscape=False):
    """A plain report page: title, an optional table, optional images, a footer.

    Deliberately ordinary. A DESIGNED page — a specific layout with the pictures
    in specific boxes — is a command's own module, not this: see
    `Commands/MassingFeasibility/pdfreport.py`, which keeps its own layout and
    borrows only the font and image helpers below.

    `images` is a sequence of PNG byte strings. reportlab is imported here rather
    than at module scope so this file stays importable without it; a command that
    calls this declares `requires=["reportlab==..."]`.
    """
    try:
        from reportlab.lib.pagesizes import A4
        from reportlab.lib.pagesizes import landscape as _landscape
        from reportlab.pdfgen import canvas as _canvas
    except ImportError as exc:  # pragma: no cover - depends on the runtime
        raise OutputError(
            "outputs.pdf needs reportlab, which is not in the runtime baseline. "
            "Declare it on the command: requires=[\"reportlab==4.2.5\"]. "
            "Original error: %s" % exc) from exc

    path = paths.output_path(_with_ext(name, ".pdf"))
    size = _landscape(A4) if landscape else A4
    width, height = size
    face, bold = register_pdf_font()

    pdf_canvas = _canvas.Canvas(path, pagesize=size)
    y = height - 40
    pdf_canvas.setFont(bold, 14)
    pdf_canvas.drawString(40, y, str(title))
    y -= 24

    if headers:
        pdf_canvas.setFont(bold, 9)
        y = _pdf_row(pdf_canvas, headers, 40, y, width)
    pdf_canvas.setFont(face, 9)
    for row in rows:
        if y < 80:
            pdf_canvas.showPage()
            pdf_canvas.setFont(face, 9)
            y = height - 40
        y = _pdf_row(pdf_canvas, row, 40, y, width)

    for png in images:
        pdf_canvas.showPage()
        draw_pdf_image(pdf_canvas, png, 40, 60, width - 80, height - 120)

    if footer:
        pdf_canvas.setFont(face, 7)
        pdf_canvas.setFillGray(0.55)
        pdf_canvas.drawRightString(width - 40, 28, str(footer))

    pdf_canvas.save()
    return _file_artifact("pdf", path)


# ---------------------------------------------------------------------------
# Into the project
# ---------------------------------------------------------------------------

def to_layout(view_name, layout_name=None, x=0.0, y=0.0, **kwargs):
    """Place a saved VIEW as a Drawing on a LAYOUT. Names as the Navigator shows them.

    ⚠️ LAYOUTS ONLY — Archicad's rule, not ours: a Drawing sourced from a project
    view cannot go on a worksheet. `evp.drawings.place_from_view` carries the full
    explanation and refuses with it; this is the one-line front door.
    """
    from . import drawings

    result = drawings.place_from_view(view_name=view_name, layout_name=layout_name,
                                      x=x, y=y, **kwargs)
    guid = (result or {}).get("guid")
    return Artifact("layout", "%s on %s" % (view_name, layout_name or "the layout"),
                    guids=[guid] if guid else (), size=1 if guid else 0)


def to_worksheet(name, ref=None):
    """Create a Worksheet database and return it as an Artifact."""
    from . import layouts

    made = layouts.create_worksheet(name=name, ref=ref) or {}
    return Artifact("worksheet", name, guids=_guids_of(made), size=1)


def to_3d_document(name, ref=None):
    """Create a 3D Document database. No other route exists — see evp.layouts."""
    from . import layouts

    made = layouts.create_3d_document(name=name, ref=ref) or {}
    return Artifact("3d-document", name, guids=_guids_of(made), size=1)


def bake(plan, name=None):
    """Commit a Plan: every spec in ONE transaction, one undo step.

    This is what "bake" means here — turn the previewed change into real elements.
    It is `Plan.commit()` with the result flattened into an Artifact, so a palette
    button and a command call produce the same record.
    """
    results = plan.commit(name)
    guids = []
    for result in results:
        for record in ((result or {}).get("results") or []):
            guid = ((record or {}).get("elementId") or {}).get("guid")
            if guid:
                guids.append(guid)
        # A single-element verb answers one level shallower than a batch one.
        direct = ((result or {}).get("elementId") or {}).get("guid")
        if direct:
            guids.append(direct)
    return Artifact("elements", "%s: %d element(s)" % (plan.name, len(plan)),
                    guids=guids, size=len(plan))


def set_properties(entries):
    """Write property values — one API call, therefore one undo step."""
    from . import properties

    properties.set_values(entries)
    guids = sorted({str(entry[0]) for entry in entries})
    return Artifact("properties", "%d value(s) on %d element(s)"
                    % (len(entries), len(guids)), guids=guids, size=len(entries))


def set_geometry(edits, tx=None):
    """Write scalar element settings. SPARSE — only the keys you changed.

    Echoing back a whole record from `evp.elements.details()` is refused by name;
    that refusal lives in `evp.elements.set_details` and is worth reading before
    calling this.
    """
    from . import elements

    elements.set_details(edits, tx=tx)
    guids = list(edits.keys()) if isinstance(edits, dict) else [g for g, _ in edits]
    return Artifact("geometry", "%d element(s) edited" % len(guids),
                    guids=[str(g) for g in guids], size=len(guids))


# ---------------------------------------------------------------------------
# The standard set — what a command can name instead of implementing
# ---------------------------------------------------------------------------

#: name -> what the framework must find before the action can run.
#:
#: THE BUTTON TEXT IS NOT HERE. It lives in `evp._scanner.STANDARD_ACTION_LABELS`,
#: because the palette has to read a button's text WITHOUT executing anything and
#: `_scanner` is loaded standalone by path in several places — it cannot import a
#: sibling. So the scanner owns what is displayed and this owns what is done, one
#: source for each fact. `test_outputs` pins that the two name sets agree.
#:
#:   "table"  a field on the Outputs model marked role="table" (see schema.output)
#:   "plan"   the run's Plan, reloaded from the run store
#:   "view"   the 3D/plan window as it stands; nothing from the command
#:
#: A command opts in by NAMING these on the decorator (`actions=["csv", "bake"]`)
#: and writes no export code at all. Anything the standard set cannot express is
#: the command's own function — that is the flexible half, and it exists so this
#: table never has to grow to cover one command's special case.
STANDARD_ACTIONS = {
    "csv":       "table",
    "json":      "table",
    "text":      "table",
    "pdf":       "table",
    "png":       "view",
    "layout":    "view",
    "worksheet": "view",
    "bake":      "plan",
}


def table_of(outputs_obj, schema=None):
    """The (headers, rows) a standard action exports, or None.

    Finds the field marked `role="table"` via `tapioca.schema.output(...)` and
    flattens it. Three row shapes are accepted because all three occur honestly:
    a list of models, of dicts, or of sequences.

    `outputs_obj` may be the live model OR the plain dict the run store kept, in
    which case `schema` is the stored JSON Schema. The dict path is what an action
    BUTTON uses: it runs in a fresh process, and importing the command to recover
    the class would be the re-run the store exists to avoid.

    Returns None when nothing declares a table — which is not an error, it is how
    the framework knows to grey the CSV button out rather than offer one that
    would write an empty file.
    """
    if outputs_obj is None:
        return None
    if schema is None:
        schema_of = getattr(type(outputs_obj), "model_json_schema", None)
        if schema_of is None:
            return None
        schema = schema_of()
    properties_ = (schema or {}).get("properties") or {}

    field = None
    for name, node in properties_.items():
        if (node.get("x-output") or {}).get("role") == "table":
            field = name
            break
    if field is None:
        return None

    items = (outputs_obj.get(field) if isinstance(outputs_obj, dict)
             else getattr(outputs_obj, field, None)) or []
    if not items:
        return [], []

    first = items[0]
    if hasattr(first, "model_dump"):
        headers = list(first.model_dump().keys())
        return headers, [[row.model_dump().get(h, "") for h in headers] for row in items]
    if isinstance(first, dict):
        headers = list(first.keys())
        return headers, [[row.get(h, "") for h in headers] for row in items]
    return [], [list(row) for row in items]


def run_action(action, name, outputs_obj=None, plan=None, schema=None):
    """Execute one STANDARD_ACTIONS entry. The palette's action bar calls this.

    `name` is the artifact's base filename — the command's folder, so two commands
    exporting a CSV do not overwrite each other's.

    A command's OWN action never comes here; it is called directly. This function
    exists so that a NAMED action behaves identically everywhere, which is the
    whole point of standardising them.
    """
    if action not in STANDARD_ACTIONS:
        raise OutputError(
            "%r is not a standard action. Known: %s. A command-specific export is "
            "its own function, not a name here."
            % (action, ", ".join(sorted(STANDARD_ACTIONS))))

    needs = STANDARD_ACTIONS[action]

    if needs == "table":
        table = table_of(outputs_obj, schema)
        if table is None:
            raise OutputError(
                "%r needs a table, and %s declares no field with role=\"table\". "
                "Mark one: Field(json_schema_extra=output(role=\"table\"))."
                % (action, _describe(outputs_obj)))
        headers, rows = table
        if action == "csv":
            return csv_file(name, headers, rows)
        if action == "json":
            return json_file(name, [dict(zip(headers, row, strict=False)) for row in rows])
        if action == "text":
            lines = ["\t".join(str(cell) for cell in headers)] if headers else []
            lines += ["\t".join(str(cell) for cell in row) for row in rows]
            return text_file(name, lines)
        return pdf(name, name, headers, rows)

    if needs == "plan":
        if plan is None:
            raise OutputError(
                "bake needs the run's Plan and there is none. A command that "
                "declares actions=[\"bake\"] must declare plan= as well.")
        return bake(plan)

    if action == "png":
        artifact, _png = capture(name)
        return artifact
    raise OutputError(
        "%r has no framework implementation: it needs a view name and a layout "
        "name that only the command knows. Call outputs.%s(...) directly."
        % (action, "to_layout" if action == "layout" else "to_worksheet"))


# ---------------------------------------------------------------------------
# Shared PDF helpers — used here AND by a command's own designed page
# ---------------------------------------------------------------------------

def register_pdf_font():
    """Register a Unicode TTF with reportlab and return `(regular, bold)` names.

    ⚠️ `Commands/MassingFeasibility/pdfreport.py` still carries its own copy of
    this and of the image box. They were lifted FROM it, and folding it back onto
    these is a two-line change — but that command is the repository's focus task
    and is awaiting in-Archicad verification, so a refactor landing in it now
    would make a failed verification ambiguous. Fold it in once it has passed.

    ⚠️ BEST EFFORT, AND THAT IS DELIBERATE. Without a Unicode face reportlab's
    built-in Helvetica drops Latin Extended-A, so Lithuanian diacritics vanish
    from an otherwise correct report. Falling back to Helvetica keeps the report
    readable rather than refusing to draw it — a missing accent is a smaller
    failure than a missing page. A family whose REGULAR exists but whose bold
    does not is accepted too, reusing the regular for both: a heading in the
    right script beats a heading in the right weight.
    """
    from reportlab.pdfbase import pdfmetrics
    from reportlab.pdfbase.ttfonts import TTFont

    fonts_dir = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
    for regular, bold, regular_file, bold_file in (
            ("Arial", "Arial-Bold", "arial.ttf", "arialbd.ttf"),
            ("Segoe", "Segoe-Bold", "segoeui.ttf", "segoeuib.ttf"),
            ("DejaVu", "DejaVu-Bold", "DejaVuSans.ttf", "DejaVuSans-Bold.ttf")):
        regular_path = os.path.join(fonts_dir, regular_file)
        bold_path = os.path.join(fonts_dir, bold_file)
        if not os.path.isfile(regular_path):
            continue
        try:
            pdfmetrics.registerFont(TTFont(regular, regular_path))
            if os.path.isfile(bold_path):
                pdfmetrics.registerFont(TTFont(bold, bold_path))
                return regular, bold
            return regular, regular
        except Exception:  # try the next candidate, then give up
            continue
    return "Helvetica", "Helvetica-Bold"


def draw_pdf_image(pdf_canvas, png_bytes, x, y_bottom, width, height, pad=0.0):
    """Draw PNG bytes into a box, preserving aspect and centring what is left over.

    `pad` insets the picture from the box, for a page that wants air around it.
    Empty bytes draw nothing rather than raising — a report missing one capture
    is still a report.
    """
    import io

    from reportlab.lib.utils import ImageReader

    if not png_bytes:
        return
    reader = ImageReader(io.BytesIO(png_bytes))
    source_width, source_height = reader.getSize()
    if source_width <= 0 or source_height <= 0:
        return
    scale = min((width - 2 * pad) / float(source_width),
                (height - 2 * pad) / float(source_height))
    drawn_width, drawn_height = source_width * scale, source_height * scale
    pdf_canvas.drawImage(reader,
                         x + (width - drawn_width) / 2.0,
                         y_bottom + (height - drawn_height) / 2.0,
                         width=drawn_width, height=drawn_height,
                         preserveAspectRatio=True, mask="auto")


# ---------------------------------------------------------------------------

def _describe(outputs_obj):
    """What to call the result in a refusal. A stored dict has no class name worth
    printing, so it is named by what it is."""
    if outputs_obj is None:
        return "the result"
    if isinstance(outputs_obj, dict):
        return "the stored result"
    return type(outputs_obj).__name__


def _with_ext(name, ext):
    """`name` may already carry its extension; adding a second one is the bug this
    prevents (`areas.csv.csv`)."""
    return name if name.lower().endswith(ext) else name + ext


def _file_artifact(kind, path, note=""):
    size = os.path.getsize(path) if os.path.isfile(path) else 0
    return Artifact(kind, os.path.basename(path), path=path, size=size, note=note)


def _guids_of(made):
    guid = made.get("guid") if isinstance(made, dict) else None
    return [guid] if guid else ()


def _pdf_row(pdf_canvas, cells, x, y, page_width):
    """One table row at a fixed column pitch, clipped to the page."""
    columns = max(1, len(list(cells)))
    pitch = (page_width - 80.0) / columns
    for index, cell in enumerate(cells):
        pdf_canvas.drawString(x + index * pitch, y, str(cell)[:40])
    return y - 14
