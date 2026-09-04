"""Run a command folder end-to-end offline, with only the WIRE faked.

WHY THIS EXISTS. `py_compile` proves a file parses and the AST scanner proves the
palette can read its signature — and both passed on a probe that then died at
runtime on `evp.transaction()` (the factory takes a required `name`) and would
have died again on `handle.value` (Handle has no such attribute; `handle.<x>`
silently yields a Ref for a later step, so it fails somewhere else entirely).
Neither check could catch either, because both are ordinary attribute/signature
errors that only exist once the code RUNS. Archicad cannot be run here, so the
gap was shipped to the user twice.

So: import the REAL `evp` package, monkeypatch the one function that touches
Archicad — `evp.api._transport` — and call `run()` for real. Everything above the
wire is genuine: transaction recording and commit, Handle/Ref semantics, every
Layer 2 wrapper, and the command's own control flow. What it CANNOT tell you is
whether Archicad likes the parameters; that is what the in-Archicad probe is for.

    python AddOn/EvP/tests/dryrun_command.py AddOn/EvP/Commands/<CommandFolder>
    python AddOn/EvP/tests/dryrun_command.py AddOn/EvP/Commands/<CommandFolder> --twice

⚠️ USE `--twice` FOR ANYTHING THAT LEAVES AN ARTEFACT BEHIND, claims find-or-create,
or writes a NAMED thing. It runs the command a second time against the state the
first run left, without resetting the fake. A single run cannot see a whole class
of defect: LayoutApiProbe2 Q8 leaves a fixed-name 3D document for the user to look
at, passed every offline gate, and then died in Archicad on its SECOND run because
the name already existed. The collision exists only BETWEEN runs.

Exit code 0 = the command ran to completion. Non-zero = a real defect, printed
with its traceback. Unknown commands are reported at the end rather than faked
silently, so a typo'd command name cannot pass as working.
"""

import importlib.util
import binascii
import base64
import json
import math
import os
import re
import struct
import sys
import traceback
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "EvP", "Sources", "PyPackage"))

# Redirect every evp.paths write to a scratch root BEFORE evp is imported. A dry
# run otherwise appends to the very same %LOCALAPPDATA%\EvP\logs files a real
# Archicad run uses, with no marker separating them — that happened, and a
# debugging session was spent reading this harness's fake guids out of what looked
# like a live log. Keep this first.
os.environ["EVP_HOME"] = os.path.join(
    os.environ.get("TEMP", os.path.join(REPO, "..")), "evp-dryrun")

import evp                                                    # noqa: E402
from evp import api as evp_api                                # noqa: E402

_GUID = "00000000-0000-0000-0000-%012d"

seen = []
unknown = set()

# Enough STATE to make a round-trip real. A stateless fake reports "wrote 3, read
# back 0 changed" and "size did not take" on a perfectly correct command, and a
# harness that cries wolf teaches you to ignore it — so writes are remembered and
# reads answer from what was written. This is what actually exercises a probe's
# verify-after-write logic, which is the logic most worth checking offline.
_ids = {}
_sections = {}
# {placed guid: the angle PlaceLibraryObject was given}, so GetElementInfo reads
# back what was really asked for rather than a constant 0.
_placed_angles = {}
_selection_sets = {"Targets": [], "Operators": []}
# E25: the change token this run has handed out, and whether WatchModel was armed.
# ArchViz viewer state, shared by the three EvP.*Viewer* verbs.
_archviz = {"open": False, "polls": 0, "navlog": False, "navInterval": 50,
            "live": False, "passes": 0,
            # The extraction pass (ArchViz/ExtractionThread.hpp), as seen from
            # outside: it RUNS FOR A WHILE. A fake that answered "done" on the
            # first query would let a probe that never polls pass offline.
            "extractPolls": 0, "extracted": False}

# The Diligent probes (PLAT-RE19 device, PLAT-RE22 viewport) keep their own
# state: they never coexist with the bgfx viewer, and the palette refuses to
# start one while the other runs.
_diligent = {"open": False, "polls": 0, "deviceAttempts": 0, "devicePolls": 0,
             "debugView": 0,
             # PLAT-RE36, the overlay camera sync. `camera` is what was last
             # PUSHED and is echoed straight back by DiligentViewportState, so
             # the residual the test computes is zero offline. `cameraTick`
             # drives a slow orbit in GetArchicad3DCamera so the test's
             # "the camera moved" branch is actually reached.
             "camera": [0.0] * 6, "syncs": 0, "cameraTick": 0, "cameraSync": False,
             # PLAT-RE81, the sync MODE switch, and PLAT-RE73's nav log. The mode
             # is stateful because a probe is required to restore what it found,
             # and a constant would let a probe that restores the wrong value —
             # or nothing at all — dry-run clean.
             "syncMode": "legacy", "syncIntervalMs": 33,
             "navLog": False, "navRows": 0, "navMarks": 0,
             # PLAT-RE37/RE60, the OVERLAY. `overlay` is what the viewport came
             # up ON, and it is reported rather than inferred for the same reason
             # the real one is: "the Diligent viewport is running" is true of
             # both surfaces, and a probe asking why the picture is not over the
             # 3D window has no other way to tell which it got.
             "overlay": False, "renderMode": 0, "callout": False,
             # The sun override (PLAT-RE58). Stateful, so a probe's A/B really
             # reads back what it pushed instead of a constant.
             "sunOverride": False, "sunOverrideAz": 135.0, "sunOverrideAlt": 45.0}

# PLAT-RE52 capture state is separate from the visible viewer, matching the real
# one-consumer lifecycle: each start expires the previous capture URL.
_diligent_capture = {"id": 0, "polls": 0, "width": 0, "height": 0,
                     "cancelled": False, "png": b""}


def _png_chunk(kind, data):
    crc = binascii.crc32(kind + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", crc)


def _fake_capture_png(width, height):
    row = b"\x00" + bytes((39, 121, 203, 255)) * width
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", header) +
            _png_chunk(b"IDAT", zlib.compress(row * height)) +
            _png_chunk(b"IEND", b""))


class _LoopbackCaptureResponse:
    def __init__(self, body):
        self.body = body

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def read(self):
        return self.body


_real_urlopen = urllib.request.urlopen


def _fake_loopback_urlopen(url, *_args, **_kwargs):
    parsed = urllib.parse.urlparse(url)
    if parsed.path != "/screenshot/diligent":
        return _real_urlopen(url, *_args, **_kwargs)
    requested = int((urllib.parse.parse_qs(parsed.query).get("id") or [0])[0])
    if requested != _diligent_capture["id"] or not _diligent_capture["png"]:
        raise urllib.error.HTTPError(url, 409, "Diligent capture id is unavailable", {}, None)
    return _LoopbackCaptureResponse(_diligent_capture["png"])


urllib.request.urlopen = _fake_loopback_urlopen

# RE51.D1 is a separate one-shot D3D12 device and presentation probe. Keep its
# state separate so the dry run also proves it never changes the D3D11 viewer.
_d3d12 = {"attempted": False, "running": False, "polls": 0}

# Which offline scenario this run is playing; see each use below.
_CANCEL_AFTER = int(os.environ.get("EVP_DRYRUN_CANCEL_AFTER", "0") or 0)

# ⚠️ time.sleep IS A NO-OP OFFLINE, AND THAT IS NOT AN OPTIMISATION. A probe that
# walks a user through gestures sleeps for MINUTES by design -- CameraSyncMatrix
# alone is about fifty seconds -- so a test that runs it a dozen times to cover
# its branches costs ten minutes and simply does not get run. Nothing offline is
# waiting on wall-clock time: the fake advances its state on POLL COUNT, so a
# loop that slept and one that did not see exactly the same sequence. Set
# EVP_DRYRUN_REAL_SLEEP=1 for the rare case where a real delay is the subject.
if os.environ.get("EVP_DRYRUN_REAL_SLEEP", "") != "1":
    time.sleep = lambda _seconds: None
_SCENARIO = os.environ.get("EVP_DRYRUN_SCENARIO", "")

# ⚠️ EVP_DRYRUN_SCENARIO=diligent_running STARTS THE VIEWPORT ALREADY OPEN, and
# the overlay sync test needs it. That command is a SECOND command run against a
# viewport a FIRST one left running, so on its own it always takes the "not
# running" early return -- which is correct behaviour and exercises none of the
# measurement loop it exists for.
if _SCENARIO == "diligent_running":
    _diligent["open"] = True
    _diligent["polls"] = 10

# ⚠️ EVP_DRYRUN_SCENARIO=diligent_plan MAKES THE OVERLAY REPORT THE *PLAN*
# CAMERA. The plan overlay probe branches on `cameraSource` -- "was the
# orthographic plan camera adopted, or is the 3D window's perspective one still
# in use" -- and that branch is the whole point of the command, so it needs a
# scenario in which the answer is yes. Without one, only the "not adopted"
# diagnosis is ever exercised offline.
# ⚠️ EVERY hookdraw SCENARIO IMPLIES THE PLAN CAMERA. The scenario is one
# string, and a hookdraw probe checks the tracked window before it ever
# reaches the mode -- so without this each failure scenario stopped nine bus
# calls in, at the window check, having tested nothing it was written for.
_DILIGENT_PLAN = _SCENARIO == "diligent_plan" or _SCENARIO.startswith("hookdraw")

# The hookdraw failure branches, each reachable on its own. They exist because
# every one of them HAS happened in Archicad: the mode refusing to arm, the swap
# chain never being identified, and the compositor never becoming ready.
_HOOKDRAW_FAILS = _SCENARIO == "hookdraw_fails"
_COMPOSITE_FAILS_TARGET = _SCENARIO == "hookdraw_no_target"
_COMPOSITE_FAILS_READY = _SCENARIO == "hookdraw_not_ready"

# The 3D window's camera, MUTABLE — see EvP.Set3DProjection below. The unit trap
# is deliberate: `viewCone` is a HORIZONTAL field of view in DEGREES, and a
# consumer that reads it as vertical or as radians still produces a
# plausible-looking frame.
_projection = {"isPersp": True,
               "posX": 24.0, "posY": -18.0, "cameraZ": 12.0,
               "targetX": 4.0, "targetY": 1.5, "targetZ": 1.5,
               "azimuth": 135.0, "rollAngle": 0.0,
               "viewCone": 60.0, "distance": 26.9,
               "isTwoPointPersp": False}

_change = {"token": 0, "watching": False, "arming": False, "drained": False}
# {guid: {field: value}} — what EvP.SetElementDetails has written this run, so the
# GetElementDetails answer below reports it back. That is what makes a
# read -> change -> write -> read-again pass (a pull-to-mesh verify) real offline.
_written_details = {}
# The native writable table, mirrored from
# NativeCommands/ElementModifyCommands.cpp -> WritableFields. Mirrored rather than
# imported because the point is to fail HERE the way the add-on fails: sending a
# whole details record back (the sparse-contract mistake) must be rejected offline,
# not discovered in Archicad.
_WRITABLE_DETAILS = {
    "slab":   {"level", "thickness"},
    "roof":   {"level", "thickness", "slantAngle"},
    "mesh":   {"level", "skirtLevel"},
    "wall":   {"level", "thickness", "height"},
    "beam":   {"level"},
    "column": {"level", "height", "planAngle"},
    "object": {"level", "planAngle", "xRatio", "yRatio", "reflected"},
    "lamp":   {"level", "planAngle", "xRatio", "yRatio", "reflected"},
    "polyline": set(),
    "fill":     set(),
}
# Keep MakeCommand's structural set in sync with CommandBase.hpp's
# StructuralCommand subclasses — see the refusal in transport().
_STRUCTURAL = frozenset({
    "EvP.CreateDatabase", "EvP.DeleteDatabase", "EvP.SetDocumentFrom3DSettings",
})
# Monotonic, so two databases created in one run never share a guid the way a
# len()-derived counter does once something is deleted between them.
_db_serial = [600]
# {drawing guid: point count} — so a probe that places a crop and reads it back
# is comparing its OWN numbers, not this fake's hardcoded rectangle.
_clips = {}
# {mesh guid: the record GetElementDetails will report} — a mesh created in this
# run reads back the Z frame it was ACTUALLY given, so a command that verifies
# its own placement is testing its comparison rather than this fake's guess.
_meshes = {}
# {db_type: {guid: name}} — seeded with the two things a project always has, so
# a probe can find a layout and a master to work against without creating one.
_databases = {
    "layout": {_GUID % 901: "A-101 Plans"},
    "masterLayout": {_GUID % 900: "A4 Portrait"},
    "worksheet": {_GUID % 902: "Worksheet"},
    "detail": {},
    "3dDocument": {},
}
# The Public View Map, flattened. A FOLDER is included on purpose: `placeableOnly`
# has to actually filter something, and "I named a folder" is a real user error.
# Two entries share the name "Perspective" ON PURPOSE — that is the real project's
# shape (a 3D Document and a saved 3D view can both be called that), and it is what
# makes name-based lookup unusable. A command that resolves by name must fail here.
_views = [
    {"name": "Ground Floor", "guid": _GUID % 910, "path": "", "itemType": "story",
     "placeable": True},
    {"name": "Elevations", "guid": _GUID % 911, "path": "", "itemType": "folder",
     "placeable": False},
    {"name": "E-01 North", "guid": _GUID % 912, "path": "Elevations",
     "itemType": "elevation", "placeable": True},
    {"name": "Perspective", "guid": _GUID % 913, "path": "", "itemType": "perspective",
     "placeable": True},
    {"name": "Perspective", "guid": _GUID % 914, "path": "", "itemType": "3dDocument",
     "placeable": True},
]


# ---------------------------------------------------------------------------
# The REAL input schemas, read out of the C++ registry tables (PLAT-RE96).
#
# ⚠️ WITHOUT THIS THE HARNESS PASSES CALLS ARCHICAD WILL REJECT, and it did so
# three times running in one afternoon -- each costing a build, a sync, a restart
# and a user round trip:
#   * `mode: "wakepredict"` -- not in SetCameraSyncMode's input enum;
#   * six fields added to CameraSyncModeState's response, none declared;
#   * `intervalMs: 0` -- below ViewerNavLog's stated minimum of 5.
# Every one was a mismatch between what a command SENDS and what the dispatcher
# ACCEPTS, and the harness could not see any of them because it faked the wire
# and never consulted a schema. `schema_check.py` closed the response half; this
# closes the input half, which is the half that actually stopped the runs.
#
# The schemas are parsed from the same registry entries the add-on registers, so
# there is no second copy to drift. A command whose schema cannot be parsed is
# simply not validated -- silence, never a fabricated failure.
#
# ⚠️ THAT SILENCE HAS ITS OWN COST, and it was paid on 2026-08-16. A registry entry
# may write its schema INLINE or name a `constexpr const char k…Input[]` constant,
# and reading only the inline form left 21 of 126 commands unvalidated -- among them
# GetModelElements and GetBodyGeometry, the two the ModelViewer reads geometry with.
# `evp.model.elements(guids=…)` sent a `guids` array the schema had never accepted;
# the harness passed it, and Archicad rejected it in front of the user. So the
# constants are resolved too, and `unvalidated_commands()` below makes the remaining
# blind spot something a test can assert on rather than something nobody can see.
_NATIVE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "Sources", "AddOn", "NativeCommands")
# The schema slot is either an inline R"json(...)json" or a bare identifier.
_REGISTRY_RE = re.compile(
    r'\{\s*"(?P<name>\w+)"\s*,\s*&MakeRegisteredNativeCommand<\w+>\s*,'
    r'\s*(?:true|false)\s*,\s*'
    r'(?:R"json\((?P<input>.*?)\)json"|(?P<inputRef>\w+))',
    re.DOTALL)
# constexpr const char kFooInput[] = R"json( … )json";
_SCHEMA_CONST_RE = re.compile(
    r'constexpr\s+const\s+char\s+(?P<const>\w+)\s*\[\s*\]\s*=\s*R"json\((?P<body>.*?)\)json"',
    re.DOTALL)
# Every native command the registry tables declare, whether or not its schema parsed.
_REGISTERED_RE = re.compile(r'\{\s*"(?P<name>\w+)"\s*,\s*&MakeRegisteredNativeCommand')

_registered_commands = set()


def _load_input_schemas():
    schemas = {}
    root = os.path.normpath(_NATIVE_DIR)
    if not os.path.isdir(root):
        return schemas
    for entry in sorted(os.listdir(root)):
        if not entry.endswith(".cpp"):
            continue
        text = open(os.path.join(root, entry), "r", encoding="utf-8", errors="replace").read()
        # Named schema constants first: a registry entry may reference one declared
        # anywhere in the same file, including further down than the table.
        constants = {}
        for match in _SCHEMA_CONST_RE.finditer(text):
            try:
                constants[match.group("const")] = json.loads(match.group("body"))
            except ValueError:
                pass
        for match in _REGISTERED_RE.finditer(text):
            _registered_commands.add(match.group("name"))
        for match in _REGISTRY_RE.finditer(text):
            inline = match.group("input")
            if inline is not None:
                try:
                    schemas[match.group("name")] = json.loads(inline)
                except ValueError:
                    pass   # unparsable -> unvalidated, never a false failure
            else:
                resolved = constants.get(match.group("inputRef"))
                if resolved is not None:
                    schemas[match.group("name")] = resolved
    return schemas


def unvalidated_commands():
    """Registered native commands whose input schema could not be read.

    Every name here is a call this harness will wave through no matter what
    parameters it carries. Kept small deliberately — see the warning above.
    """
    return sorted(name for name in _registered_commands if name not in _INPUT_SCHEMAS)


_INPUT_SCHEMAS = _load_input_schemas()


def _schema_violation(schema, params):
    """The dispatcher's first complaint about `params`, or None.

    Mirrors the subset of JSON Schema the registry tables actually use, and
    phrases the message the way the real validator does so a failure here reads
    identically to one from Archicad.
    """
    properties = schema.get("properties") or {}
    for name in schema.get("required") or []:
        if name not in params:
            return "$.%s: required property is missing" % name
    if schema.get("additionalProperties") is False:
        for name in params:
            if name not in properties:
                return "$.%s: additional property is not allowed" % name
    for name, value in params.items():
        rule = properties.get(name)
        if not isinstance(rule, dict):
            continue
        kind = rule.get("type")
        if kind == "integer" and isinstance(value, bool):
            return "$.%s: value is not an integer" % name
        if kind == "integer" and not isinstance(value, int):
            return "$.%s: value is not an integer" % name
        if kind == "number" and not isinstance(value, (int, float)):
            return "$.%s: value is not a number" % name
        if kind == "string" and not isinstance(value, str):
            return "$.%s: value is not a string" % name
        if kind == "boolean" and not isinstance(value, bool):
            return "$.%s: value is not a boolean" % name
        if "enum" in rule and value not in rule["enum"]:
            return "$.%s: value is not in enum" % name
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            if "minimum" in rule and value < rule["minimum"]:
                return "$.%s: value violates minimum" % name
            if "maximum" in rule and value > rule["maximum"]:
                return "$.%s: value violates maximum" % name
    return None


def _validate_input(command, params):
    """Reject exactly what the dispatcher would, before the canned response."""
    bare = command.split(".", 1)[-1]
    schema = _INPUT_SCHEMAS.get(bare)
    if schema is None:
        return None
    problem = _schema_violation(schema, params)
    if problem is None:
        return None
    return {"ok": False,
            "error": {"kind": "SchemaValidationFailed",
                      "message": "%s input schema violation: %s" % (command, problem),
                      "command": command},
            "meta": {"backend": "dryrun", "duration_ms": 0.1}}


# The native sun study is a SESSION, so its fake has to hold state between calls.
# See the canned responses below for why a stateless fake would pass a broken
# caller and hang a working one.
_SUN_STUDY = {}


def _ok(data):
    """Envelope a native command's response.

    `ok` goes in BOTH places on purpose, because that is what really happens: the
    bus envelope has its own `ok`, and every native command ALSO does
    os.Add("ok", true) inside its own ObjectState — which becomes `data`. A
    caller reading `data["ok"]` (as the transaction path must, since a Handle's
    result() is the data payload, not the envelope) depends on the inner one.
    Omitting it made this harness report a false failure on its first run.
    """
    body = {"ok": True}
    body.update(data)
    return {"ok": True, "data": body, "meta": {"backend": "dryrun", "duration_ms": 0.1}}


def _err(message, code="CommandFailed", detail=None):
    """Envelope a REFUSAL the way the real bus spells one.

    ⚠️ `error` IS AN OBJECT, NOT A STRING, and getting that wrong here fabricates
    a crash in whatever probe exercises a refusal. `evp.api.call` does
    `(result.error or {}).get("code")` on every failed call, so a bare string
    raises `AttributeError: 'str' object has no attribute 'get'` — inside the evp
    package, with a traceback that points at the probe's own `_call`. Thirteen
    fakes in this file were written with the string form, which is why the
    refusal branches they exist to model had apparently never been dry-run.

    The shape is `ApiDispatcher.cpp -> MakeError`: code, message, optional
    detail. `detail` is OMITTED rather than null when empty, because
    GS::ObjectState cannot hold a JSON null and the real envelope therefore has
    no such key.
    """
    error = {"code": code, "message": message}
    if detail:
        error["detail"] = detail
    return {"ok": False, "error": error, "meta": {"backend": "dryrun", "duration_ms": 0.1}}


def _v2(data):
    """Envelope a migrated command whose success payload has no transport fields."""
    return {"ok": True, "data": dict(data),
            "meta": {"backend": "dryrun", "duration_ms": 0.1}}


def _rect(cx, cy, w, h, deg=0.0):
    """A rectangle footprint as the wire spells one: [{x, y}, ...], CCW, optionally
    rotated about its own centre. Rotation is the point — an axis-aligned fake
    cannot tell a command that aligns text to an element apart from one that does
    not."""
    import math as _m
    t = _m.radians(deg)
    out = []
    for dx, dy in ((-w / 2, -h / 2), (w / 2, -h / 2), (w / 2, h / 2), (-w / 2, h / 2)):
        out.append({"x": cx + dx * _m.cos(t) - dy * _m.sin(t),
                    "y": cy + dx * _m.sin(t) + dy * _m.cos(t)})
    return out


# A SECOND SELECTION, opted into with EVP_DRYRUN_SCENARIO=polyline, because the
# default one (three identical columns) cannot exercise a command that sorts along
# a reference polyline, labels outlines and treats kinds differently — it bails at
# "no polyline in the selection" and reports success having run six lines.
#
# The default stays exactly as it was: every other command's dry run is unaffected.
# The shapes are deliberately awkward — a rotated slab, a closed polyline that must
# NOT be mistaken for the reference, a fill, a morph the details read cannot speak,
# and a wall that has no outline to label.
_SCENARIO = os.environ.get("EVP_DRYRUN_SCENARIO", "")

_POLY_GUIDS = {
    "open_polyline":   _GUID % 800,
    "slab_rotated":    _GUID % 801,
    "slab_axis":       _GUID % 802,
    "closed_polyline": _GUID % 803,
    "fill":            _GUID % 804,
    "morph":           _GUID % 805,
    "wall":            _GUID % 806,
}

_POLY_DETAILS = {
    _POLY_GUIDS["open_polyline"]: {
        "kind": "polyline", "typeName": "Polyline", "typeId": 20, "found": True,
        "details": {"polygonOutline": [{"x": 0.0, "y": 0.0}, {"x": 10.0, "y": 0.0},
                                       {"x": 20.0, "y": 4.0}],
                    "polygonArcs": [0.0, 0.0, 0.0], "closed": False, "pen": 3}},
    _POLY_GUIDS["slab_rotated"]: {
        "kind": "slab", "typeName": "Slab", "typeId": 8, "found": True,
        "details": {"thickness": 0.2, "level": 0.0, "hasHoles": False, "holes": [],
                    "polygonOutline": _rect(2.0, 3.0, 4.0, 2.0, 30.0),
                    "polygonArcs": [0.0] * 4}},
    _POLY_GUIDS["slab_axis"]: {
        "kind": "slab", "typeName": "Slab", "typeId": 8, "found": True,
        "details": {"thickness": 0.2, "level": 0.0, "hasHoles": False, "holes": [],
                    "polygonOutline": _rect(10.0, 3.0, 4.0, 2.0),
                    "polygonArcs": [0.0] * 4}},
    _POLY_GUIDS["closed_polyline"]: {
        "kind": "polyline", "typeName": "Polyline", "typeId": 20, "found": True,
        "details": {"polygonOutline": _rect(14.0, 3.0, 3.0, 1.0, 75.0),
                    "polygonArcs": [0.0] * 4, "closed": True, "pen": 4}},
    _POLY_GUIDS["fill"]: {
        "kind": "fill", "typeName": "Fill", "typeId": 18, "found": True,
        "details": {"polygonOutline": _rect(6.0, 3.0, 2.0, 1.0, -15.0),
                    "polygonArcs": [0.0] * 4, "hasHoles": False, "holes": [],
                    "pen": 5, "fillPen": 6, "fillBGPen": 0}},
    # A MISS with the morph's real numeric type — the only language-independent
    # way a command can recognise one, since GetElementDetails cannot read it.
    _POLY_GUIDS["morph"]: {
        "kind": "", "typeName": "Morph", "typeId": 45, "found": False,
        "reason": "unsupportedType", "details": {}},
    _POLY_GUIDS["wall"]: {
        "kind": "wall", "typeName": "Wall", "typeId": 1, "found": True,
        "details": {"thickness": 0.3, "height": 3.0, "level": 0.0,
                    "slantAngle": 1.5708,
                    "begCoordinate": {"x": 12.0, "y": -2.0, "z": 0.0},
                    "endCoordinate": {"x": 13.0, "y": -2.0, "z": 0.0}}},
}

# Plan centres for the bbox call, so a command that falls back to a bounding box
# gets an answer consistent with the outlines above.
_POLY_BBOX = {
    _POLY_GUIDS["morph"]: (18.0, 3.0, 1.5),
    _POLY_GUIDS["wall"]: (12.5, -2.0, 1.5),
}

# A THIRD SELECTION, opted into with EVP_DRYRUN_SCENARIO=walls, for the plan
# ANCHOR reads (PLAT-RE65). The default selection is three columns with a
# zero-length axis, so a command that measures an outline ACROSS a wall's axis
# can only reach its "not measurable" branch there — which is the branch least
# worth exercising.
#
# ⚠️ THE TWO WALLS MEET AT A CORNER AND WALL A IS MITRED, ON PURPOSE. A
# connection polygon that equals the untrimmed rectangle proves nothing: it is
# the DIFFERENCE that says ACAPI_Element_GetRelations was really consulted. And
# the difference a real corner makes is a MITRE, not a square cut — the live run
# on 2026-08-12 found wall 1990DBF2 with one face 200 mm SHORTER and the other
# 10 mm LONGER. So wall A's faces move in OPPOSITE directions here: a probe that
# reports one combined length cancels them out and reports nothing, and this
# fixture is what fails it. Wall B runs 4 m north from that corner, untrimmed.
#
# Both are 0.3 m thick with CENTRED reference lines, so the width across the
# axis is exactly `thickness` and a wrong reading shows up as a width error
# rather than hiding in a length.
#
# ⚠️ BOTH CARRY A MEMO POLYGON, because the live run measured that straight
# walls do (6 of 6) — ACAPinc.h's "required only for APIWtyp_Poly" governs what
# a CREATE must supply, not what a GET returns. An earlier version of this
# fixture said False on the strength of that remark, which is the same mistake
# as a fake mirroring its caller instead of the add-on.
# What the last SetPlanAnchors handed the viewer, so DiligentViewportState can
# answer from it rather than from a constant.
_plan_anchors = {"enabled": False, "vertices": 0, "widthPixels": 0.0}

_WALL_GUIDS = {"a": _GUID % 900, "b": _GUID % 901}

_WALL_DETAILS = {
    _WALL_GUIDS["a"]: {
        "typeName": "Wall", "typeId": 1,
        "details": {"thickness": 0.3, "height": 3.0, "level": 0.0,
                    "planAngle": 0.0, "slantAngle": 1.5708, "isSlanted": False,
                    "begCoordinate": {"x": 0.0, "y": 0.0, "z": 0.0},
                    "endCoordinate": {"x": 6.0, "y": 0.0, "z": 0.0}},
        # MITRED at wall B's corner: the near face is cut back to 5.8, the far
        # one runs on to 6.05. Four vertices still — the corner tilts the end
        # edge instead of adding one, which is why a vertex count is not a trim
        # test.
        "outline": [(0.0, -0.15), (5.8, -0.15), (6.05, 0.15), (0.0, 0.15)],
        "memo": [(0.0, -0.15), (6.0, -0.15), (6.0, 0.15), (0.0, 0.15)],
        "connected": {"atBegin": 0, "atEnd": 1, "toReferenceLine": 0,
                      "onReferenceLine": 0, "crossing": 0},
    },
    _WALL_GUIDS["b"]: {
        "typeName": "Wall", "typeId": 1,
        "details": {"thickness": 0.3, "height": 3.0, "level": 0.0,
                    "planAngle": 1.5708, "slantAngle": 1.5708, "isSlanted": False,
                    "begCoordinate": {"x": 5.95, "y": 0.0, "z": 0.0},
                    "endCoordinate": {"x": 5.95, "y": 4.0, "z": 0.0}},
        "outline": [(5.8, 0.0), (6.1, 0.0), (6.1, 4.0), (5.8, 4.0)],
        "memo": [(5.8, 0.0), (6.1, 0.0), (6.1, 4.0), (5.8, 4.0)],
        "connected": {"atBegin": 1, "atEnd": 0, "toReferenceLine": 0,
                      "onReferenceLine": 0, "crossing": 0},
    },
}

# A FOURTH SELECTION, opted into with EVP_DRYRUN_SCENARIO=roofs, for the slope
# reads. The default selection is three columns, so PlaceSlopeSymbols could only
# ever reach its "nothing here is a roof" early return offline — the branch least
# worth exercising, and the one that hid the whole property path.
#
# ⚠️ THE POINT OF THIS FIXTURE IS THAT SLOPE HAS TWO SOURCES. A single-plane roof
# STATES its pitch (slantAngle) and the pivot it is measured from (baseLine +
# posSign); a MULTI-PLANE roof reports slantAngle 0 because its pitch is per-plane,
# and can only be measured off the 3D mesh. A fixture of plane roofs alone would
# let a command that silently ignores roofClass pass, reading 0 as "flat".
#
# So: two plane roofs that DRAIN TO A SHARED LOW CORNER at the origin (they must
# cluster — the real data are inverted pyramids round a gully, and a fixture whose
# roofs never cluster cannot fail the clustering), one poly roof that forces the
# mesh fallback, and one plane roof that is genuinely dead flat.
#
# Roof A rises towards +Y and roof B towards -Y off the SAME pivot line, which is
# exactly the posSign flip: same baseLine, opposite posSign. A command that reads
# posSign against a fixed side of the world instead of against the pivot line's own
# direction puts both arrows the same way, and this pair is what fails it.
_ROOF_SLANT = math.atan(0.05)                                    # 5% drainage fall

_ROOF_GUIDS = {"a": _GUID % 910, "b": _GUID % 911, "flat": _GUID % 912}


def _plane_roof(outline, pos_sign, slant, level=0.0):
    """One GetElementDetails `details` block for a single-plane roof.

    The poly half is present and empty, exactly as the native command emits it —
    both halves always ride along, so a caller must branch on `roofClass` rather
    than test for zero.
    """
    return {
        "thickness": 0.3, "level": level, "planAngle": 0.0,
        "slantAngle": slant, "isSlanted": False,
        "polygonOutline": [{"x": x, "y": y} for x, y in outline],
        "polygonArcs": [0.0] * len(outline),
        "holes": [], "hasHoles": False,
        "roofClass": "plane",
        # The pivot line both A and B hinge on: along +X through the origin.
        "baseLine": {"begCoordinate": {"x": 0.0, "y": 0.0},
                     "endCoordinate": {"x": 10.0, "y": 0.0}},
        "posSign": pos_sign,
        "pivotOutline": [], "pivotArcs": [], "levels": [], "levelNum": 0,
        "eavesOverHang": 0.0, "overHangType": 0,
    }


_ROOF_DETAILS = {
    # Rises towards +Y: low edge on the pivot at y=0, high edge at y=5.
    _ROOF_GUIDS["a"]: _plane_roof([(0.0, 0.0), (10.0, 0.0), (10.0, 5.0), (0.0, 5.0)],
                                  True, _ROOF_SLANT),
    # Same pivot, opposite posSign — rises towards -Y. Shares the low corner at the
    # origin with roof A, so the two must land in ONE cluster.
    _ROOF_GUIDS["b"]: _plane_roof([(0.0, 0.0), (0.0, -5.0), (10.0, -5.0), (10.0, 0.0)],
                                  False, _ROOF_SLANT),
    # Dead flat: a real roof with no fall direction. Not a fallback candidate —
    # no amount of mesh would give it a slope — so it must be SKIPPED, not measured.
    _ROOF_GUIDS["flat"]: _plane_roof([(20.0, 0.0), (24.0, 0.0), (24.0, 4.0)],
                                     True, 0.0),
}

# The `slabs` scenario — MassingFeasibility / MassingDataset read massing blocks as
# slabs, and the harness's default selection is three COLUMNS, so without these a
# massing command dry-runs to "no slabs in the selection" and never reaches its own
# maths. Three blocks of deliberately different size: one over the 500 m2 oversize
# rule (footprint * 0.8 > 500), one under it, and one with a hole, so the flagging,
# the hole reporting and the totals all have something to act on.
_SLAB_GUIDS = {"big": _GUID % 701, "small": _GUID % 702, "holed": _GUID % 703}


def _slab(outline, thickness, holes=()):
    """One GetElementDetails `details` block for a slab."""
    return {
        "thickness": thickness, "height": 0.0, "level": 0.0,
        "planAngle": 0.0, "slantAngle": 0.0, "isSlanted": False,
        "polygonOutline": [{"x": x, "y": y} for x, y in outline],
        "polygonArcs": [0.0] * len(outline),
        "polygonZ": [],
        "holes": [{"polygonOutline": [{"x": x, "y": y} for x, y in ring],
                   "polygonArcs": [0.0] * len(ring), "polygonZ": []}
                  for ring in holes],
        "hasHoles": bool(holes),
    }


_SLAB_DETAILS = {
    # 40 x 20 = 800 m2 -> 800 * 0.8 = 640 > 500, so this one is OVERSIZE.
    # 18.6 m thick = 6 floors at the 3.1 m the story fixture implies.
    _SLAB_GUIDS["big"]: _slab([(0.0, 0.0), (40.0, 0.0), (40.0, 20.0), (0.0, 20.0)], 18.6),
    # 20 x 20 = 400 m2 -> 320, under the rule. 3 floors.
    _SLAB_GUIDS["small"]: _slab([(50.0, 0.0), (70.0, 0.0), (70.0, 20.0), (50.0, 20.0)], 9.3),
    # A courtyard block: the hole is REPORTED (has_holes) but not subtracted — the
    # footprint this report has always used is the gross outer contour.
    _SLAB_GUIDS["holed"]: _slab([(0.0, 30.0), (30.0, 30.0), (30.0, 50.0), (0.0, 50.0)], 12.4,
                                holes=[[(10.0, 38.0), (20.0, 38.0), (20.0, 44.0), (10.0, 44.0)]]),
}


# --------------------------------------------------------------------------- #
#  The snapshot: two boxes, and the ZERO-COPY bridge that serves their arrays   #
# --------------------------------------------------------------------------- #
#
# The same two guids EvP.GetModelElements and API.GetSelectedElements describe,
# so a command that snapshots the selection and then asks the structured reads
# about those elements sees ONE consistent world.
#
# The geometry is a box per element with its vertices SPLIT PER FACE — 24
# vertices for 12 triangles, not a welded 8. That is what the real extractor
# produces since the normals fix (ConvexPolygon::GetNormalVectorByVertex plus
# splitting), and it is load-bearing for anything that reads a per-vertex normal:
# a welded fixture shades every box like a sphere and would let a face-normal bug
# through. Triangles alternate between two materials so the per-triangle material
# index is never uniform — a single-material fixture cannot catch a grouping bug.
_SNAP_GUIDS = [_GUID % 601, _GUID % 602]

# The MULTI-PLANE roof of the `roofs` scenario, deliberately one of the snapshot's
# own guids: the mesh fallback can only be exercised if the roof that needs it is
# actually in the snapshot. Its slantAngle is 0 — that is not a flat roof, it is
# what a poly roof always reports, and `roofClass` is the only thing that says so.
_POLY_ROOF_GUID = _SNAP_GUIDS[0]

_ROOF_DETAILS[_POLY_ROOF_GUID] = {
    "thickness": 0.3, "level": 0.0, "planAngle": 0.0,
    "slantAngle": 0.0, "isSlanted": False,
    "polygonOutline": [{"x": x, "y": y}
                       for x, y in [(0.0, 20.0), (8.0, 20.0), (8.0, 26.0), (0.0, 26.0)]],
    "polygonArcs": [0.0] * 4,
    "holes": [], "hasHoles": False,
    "roofClass": "poly",
    "baseLine": {"begCoordinate": {"x": 0.0, "y": 0.0},
                 "endCoordinate": {"x": 0.0, "y": 0.0}},
    "posSign": False,
    "pivotOutline": [{"x": x, "y": y}
                     for x, y in [(1.0, 21.0), (7.0, 21.0), (7.0, 25.0), (1.0, 25.0)]],
    "pivotArcs": [0.0] * 4,
    # The pitch a poly roof really carries: per LEVEL, not in slantAngle.
    "levels": [{"levelAngle": math.atan(0.3), "levelHeight": 2.0}],
    "levelNum": 1,
    "eavesOverHang": 0.5, "overHangType": 0,
}


# --------------------------------------------------------------------------- #
#  Tapir element registry — GetElementsByType hands out guids, and             #
#  GetDetailsOfElements has to answer for the SAME ones with the right shape.  #
# --------------------------------------------------------------------------- #
# Two guids per type so a PAIRWISE check (wall-to-wall connection angles, zone
# adjacency, door bridging) has something to compare. One guid per type made
# every such command dry-run "clean" without executing its real loop body.
_ELEM_TYPE_BASE = {"Wall": 400, "Zone": 410, "Door": 420, "Window": 430,
                   "Slab": 440, "Stair": 450, "Roof": 460, "Object": 470}
_elem_types = {}          # guid -> element type name, filled on first fetch

# Layers, so a command that filters by layer NAME can be exercised. "Balcony" is
# index 33 — deliberately a plain small number, because the bug this fixture
# exists for was a command comparing a layer NAME against the numeric layerIndex
# a Tapir detail actually carries.
_LAYERS = [("Balcony", 33), ("Walls", 70), ("Zones", 73), ("Structural", 2871)]
_LAYER_GUID = "00000000-0000-0000-0000-9%011d"

# Surface ATTRIBUTES, the project/template pool the official API reports —
# deliberately NOT the same set as the model pool in EvP.GetModelMaterials.
#
# ⚠️ THE TWO POOLS MUST DISAGREE HERE OR THE FIXTURE PROVES NOTHING.
# SurfaceTemplateDump exists to join "every surface the template defines"
# against "the surfaces this model actually uses", so a fake where the two are
# identical exercises neither the unused-surface branch nor the join. The first
# two names match the model fake exactly; "Steel Brushed" is defined and unused.
#
# The channels are ints 0..100, which is what the official API documents and
# what the dump's SCALE CHECK compares against ModelerAPI's mixed scales.
# (name, materialType, shine, specularReflection, transparency, diffuseReflection)
_SURFACES = [
    ("Concrete", "Matte", 4, 26, 0, 70),
    ("Glass - Clear", "Glass", 80, 100, 69, 40),
    ("Steel Brushed", "Metal", 18, 95, 0, 55),
]
_SURFACE_GUID = "00000000-0000-0000-0000-7%011d"
_BALCONY_LAYER_INDEX = 33

# The CLOSED elementType enum of the official API.GetElementsByType, verbatim
# from archicad/releases/ac29/b3000commands.py. Anything outside it is rejected
# by Archicad, so the harness answers it empty rather than inventing elements.
_API_ELEMENT_TYPES = {
    "All", "Wall", "Column", "Beam", "Window", "Door", "Object", "Lamp", "Slab",
    "Roof", "Mesh", "Zone", "CurtainWall", "Shell", "Skylight", "Morph", "Stair",
    "Railing", "Opening",
}


# Property fakes. The zone categories are the REAL Lithuanian spellings a
# project uses ("GYVENAMASIS KAMBARYS", not "GYVENAMASIS") — the exact-match
# room-type lookup that shipped looked correct against a tidy fixture and matched
# nothing on the model.
_property_names = {}          # propertyId guid -> property name
_property_slots = {}          # property name -> stable small int
_ZONE_CATEGORIES = ["GYVENAMASIS KAMBARYS", "VIRTUVE"]
_ZONE_CAT_GUID = "00000000-0000-0000-0000-8%011d"


def _property_slot(name):
    return _property_slots.setdefault(name, len(_property_slots))


def _property_value_for(name, guid, index):
    """A plausible value for one property on one element, or None to answer
    `userUndefined` (which a command must also survive)."""
    if name.endswith("Zone_NetArea") or name.endswith("Zone_CalculatedArea"):
        return "18.50"
    if "ZoneCategory" in name:
        return _ZONE_CATEGORIES[index % len(_ZONE_CATEGORIES)]
    if name.endswith("Zone_ZoneName"):
        return "1-1"
    if name.endswith("Zone_ZoneNumber"):
        return str(index + 1)
    if name.endswith("Room Count"):
        return "2"
    if name.endswith("Total Area Display"):
        return "37.00"
    return None


def _fixture_outline(guid):
    """The plan outline of a fixture element: a polygon as-is, a wall as its
    reference line. Returns [] for anything with no plan extent."""
    detail = _tapir_detail_for(guid)
    d = detail.get("details", {})
    ring = [(p["x"], p["y"]) for p in d.get("polygonOutline", [])]
    if ring:
        return ring
    beg, end = d.get("begCoordinate"), d.get("endCoordinate")
    if beg and end:
        return [(beg["x"], beg["y"]), (end["x"], end["y"])]
    return []


def _fixtures_touch(guid_a, guid_b, tolerance=0.05):
    """Do two fixture elements meet in plan? Segment-to-segment, so a balcony
    slab whose edge lies ON the wall reference line counts as touching."""
    a, b = _fixture_outline(guid_a), _fixture_outline(guid_b)
    if len(a) < 2 or len(b) < 2:
        return False

    def seg_dist(p1, p2, p3, p4):
        def pt_seg(p, s, e):
            dx, dy = e[0] - s[0], e[1] - s[1]
            l2 = dx * dx + dy * dy
            if l2 < 1e-18:
                return math.hypot(p[0] - s[0], p[1] - s[1])
            t = max(0.0, min(1.0, ((p[0] - s[0]) * dx + (p[1] - s[1]) * dy) / l2))
            return math.hypot(p[0] - (s[0] + t * dx), p[1] - (s[1] + t * dy))
        return min(pt_seg(p3, p1, p2), pt_seg(p4, p1, p2),
                   pt_seg(p1, p3, p4), pt_seg(p2, p3, p4))

    na = len(a) if len(a) > 2 else 1
    nb = len(b) if len(b) > 2 else 1
    for i in range(na):
        a1, a2 = a[i], a[(i + 1) % len(a)]
        for j in range(nb):
            b1, b2 = b[j], b[(j + 1) % len(b)]
            if seg_dist(a1, a2, b1, b2) <= tolerance:
                return True
    return False


def _layer_slot(guid):
    """Index into _LAYERS for a layer attribute guid, or None."""
    for i in range(len(_LAYERS)):
        if guid == _LAYER_GUID % i:
            return i
    return None


def _elem_type_guids(elem_type):
    """The (stable) guids for one element type, remembering the type per guid."""
    base = _ELEM_TYPE_BASE.setdefault(elem_type, 480 + 10 * len(_ELEM_TYPE_BASE))
    guids = [_GUID % base, _GUID % (base + 1)]
    for g in guids:
        _elem_types[g] = elem_type
    return guids


def _tapir_detail_for(guid):
    """One Tapir GetDetailsOfElements item: {type, id, floorIndex, details}.

    The two guids of a type are placed so they actually INTERACT: the two walls
    meet at a shared corner at a non-90 deg angle (so an angle check fires), the
    two zones sit 0.1 m apart (an internal separator, so they group as one
    apartment), and the two doors are hosted in wall #0.
    """
    elem_type = _elem_types.get(guid, "Wall")
    second = guid.endswith("1")
    walls = _elem_type_guids("Wall")

    if elem_type == "Slab":
        # A 4 x 1.5 m balcony hanging off wall #0 (which runs +X from the
        # origin), so the wall-adjacent edge is the y = 0 side. The second slab
        # is a floor slab on another layer, so a layer filter has something to
        # exclude — a fixture where the filter matches everything cannot catch a
        # filter that silently does nothing.
        x0 = 0.0 if not second else 20.0
        ring = ([(x0, -1.5), (x0 + 4.0, -1.5), (x0 + 4.0, 0.0), (x0, 0.0)]
                if not second else
                [(x0, 0.0), (x0 + 6.0, 0.0), (x0 + 6.0, 6.0), (x0, 6.0)])
        return {"type": "Slab", "id": "S-%d" % (2 if second else 1),
                "floorIndex": 0,
                "layerIndex": (2871 if second else _BALCONY_LAYER_INDEX),
                "details": {"thickness": 0.2, "level": 0.0,
                            "polygonOutline": [{"x": x, "y": y} for x, y in ring],
                            "holes": []}}

    if elem_type == "Zone":
        x0 = 0.0 if not second else 4.1        # 0.1 m gap -> internal separator
        ring = [{"x": x0, "y": 0.0}, {"x": x0 + 4.0, "y": 0.0},
                {"x": x0 + 4.0, "y": 3.0}, {"x": x0, "y": 3.0}]
        details = {"name": "1-1", "numberStr": "2" if second else "1",
                   "isManual": False, "polygonOutline": ring, "holes": [],
                   # The zone's CATEGORY ATTRIBUTE — the only route to a category
                   # NAME. The Zone_ZoneCategoryCode property reads "1"/"V" on a
                   # real project, which no room-type rule can bucket.
                   "categoryAttributeId": {"guid": _ZONE_CAT_GUID % (1 if second else 0)}}
    elif elem_type in ("Door", "Window"):
        # `centerOffset` — the position along the owner wall's reference line —
        # is REQUIRED in Tapir's WindowDoorDetails and is the only position an
        # opening has (there is no outline and no coordinates). Two openings
        # 1.0 m and 3.0 m along a 4 m wall leave 1.0 m at each balcony end, so
        # the 1.2 m edge rule fires and mode 5 has something to find.
        details = {"width": 0.9 if second else 1.05,
                   "height": 2.1 if elem_type == "Door" else 1.4,
                   "sillHeight": 0.0 if elem_type == "Door" else 0.9,
                   "centerOffset": 3.0 if second else 1.0,
                   "ownerElementId": {"guid": walls[0]},
                   "ownerElementType": "Wall"}
    elif elem_type == "Wall":
        # Wall 0 runs along +X from the origin; wall 1 leaves the SAME corner at
        # ~30 deg, so it is connected and skew — both wall checks have work to do.
        beg, end = ((4.0, 0.0), (7.46, 2.0)) if second else ((0.0, 0.0), (4.0, 0.0))
        details = {"geometryType": "Straight",
                   "begCoordinate": {"x": beg[0], "y": beg[1]},
                   "endCoordinate": {"x": end[0], "y": end[1]},
                   "begThickness": 0.1, "endThickness": 0.1,
                   "height": 2.7, "zCoordinate": 0.0}
    else:
        details = {}

    return {"type": elem_type,
            "id": "%s-%d" % (elem_type[:2].upper(), 2 if second else 1),
            "floorIndex": 0,
            "details": details}


def _box_mesh(origin):
    """One axis-aligned 2x2x3 box: (positions, normals, triangles, tri_material).

    Per-face vertices, so normals are exact and UV projection has a real face to
    project onto.
    """
    ox, oy, oz = origin
    sx, sy, sz = 2.0, 2.0, 3.0
    corners = [(0, 0, 0), (sx, 0, 0), (sx, sy, 0), (0, sy, 0),
               (0, 0, sz), (sx, 0, sz), (sx, sy, sz), (0, sy, sz)]
    faces = [((0, 1, 2, 3), (0.0, 0.0, -1.0)), ((4, 7, 6, 5), (0.0, 0.0, 1.0)),
             ((0, 4, 5, 1), (0.0, -1.0, 0.0)), ((1, 5, 6, 2), (1.0, 0.0, 0.0)),
             ((2, 6, 7, 3), (0.0, 1.0, 0.0)), ((3, 7, 4, 0), (-1.0, 0.0, 0.0))]
    positions, normals, triangles, tri_material = [], [], [], []
    for fi, (quad, normal) in enumerate(faces):
        base = len(positions) // 3
        for ci in quad:
            cx, cy, cz = corners[ci]
            positions.extend([ox + cx, oy + cy, oz + cz])
            normals.extend(normal)
        triangles.extend([base, base + 1, base + 2, base, base + 2, base + 3])
        tri_material.extend([1 if fi % 2 == 0 else 2] * 2)
    return positions, normals, triangles, tri_material


_SNAP_MESHES = [_box_mesh((0.0, 0.0, 0.0)), _box_mesh((6.0, 1.0, 0.0))]


class _FakeBridge:
    """Stands in for the `_evp` C extension: the zero-copy geometry bridge.

    `evp.geometry.Mesh._view` asks for a named array and reshapes it from the
    dtype and shape this reports, so the dtypes have to be the REAL ones —
    float64 vertices, float32 normals, uint32 triangles, int32 materials. A fake
    that returned float64 everywhere would hide a command that assumes the wrong
    width.
    """

    @staticmethod
    def acquire_buffer(request_json):
        import array
        request = json.loads(request_json)
        kind = request.get("kind")
        positions, normals, triangles, tri_material = _SNAP_MESHES[request.get("mesh", 0)]
        if kind == "vertices":
            return array.array("d", positions).tobytes(), json.dumps(
                {"dtype": "float64", "shape": [len(positions) // 3, 3]})
        if kind == "normals":
            return array.array("f", normals).tobytes(), json.dumps(
                {"dtype": "float32", "shape": [len(normals) // 3, 3]})
        if kind == "triangles":
            return array.array("I", triangles).tobytes(), json.dumps(
                {"dtype": "uint32", "shape": [len(triangles) // 3, 3]})
        if kind == "triMaterial":
            return array.array("i", tri_material).tobytes(), json.dumps(
                {"dtype": "int32", "shape": [len(tri_material), 1]})
        raise KeyError("dryrun bridge has no array kind %r" % kind)


sys.modules.setdefault("_evp", _FakeBridge)


# --------------------------------------------------------------------------- #
#  Plan overlay probe — a real Archicad window tree                            #
# --------------------------------------------------------------------------- #
#
# The chain a maximised floor plan actually produces. Kept stateful for the
# overlay itself (show adds it to the tree, hide removes it) so the probe's
# click-through check — "present in the strict chain, absent from the hit
# chain" — is really exercised instead of always taking the not-found branch.

_OVERLAY_HWND = 0x00AA0001
_OVERLAY_CLS = "EvP_PlanOverlayProbe_Window"
_overlay_live = []          # empty or [rect] — a list so it stays module state
_overlay_layered = [False]  # what the last show asked for; drives survival
_plan_track = {"on": False, "intervalMs": 33,
               "polls": 0, "recomputes": 0, "repaints": 0}


def _win(hwnd, parent, cls, rect, style, ex="0x00000000", text="", visible=True):
    left, top, right, bottom = rect
    return {"hwnd": hwnd, "parent": parent, "class": cls, "text": text,
            "ctrlId": 0, "visible": visible, "style": style, "exStyle": ex,
            "rect": {"left": left, "top": top, "right": right, "bottom": bottom}}


# ⚠️ DGClientWindow is the FRAME's MDI client and the document window is INSIDE
# it — confirmed live 2026-08-04, and the reverse of what was assumed first. The
# document window's rect is legitimately larger than its parent's because it is
# maximised with a caption and thick frame.
_PLAN_CHAIN = [
    _win(0x900BD4, 0x000000, "DGFrameWindow",
         (4874, 313, 7285, 1849), "0x16CF0000", ex="0x00000110",
         text="python tests - Archicad 29"),
    _win(0x58163C, 0x900BD4, "DGClientWindow",
         (5022, 558, 6851, 1804), "0x56000000", ex="0x00000010"),
    _win(0x101980, 0x58163C, "DGModelessDlgClass",
         (5011, 513, 6862, 1816), "0x55CF0000", ex="0x00010101",
         text="python tests / 0. Ground Floor"),
    # The canvas: no WS_CLIPSIBLINGS and no WS_CLIPCHILDREN, which is the whole
    # reason action="clip" exists.
    _win(0x15206E, 0x101980, "DGUserItemClass",
         (5022, 558, 6851, 1776), "0x50020000"),
]


def _overlay_win():
    return _win(_OVERLAY_HWND, 0x101980, _OVERLAY_CLS,
                (5022, 558, 6851, 1776), "0x54000000", ex="0x08000020",
                text="EvP Probe Overlay")


def _plan_overlay(command, params):
    if command == "EvP.ProbeWindowAt":
        # ⚠️ When the overlay is up the strict chain does not merely GAIN it, it
        # ENDS at it: the overlay is a sibling of the canvas, on top, so
        # ChildWindowFromPointEx picks it at the document-window level and the
        # canvas never appears. Reproducing that faithfully is what makes this
        # harness able to catch the level-climbing bug — a fake that appended the
        # overlay after the canvas would let the broken resolution pass.
        strict = list(_PLAN_CHAIN)
        if _overlay_live:
            strict = _PLAN_CHAIN[:-1] + [_overlay_win()]
        return _ok({
            "point": {"x": 6023, "y": 1133, "defaulted": True},
            "strictChain": strict,
            # The hit chain skips WS_EX_TRANSPARENT windows — which is exactly
            # what makes the overlay click-through, so it is never in here.
            "hitChain": list(_PLAN_CHAIN),
            # The host chain excludes our class outright, so it is the plain
            # Archicad tree whether or not an overlay is up.
            "hostChain": list(_PLAN_CHAIN),
            "realChild": _PLAN_CHAIN[-1],
        })

    if command == "EvP.EnumChildWindows":
        children = _PLAN_CHAIN[1:] + ([_overlay_win()] if _overlay_live else [])
        return _ok({"mainWindow": _PLAN_CHAIN[0], "windows": children,
                    "count": len(children)})

    if command == "EvP.ProbeOverlayShow":
        _overlay_live.append(1)
        _overlay_layered[0] = bool(params.get("layered"))
        return _ok({"overlay": _overlay_win(), "parent": _PLAN_CHAIN[2],
                    "screenRect": _overlay_win()["rect"],
                    "covered": bool(params.get("cover")),
                    "layered": bool(params.get("layered"))})

    if command == "EvP.ProbeOverlayState":
        if not _overlay_live:
            return _ok({"exists": False, "paintCount": 0})
        # Encodes the REAL measured outcome, so both branches are exercised and
        # the fake teaches the same thing the live runs did: a plain GDI overlay
        # is overdrawn within a few hundred ms, a WS_EX_LAYERED one survives.
        # A fake that always reported 5/5 would leave the "it died" reading
        # untested — and misreading exactly that cost two sessions.
        _overlay_live.append(1)
        n = 1 if _overlay_layered[0] else len(_overlay_live)
        gone = {"sampled": True, "magentaPoints": 0,
                "colours": {"centre": "#FFFFFF", "nw": "#FFFFFF", "ne": "#FFFFFF",
                            "sw": "#C8C8C8", "se": "#FFFFFF"}}
        intact = {"sampled": True, "magentaPoints": 5,
                  "colours": {k: "#FF00FF"
                              for k in ("centre", "nw", "ne", "sw", "se")}}
        return _ok({"exists": True, "overlay": _overlay_win(),
                    "parent": _PLAN_CHAIN[2],
                    "screenRect": _overlay_win()["rect"],
                    "parentClientRect": {"left": 0, "top": 0,
                                         "right": 1829, "bottom": 1246},
                    "paintCount": 1, "lastPaintMsAgo": 412,
                    "pixels": intact if n < 3 else gone,
                    "repaintPainted": True,
                    "pixelsAfterRepaint": intact})

    # --- Plan overlay TRACKING (§15 navigation sync) ------------------------
    #
    # The transform is the real shape: scaleY NEGATIVE (model Y up, screen Y
    # down) and an offset that puts the model origin somewhere sane on screen.
    # A fake with a positive scaleY would let mirrored geometry pass here.
    if command == "EvP.OverlayTransform" or command == "EvP.OverlayTrackStats":
        transform = {
            "valid": True,
            "scaleX": 37.795, "scaleY": -37.795,
            "offX": 915.0, "offY": 623.0,
            "refPointA": {"x": 228.0, "y": 152.0},
            "refPointB": {"x": 1600.0, "y": 1066.0},
            "refModelA": {"x": -18.1746, "y": 12.4620},
            "refModelB": {"x": 18.1217, "y": -11.7213},
            # The REAL numbers off a 150%-scaled display: the canvas is in
            # physical pixels, the projection in logical ones.
            "impliedW": 2187.0, "impliedH": 1236.0,
            "canvasW": 3281.0, "canvasH": 1854.0,
            "dpiX": 1.5002, "dpiY": 1.5000, "dpiApplied": True,
        }
        if command == "EvP.OverlayTransform":
            return _ok({"transform": transform})
        _plan_track["polls"] += 12
        _plan_track["recomputes"] += 3
        _plan_track["repaints"] += 3
        return _ok({"exists": bool(_overlay_live), "tracking": _plan_track["on"],
                    "intervalMs": _plan_track["intervalMs"],
                    "polls": _plan_track["polls"],
                    "recomputes": _plan_track["recomputes"],
                    "repaints": _plan_track["repaints"],
                    "acapiFailures": 0, "transform": transform,
                    "modelToken": 4471})

    if command == "EvP.OverlayCalibrate":
        # Measured at 150% scaling. Only the canvas's two axes agree; the others
        # differ by unequal insets, which is what makes the sweep discriminating
        # rather than merely informative.
        return _ok({"rows": [
            {"window": "canvas", "clientW": 3281, "clientH": 1854,
             "impliedW": 2187, "impliedH": 1236, "kx": 1.5002, "ky": 1.5000,
             "disagree": 0.00013},
            {"window": "document", "clientW": 3303, "clientH": 1944,
             "impliedW": 2187, "impliedH": 1236, "kx": 1.5103, "ky": 1.5728,
             "disagree": 0.0414},
            {"window": "mdiclient", "clientW": 3281, "clientH": 1900,
             "impliedW": 2187, "impliedH": 1236, "kx": 1.5002, "ky": 1.5372,
             "disagree": 0.0247},
        ]})

    if command == "EvP.SetOverlayGeometry":
        polys = params.get("polylines") or []
        return _ok({"polylines": len(polys),
                    "points": sum(len(p) // 2 for p in polys)})

    if command == "EvP.SetOverlayTracking":
        _plan_track["on"] = bool(params.get("enable", True))
        _plan_track["intervalMs"] = params.get("intervalMs", 33)
        return _ok({"tracking": _plan_track["on"],
                    "intervalMs": _plan_track["intervalMs"]})

    if command == "EvP.ProbeForceRedraw":
        return _ok({"mode": params.get("mode", "both"), "acapiErr": 0,
                    "invalidated": bool(params.get("hwnd"))})

    if command == "EvP.ProbeOverlayHide":
        was = bool(_overlay_live)
        _overlay_live.clear()
        # restoreStyle defaults TRUE, and the caller passing False is the whole
        # point of the parameter — a hide that always restored silently undid the
        # clip experiment it was called before.
        restore = params.get("restoreStyle", True)
        return _ok({"wasVisible": was, "paintCount": 3,
                    "styleRestored": bool(restore),
                    "styleStillSet": not restore})

    # ProbeSetClipSiblings — the canvas's real 0x50020000 gaining whichever bit
    # was asked for: WS_CLIPSIBLINGS 0x04000000 or WS_CLIPCHILDREN 0x02000000.
    bits = 0
    if params.get("clipSiblings", True):
        bits |= 0x04000000
    if params.get("clipChildren", False):
        bits |= 0x02000000
    before = 0x50020000
    enable = bool(params.get("enable", True))
    after = (before | bits) if enable else (before & ~bits)
    return _ok({"target": _PLAN_CHAIN[-1], "before": "0x%08X" % before,
                "after": "0x%08X" % after, "enabled": enable})


def _one(command, params):
    """Canned, SHAPE-ACCURATE responses — the keys the native commands really emit.

    Shape accuracy is the whole value: a wrapper that reads `results` when the
    command emits `elementIds` fails here exactly as it would live.
    """
    if command.startswith("Tapioca."):
        command = "EvP." + command[len("Tapioca."):]

    handled = _scriptui(command, params)
    if handled is not None:
        return handled

    if command == "API.GetProductInfo":
        return _v2({"version": 29, "buildNumber": 3000, "languageCode": "INT"})

    if command == "Tapir.GetCurrentWindowType":
        return _v2({"currentWindowType": "FloorPlan"})

    if command == "EvP.GetSelection":
        if _SCENARIO == "polyline":
            guids = list(_POLY_DETAILS.keys())
            return _v2({"elements": [{"elementId": {"guid": guid}} for guid in guids]})
        if _SCENARIO == "topography":
            return _v2({"elements": [
                {"elementId": {"guid": _SNAP_GUIDS[0]}},
            ]})
        if _SCENARIO == "walls":
            return _v2({"elements": [{"elementId": {"guid": guid}}
                                     for guid in _WALL_DETAILS]})
        if _SCENARIO == "roofs":
            return _v2({"elements": [{"elementId": {"guid": guid}}
                                     for guid in _ROOF_DETAILS]})
        if _SCENARIO == "slabs":
            return _v2({"elements": [{"elementId": {"guid": guid}}
                                     for guid in _SLAB_DETAILS]})
        return _v2({"elements": [{"elementId": {"guid": _GUID % i}} for i in range(3)]})

    if command in ("EvP.SetSelection", "EvP.ModifySelection"):
        elements = params.get("elements", [])
        missing = []
        count = 0 if params.get("op") == "clear" else len(elements)
        data = {"selected": len(elements), "missing": missing, "count": count}
        if command == "EvP.ModifySelection":
            data["changed"] = len(elements)
        return _v2(data)

    if command == "EvP.ListSelectionSets":
        return _v2({"names": list(_selection_sets)})

    if command == "EvP.GetSelectionSet":
        name = params.get("name", "")
        if _SCENARIO == "topography" and name == "Terrain":
            guids = [_SNAP_GUIDS[0]]
        else:
            guids = list(_selection_sets.get(name, []))
        return _v2({"elements": [{"elementId": {"guid": guid}} for guid in guids],
                    "count": len(guids)})

    if command == "EvP.ModifySelectionSet":
        name = params.get("name", "")
        values = _selection_sets.setdefault(name, [])
        incoming = [item.get("elementId", {}).get("guid", "")
                    for item in params.get("elements", [])]
        if params.get("current"):
            incoming = [_GUID % i for i in range(3)]
        op = params.get("op")
        if op == "update":
            _selection_sets[name] = list(dict.fromkeys(incoming))
        elif op == "add":
            _selection_sets[name] = list(dict.fromkeys(values + incoming))
        elif op == "remove":
            _selection_sets[name] = [g for g in values if g not in incoming]
        elif op == "clear":
            _selection_sets[name] = []
        values = _selection_sets[name]
        return _v2({"elements": [{"elementId": {"guid": guid}} for guid in values],
                    "changed": len(incoming), "count": len(values)})

    if command == "EvP.ReselectSelectionSet":
        values = list(_selection_sets.get(params.get("name", ""), []))
        return _v2({"selected": len(values), "missing": [], "changed": len(values), "count": len(values)})

    if command == "EvP.GetElementIds":
        guids = [(item.get("elementId") or {}).get("guid", "")
                 for item in params.get("elements", [])]
        return _v2({"count": len(guids), "identities": [
            {"elementId": {"guid": g}, "found": True,
             "value": _ids.setdefault(g, "ID-%d" % i),
             "typeName": "Wall", "typeId": 1}
            for i, g in enumerate(guids)]})

    if command == "EvP.GetElementInfo":
        guids = [(item.get("elementId") or {}).get("guid", "")
                 for item in params.get("elements", [])]
        return _v2({"count": len(guids), "infoOfElements": [
            {"elementId": {"guid": guid}, "found": True, "type": "1",
             "floorInd": 0, "angle": _placed_angles.get(guid, 0.0)}
            for guid in guids]})

    if command == "EvP.GetLibraryPartInfo":
        names = params.get("libraryPartNames", [])
        name = names[0] if names else "Slope Symbol"
        return _v2({"libraryPartName": name, "libInd": 42,
                    "sizeA": 0.8, "sizeB": 0.25, "paramCount": 6})

    if command == "EvP.FindPlacedObjects":
        return _v2({"elements": [{"elementId": {"guid": _GUID % 820}}],
                    "count": 1, "libraryPartName": "Slope Symbol"})

    if command == "EvP.PlaceLibraryObject":
        # REMEMBER THE ANGLE, so the read-back below reports what was really asked
        # for. A fake that always answered 0 made every verify-after-write look like
        # a failed placement — and PlaceSlopeSymbols reads its angles back precisely
        # to tell "the geometry is wrong" from "the placement dropped the angle",
        # which is the one question a constant answer cannot help with. It also
        # alerts on a mismatch, so the fake was manufacturing a scary false alarm on
        # every dry run.
        guid = _GUID % (830 + len(seen))
        _placed_angles[guid] = params.get("angle", 0.0)
        return _v2({"elementId": {"guid": guid},
                    "libraryPartName": (params.get("libraryPartNames") or ["Slope Symbol"])[0],
                    "libInd": 42})

    if command == "EvP.PlaceLevelDimension":
        # The parent rides as the TYPED IDENTITY (`parent.elementId.guid`), not a
        # flat `parentGuid` — the input schema is closed, so the old flat spelling
        # is rejected before the handler runs. Validation catches that on its own;
        # this is here so the verb stops being reported as unanswered.
        return _v2({"elementId": {"guid": _GUID % (860 + len(seen))}})

    if command == "EvP.SetElementIds":
        items = params.get("identities", [])
        for item in items:
            guid = (item.get("elementId") or {}).get("guid", "")
            _ids[guid] = item.get("value", "")
        # E25: the real dispatcher bumps the change token after any successful
        # write, because Archicad never reports our own changes. A fake that did
        # not would make a correct probe report that bump as missing.
        _change["token"] += 1
        _change["drained"] = False
        return _v2({"count": len(items), "changed": len(items),
                    "results": [{"elementId": it.get("elementId", {}),
                                 "succeeded": True} for it in items]})

    if command == "EvP.SetElementDetails":
        # Mirrors the native contract: SPARSE (only changed fields), and a field
        # this kind cannot settle is REJECTED by name rather than dropped. A
        # caller that echoes a whole details record back fails here, which is the
        # entire reason this branch reproduces the rule instead of saying ok.
        edits = params.get("edits") or []
        results, changed = [], 0
        for edit in edits:
            guid = edit.get("guid", "")
            details = edit.get("details") or {}
            kind = "mesh" if guid in _meshes else "column"
            if not guid:
                results.append({"guid": "", "ok": False, "kind": "",
                                "applied": [], "error": "edit has no guid"})
                continue
            if not details:
                results.append({"guid": guid, "ok": False, "kind": kind, "applied": [],
                                "error": "edit has no non-empty details={...}"})
                continue
            allowed = _WRITABLE_DETAILS.get(kind, set())
            bad = sorted(k for k in details if k not in allowed)
            if bad:
                results.append({
                    "guid": guid, "ok": False, "kind": kind, "applied": [],
                    "error": 'kind "%s" cannot write: %s. Writable here: %s. Send only '
                             "the scalar settings you changed." %
                             (kind, ", ".join(bad), ", ".join(sorted(allowed)) or "(nothing)")})
                continue
            _written_details.setdefault(guid, {}).update(details)
            if guid in _meshes:
                _meshes[guid].update(details)
            results.append({"guid": guid, "ok": True, "kind": kind,
                            "applied": sorted(details), "error": ""})
            changed += 1
        return _ok({"count": len(results), "changed": changed, "results": results})

    if command == "EvP.GetElementDetails":
        # ⚠️ THE WIRE IS {elements:[{elementId:{guid}}]} AND THE RECORDS CARRY
        # elementId:{guid} + value, NOT a flat guid + elementId. This fake used to
        # speak the pre-E16.0 flat shape, exactly like the wrapper that called it,
        # so the two agreed with each other and disagreed with the add-on — and a
        # dry run PASSED while every live call died on SchemaValidationFailed
        # before the handler ran. A fake that mirrors the caller instead of the
        # contract cannot catch a contract bug; this one now mirrors the
        # registered schema in ElementReadCommands.cpp.
        guids = [(e.get("elementId") or {}).get("guid", "")
                 for e in params.get("elements", [])]
        if _SCENARIO == "polyline":
            out = []
            for i, g in enumerate(guids):
                rec = _POLY_DETAILS.get(g)
                if rec is None:
                    out.append({"elementId": {"guid": g}, "found": False, "kind": "",
                                "floorInd": 0, "value": "", "typeName": "", "typeId": 0,
                                "reason": "notFound", "details": {}})
                    continue
                out.append({"elementId": {"guid": g}, "found": rec["found"],
                            "kind": rec["kind"], "floorInd": 0,
                            "value": _ids.setdefault(g, "ID-%d" % i),
                            "typeName": rec["typeName"], "typeId": rec["typeId"],
                            "reason": rec.get("reason", ""), "details": rec["details"]})
            return _ok({"count": len(out), "detailsOfElements": out})
        if _SCENARIO == "slabs":
            out = []
            for i, g in enumerate(guids):
                slab = _SLAB_DETAILS.get(g)
                if slab is None:
                    out.append({"elementId": {"guid": g}, "found": False, "kind": "",
                                "floorInd": 0, "value": "", "typeName": "", "typeId": 0,
                                "reason": "notFound", "details": {}})
                    continue
                out.append({"elementId": {"guid": g}, "found": True, "kind": "slab",
                            "floorInd": 0, "value": _ids.setdefault(g, "ID-%d" % i),
                            "typeName": "Slab", "typeId": 5, "reason": "",
                            "details": dict(slab)})
            return _ok({"count": len(out), "detailsOfElements": out})
        if _SCENARIO == "roofs":
            out = []
            for i, g in enumerate(guids):
                roof = _ROOF_DETAILS.get(g)
                if roof is None:
                    out.append({"elementId": {"guid": g}, "found": False, "kind": "",
                                "floorInd": 0, "value": "", "typeName": "", "typeId": 0,
                                "reason": "notFound", "details": {}})
                    continue
                out.append({"elementId": {"guid": g}, "found": True, "kind": "roof",
                            "floorInd": 0, "value": _ids.setdefault(g, "ID-%d" % i),
                            "typeName": "Roof", "typeId": 6, "reason": "",
                            "details": dict(roof)})
            return _ok({"count": len(out), "detailsOfElements": out})
        if any(g in _WALL_DETAILS for g in guids):
            out = []
            for i, g in enumerate(guids):
                wall = _WALL_DETAILS.get(g)
                if wall is None:
                    out.append({"elementId": {"guid": g}, "found": False, "kind": "",
                                "floorInd": 0, "value": "", "typeName": "", "typeId": 0,
                                "reason": "notFound", "details": {}})
                    continue
                out.append({"elementId": {"guid": g}, "found": True, "kind": "wall",
                            "floorInd": 0, "value": _ids.setdefault(g, "ID-%d" % i),
                            "typeName": wall["typeName"], "typeId": wall["typeId"],
                            "reason": "", "details": dict(wall["details"])})
            return _ok({"count": len(out), "detailsOfElements": out})
        # A guid this run's CreateMesh handed out reads back as the mesh it was
        # given; everything else keeps the long-standing column answer.
        if any(g in _meshes for g in guids):
            return _ok({"count": len(guids), "detailsOfElements": [
                {"elementId": {"guid": g}, "found": g in _meshes,
                 "kind": "mesh" if g in _meshes else "column", "floorInd": 0,
                 "value": "ID-%d" % i, "typeName": "Mesh", "typeId": 12,
                 "reason": "" if g in _meshes else "notFound",
                 "details": _meshes.get(g, {})}
                for i, g in enumerate(guids)]})
        return _ok({"count": len(guids), "detailsOfElements": [
            {"elementId": {"guid": g}, "found": True, "kind": "column", "floorInd": 0,
             "value": "ID-%d" % i, "typeName": "Column", "typeId": 3, "reason": "",
             # `level` reads back what SetElementDetails wrote, so a pull-to-mesh
             # style read -> change -> write -> verify really compares its OWN
             # number instead of this fake's constant.
             "details": dict({"height": 3.0, "level": 0.0,
                              "sectionWidth": _sections.get(g, (0.3, 0.3))[0],
                              "sectionHeight": _sections.get(g, (0.3, 0.3))[1],
                              "nSegments": 1, "planAngle": 0.0, "slantAngle": 1.5708,
                              "isSlanted": False,
                              "begCoordinate": {"x": 0.0, "y": 0.0, "z": 0.0},
                              "endCoordinate": {"x": 0.0, "y": 0.0, "z": 0.0}},
                             **_written_details.get(g, {}))}
            for i, g in enumerate(guids)]})

    if command == "EvP.GetWallPlanOutlines":
        # The plan ANCHOR read (PLAT-RE65). A guid outside _WALL_DETAILS is
        # answered as NOT A WALL rather than with a plausible rectangle: the
        # default selection is three columns, and a fake that handed them wall
        # outlines would let a command that never checks `succeeded` dry-run
        # clean and then meet its first real refusal in Archicad.
        out = []
        for e in params.get("elements", []):
            guid = (e.get("elementId") or {}).get("guid", "")
            wall = _WALL_DETAILS.get(guid)
            if wall is None:
                out.append({"elementId": {"guid": guid}, "succeeded": False,
                            "error": "not a wall: Column"})
                continue
            out.append({
                "elementId": {"guid": guid},
                "succeeded": True,
                "wallShape": "straight",
                "outline": [{"x": x, "y": y} for x, y in wall["outline"]],
                "outlineArcs": [0.0] * len(wall["outline"]),
                "holes": [],
                "outlineSource": "connectionPolygon",
                "memoPolygonPresent": True,
                "memoOutline": [{"x": x, "y": y} for x, y in wall["memo"]],
                "memoOutlineArcs": [0.0] * len(wall["memo"]),
                "connectedWalls": dict(wall["connected"]),
            })
        return _ok({"count": len(out), "outlines": out})

    if command == "EvP.SetPlanAnchors":
        _plan_anchors["enabled"] = bool(params.get("enabled", False))
        _plan_anchors["widthPixels"] = float(params.get("widthPixels", 2.0))
        # The drawing half of PLAT-RE65. Mirrors the handler's own contract: a
        # non-wall is SKIPPED rather than refused, so `count` can be lower than
        # the number of guids and a caller that assumes otherwise fails here
        # instead of in Archicad.
        walls = [e for e in params.get("elements", [])
                 if (e.get("elementId") or {}).get("guid", "") in _WALL_DETAILS]
        rings = len(walls)
        vertices = rings * 4 * 6                    # 4 closing segments, 6 verts each
        _plan_anchors["vertices"] = vertices if _plan_anchors["enabled"] else 0
        return _ok({"count": rings, "rings": rings,
                    "vertices": vertices, "accepted": True})

    if command == "EvP.CreateMesh":
        # The C++ guard, mirrored — outline parity, the polyZ/outline pairing,
        # sum(ridgeCounts)*3 == len(ridgeCoords) and the degenerate-ring check.
        # Reproduced because CreateMesh's failures are the quiet kind: a
        # collapsed mesh is created WITHOUT an error, so a caller that got the
        # arrays wrong learns nothing from a fake that just says ok.
        outline = params.get("outline") or []
        poly_z = params.get("polyZ") or []
        counts = params.get("ridgeCounts") or []
        coords = params.get("ridgeCoords") or []
        n = len(outline) // 2
        why = None
        if len(outline) % 2 or n < 3:
            why = "need outline=[x0,y0,x1,y1,...] with >=3 points (even count)"
        elif poly_z and len(poly_z) != n:
            why = "polyZ must have exactly one Z per outline vertex"
        elif sum(counts) * 3 != len(coords):
            why = "sum(ridgeCounts)*3 must equal ridgeCoords.size()"
        else:
            area2 = 0.0
            for i in range(n):
                j = (i + 1) % n
                area2 += outline[i * 2] * outline[j * 2 + 1] - outline[j * 2] * outline[i * 2 + 1]
            if abs(area2) < 1e-9:
                why = "outline encloses no area — the contour is degenerate"
        if why:
            return {"ok": False,
                    "error": {"kind": "CommandFailed", "message": why,
                              "command": command},
                    "meta": {"backend": "dryrun", "duration_ms": 0.1}}
        guid = _GUID % (700 + len(_meshes))
        _meshes[guid] = {
            "level": params.get("baseLevel", 0.0),
            "skirtLevel": params.get("skirtLevel", 0.0),
            "skirtType": params.get("skirt", "SolidBodyWithSkirt"),
            "polygonOutline": [{"x": outline[i * 2], "y": outline[i * 2 + 1]}
                               for i in range(n)],
            "polygonZ": list(poly_z),
            "levelEnds": [sum(counts[:i + 1]) for i in range(len(counts))],
        }
        return _ok({"guid": guid,
                    "baseLevel": params.get("baseLevel", 0.0),
                    "skirtLevel": params.get("skirtLevel", 0.0),
                    "floorInd": params.get("floorInd", 0),
                    "switchedToFloorPlan": bool(params.get("onFloorPlan"))})

    if command == "EvP.GetTextElements":
        # A SYNTHETIC SURVEY, not an echo of the requested guids — because a
        # caller of this command is building a terrain surface, and three texts
        # cannot exercise a triangulation, a contour slice or a hull. Sixty spot
        # heights on a smooth rise do, so the dry run really walks the pipeline
        # instead of bailing out at "not enough points".
        #
        # Two deliberately non-numeric labels are included: refusing them is the
        # rule selection mode exists to enforce (a marquee catches the title
        # block), and a fake that only ever returns clean data would never show
        # a regression in it.
        import math as _math
        texts = []
        for i in range(60):
            x = (i % 10) * 6.0 + (i * 0.37) % 1.5
            y = (i // 10) * 12.0 + (i * 0.53) % 1.5
            z = 94.0 + 3.0 * _math.exp(-((x - 30) ** 2 + (y - 30) ** 2) / 400.0)
            texts.append({"elementId": {"guid": _GUID % (500 + i)}, "content": "%.2f" % z,
                          "x": x, "y": y, "hasBounds": True,
                          "xMin": x, "yMin": y, "xMax": x + 1.2, "yMax": y + 0.5,
                          "anchorX": x, "anchorY": y, "anchor": "bottomLeft",
                          "pen": 12, "angle": 0.0, "size": 2.0,
                          "multiStyle": False, "nLine": 1,
                          "floorInd": 0, "layer": "2131"})
        texts.append({"elementId": {"guid": _GUID % 599}, "content": "Sklypas Nr. 12",
                      "x": 5.0, "y": 5.0, "hasBounds": True,
                      "xMin": 5.0, "yMin": 5.0, "xMax": 9.0, "yMax": 5.5,
                      "anchorX": 5.0, "anchorY": 5.0, "anchor": "bottomLeft",
                      "pen": 1, "angle": 0.0, "size": 2.0,
                      "multiStyle": False, "nLine": 1,
                      "floorInd": 0, "layer": "0000"})
        return _v2({"fromSelection": "elements" not in params,
                    "texts": texts, "count": len(texts), "skipped": 0})

    if command == "EvP.GetArcElements":
        # Circle markers that PAIR with the synthetic texts above — one circle a
        # little offset from each label, which is the real drawing convention
        # (the height is written beside the point, not on it). Two labels share
        # the neighbourhood of one circle on purpose, so a caller that lets two
        # labels claim the same marker fails here.
        arcs = []
        for i in range(60):
            x = (i % 10) * 6.0 + (i * 0.37) % 1.5
            y = (i // 10) * 12.0 + (i * 0.53) % 1.5
            arcs.append({"elementId": {"guid": _GUID % (800 + i)},
                         "x": x - 0.8, "y": y - 0.4, "radius": 0.25,
                         "isCircle": True, "begAngle": 0.0, "endAngle": 0.0,
                         "ratio": 1.0, "angle": 0.0, "pen": 12,
                         "layer": "2131", "floorInd": 0})
        return _v2({"scope": params.get("scope", "database"),
                    "arcs": arcs, "count": len(arcs), "skipped": 0})

    if command == "EvP.CreateText":
        items = params.get("texts", [])
        return _ok({"count": len(items),
                    "guids": [_GUID % (100 + i) for i in range(len(items))],
                    "results": [{"ok": True, "guid": _GUID % (100 + i)}
                                for i in range(len(items))]})

    if command == "EvP.PlacePicture":
        px, py = 64, 48
        if params.get("width") and params.get("height"):
            w, h = params["width"], params["height"]
        else:
            dpi = params.get("dpi") or 96.0
            w, h = px / dpi * 0.0254, py / dpi * 0.0254
        return _ok({"guid": _GUID % 200, "pixelWidth": px, "pixelHeight": py,
                    "placedWidth": w, "placedHeight": h})

    if command == "EvP.CreateColumn":
        n = len(params.get("x", []))
        # Echo the requested section back through GetElementDetails, the way the
        # real pair does — so a probe comparing "asked for" against "read back"
        # is testing its own comparison, not this fake's imagination.
        if params.get("shape") == "circular":
            section = (params.get("diameter", 0.3), params.get("diameter", 0.3))
        elif params.get("shape") == "rectangular":
            section = (params.get("width", 0.3), params.get("height2", 0.3))
        else:
            section = (0.3, 0.3)
        guids = [_GUID % (300 + len(_sections) + i) for i in range(n)]
        for g in guids:
            _sections[g] = section
        return _ok({
            "count": n, "floorInd": 0,
            # The REGISTERED output shape, from CreateCommands.cpp's
            # CreateColumn registration: succeeded + a nested elementId. This
            # fake previously answered {"ok", "guid"}, which the native command
            # does not return — so a command written against the real schema
            # failed here while working in Archicad, and one written against
            # this fake did the reverse.
            "results": [{"succeeded": True, "elementId": {"guid": g}}
                        for g in guids],
            # ⚠️ NOT in the registered schema. Kept only because
            # Commands/TreesFromDxf reads `step["ok"]` and `step["guids"]`,
            # which this fake invented. In Archicad that command therefore
            # records no GUIDs and reports every group as FAILED. Fixing it
            # belongs to CMD-TreesFromDxf; removing these keys here would only
            # change where the bug shows up.
            "ok": True,
            "guids": guids,
        })

    if command == "EvP.GetDrawingClipPolygon":
        guids = params.get("guids", [])
        # A drawing this run PLACED reads back the point count it was given; one
        # it did not falls back to a plausible rectangle.
        return _ok({"count": len(guids), "drawings": [
            {"guid": g, "found": True, "name": "Ground Floor", "isCutWithFrame": True,
             "clipPolygon": [0.0] * (2 * _clips[g]) if g in _clips
                            else [0.0, 0.0, 0.2, 0.0, 0.2, 0.15, 0.0, 0.15], "arcs": [],
             "pos": {"x": 0.1, "y": 0.1},
             "bounds": {"xMin": 0.0, "yMin": 0.0, "xMax": 0.2, "yMax": 0.15},
             "ratio": 1.0, "drawingScale": 0.01}
            for g in guids]})

    if command == "EvP.SetDrawingClipPolygon":
        n = len(params.get("clipPolygon", [])) // 2
        return _ok({"guid": params.get("guid", ""), "verified": True,
                    "pointsWritten": n, "pointsReadBack": n})

    if command == "EvP.ListViews":
        placeable = [v for v in _views if v["placeable"]]
        rows = placeable if params.get("placeableOnly", True) else list(_views)
        if params.get("map", "view") not in ("view", None, ""):
            rows = []                       # only the Public View Map is seeded
        return _ok({"count": len(rows), "views": [dict(v, map=params.get("map", "view"))
                                                  for v in rows]})

    if command == "EvP.PlaceDrawingFromView":
        # The LAYOUTS-ONLY rule, mirrored. A Drawing sourced from a project view
        # can be placed only on a layout — Archicad's rule, stated in the
        # API_DrawingType remarks. Faked so a command aiming one at a worksheet
        # fails offline instead of looking fine until the in-Archicad run.
        #
        # Resolves by NAME as well as guid, because names are now the primary
        # input and a wrapper that sends the wrong key must fail here.
        wanted_name = params.get("viewName", "")
        if wanted_name:
            hits = [v for v in _views if v["name"] == wanted_name and v["placeable"]]
            if not hits:
                return {"ok": False, "data": {"ok": False,
                        "error": 'no placeable view named "%s"' % wanted_name},
                        "meta": {"backend": "dryrun", "duration_ms": 0.1}}
            if len(hits) > 1:
                # Ambiguity is a refusal, mirroring the native command — so a
                # caller that resolves by name fails offline instead of silently
                # placing the wrong view in Archicad.
                return {"ok": False, "data": {"ok": False, "error":
                        '"%s" matches %d views - pass viewGuid to say which you mean'
                        % (wanted_name, len(hits))},
                        "meta": {"backend": "dryrun", "duration_ms": 0.1}}
        wanted_guid = params.get("viewGuid", "")
        if wanted_guid and not any(v["guid"] == wanted_guid and v["placeable"] for v in _views):
            return {"ok": False, "data": {"ok": False,
                    "error": "no placeable view with guid %s" % wanted_guid},
                    "meta": {"backend": "dryrun", "duration_ms": 0.1}}

        target = params.get("layoutGuid", "")
        target_name = params.get("layoutName", "")
        if target_name:
            kind = next((t for t, rows in _databases.items()
                         if target_name in rows.values()), None)
            if kind is None:
                return {"ok": False, "data": {"ok": False,
                        "error": 'no database named "%s"' % target_name},
                        "meta": {"backend": "dryrun", "duration_ms": 0.1}}
            if kind not in ("layout", "masterLayout"):
                return {"ok": False, "data": {"ok": False, "error":
                        "the current database is a %s, which is NOT a layout. A Drawing whose "
                        "source is a project view can be placed ONLY on a layout." % kind},
                        "meta": {"backend": "dryrun", "duration_ms": 0.1}}
        if target:
            kind = next((t for t, rows in _databases.items() if target in rows), None)
            if kind is None:
                return {"ok": False, "data": {"ok": False,
                        "error": "no LAYOUT with guid %s" % target},
                        "meta": {"backend": "dryrun", "duration_ms": 0.1}}
            if kind not in ("layout", "masterLayout"):
                return {"ok": False, "data": {"ok": False, "error":
                        "the current database is a %s, which is NOT a layout. A Drawing whose "
                        "source is a project view can be placed ONLY on a layout." % kind},
                        "meta": {"backend": "dryrun", "duration_ms": 0.1}}
        n = len(params.get("clipPolygon", [])) // 2
        _clips[_GUID % 500] = n
        return _ok({"guid": _GUID % 500, "viewName": "Ground Floor",
                    "layout": "A-101 Plans", "cropped": n > 0})

    # The layout/database family. STATEFUL, like the id and section fakes above:
    # a create is remembered so ListDatabases then reports it and a delete can
    # find it. That is what makes a probe's create -> verify -> delete sequence
    # exercise its OWN logic rather than three unrelated canned answers.
    if command == "EvP.ListDatabases":
        wanted = params.get("types") or list(_databases)
        rows = []
        for db_type in wanted:
            for guid, name in _databases.get(db_type, {}).items():
                row = {"type": db_type, "guid": guid, "name": name,
                       "ref": "", "title": name}
                if db_type == "layout":
                    row["masterLayoutGuid"] = _GUID % 900
                rows.append(row)
        return _ok({"count": len(rows), "databases": rows})

    if command == "EvP.CreateDatabase":
        db_type = params.get("type", "")
        if db_type == "layout":
            return {"ok": False, "data": {"ok": False,
                    "error": "a layout needs a master layout - use API.CreateLayout "
                             "(navtree.create_layout)"},
                    "meta": {"backend": "dryrun", "duration_ms": 0.1}}
        # ⚠️ A DUPLICATE NAME FAILS, and it fails as APIERR_GENERAL. Observed live
        # 2026-07-31: LayoutApiProbe2 Q8 left a fixed-name 3D document behind by
        # design and then could never run again. Modelled here so a probe that
        # leaves an artefact must handle re-running — which is what --twice below
        # is for, since the collision is between RUNS and one run cannot see it.
        wanted_name = params.get("name") or ""
        if wanted_name and wanted_name in _databases.get(db_type, {}).values():
            return {"ok": False, "data": {"ok": False, "error":
                    "ACAPI_Database_NewDatabase failed: APIERR_GENERAL (-2130313215) - General "
                    "error code. [creating a %s named %r - NOTE: a %s named %r ALREADY EXISTS, "
                    "and a duplicate name is the likeliest cause]"
                    % (db_type, wanted_name, db_type, wanted_name)},
                    "meta": {"backend": "dryrun", "duration_ms": 0.1}}
        _db_serial[0] += 1
        guid = _GUID % _db_serial[0]
        name = params.get("name") or ("%s %d" % (db_type, len(_databases.get(db_type, {})) + 1))
        _databases.setdefault(db_type, {})[guid] = name
        return _ok({"type": db_type, "guid": guid, "name": name, "ref": params.get("ref", "")})

    if command == "EvP.DeleteDatabase":
        db_type = params.get("type", "")
        results, deleted = [], 0
        for guid in params.get("guids", []):
            existing = _databases.get(db_type, {})
            if guid in existing:
                results.append({"guid": guid, "ok": True, "name": existing.pop(guid)})
                deleted += 1
            else:
                results.append({"guid": guid, "ok": False,
                                "error": "no %s database with guid %s" % (db_type, guid)})
        return _ok({"deleted": deleted, "results": results})

    # Stateful like the rest of the family: it refuses a guid ListDatabases does
    # not know, so a probe that passes the wrong guid fails HERE rather than
    # in Archicad. `verified` mirrors what the real command reports — it is the
    # field a caller must check, so the fake must not hardcode it True and hide a
    # probe that never looks at it.
    if command == "EvP.SetDocumentFrom3DSettings":
        guid = params.get("guid", "")
        docs = _databases.get("3dDocument", {})
        if guid not in docs:
            return {"ok": False, "data": {"ok": False, "error":
                    "no 3dDocument database with guid %s" % guid},
                    "meta": {"backend": "dryrun", "duration_ms": 0.1}}
        from_view = params.get("fromCurrent3DView", True)
        applied = ["projectionSetting", "window3DInfo"] if from_view else []
        for key in ("transparency", "cutaway3D", "materialFrom3D"):
            if key in params:
                applied.append(key)
        if not applied:
            return {"ok": False, "data": {"ok": False, "error":
                    "nothing to apply: fromCurrent3DView is false and no flags were given"},
                    "meta": {"backend": "dryrun", "duration_ms": 0.1}}
        return _ok({"guid": guid, "name": docs[guid], "isPersp": False,
                    "applied": applied, "verified": bool(from_view)})

    if command == "API.DeleteNavigatorItems":
        return _ok({"executionResults": [{"success": True}
                                         for _ in params.get("navigatorItemIds", [])]})

    # API.GetElementsByType is plan-only and its elementType enum is CLOSED —
    # "Drawing" is not in it (checked in the archicad package's own value_set).
    # A type outside the enum answers EMPTY on purpose, so a command that reaches
    # for one visibly finds nothing exactly as it did live. A type INSIDE the
    # enum answers normally: faking every call empty (the previous behaviour)
    # meant no command reading walls, slabs or zones this way could be exercised
    # at all, which hid a whole mode that returned before its first real step.
    if command == "API.GetElementsByType":
        elem_type = params.get("elementType", "")
        if elem_type not in _API_ELEMENT_TYPES:
            return _ok({"elements": []})
        return _ok({"elements": [{"elementId": {"guid": g}}
                                 for g in _elem_type_guids(elem_type)]})

    # Tapir's form takes `filters` (view scope) as well as a database scope. Two
    # guids, not one, so a pairwise check (wall-to-wall angles, zone adjacency)
    # has something to compare instead of trivially finding nothing. The guids
    # are REMEMBERED against the requested type so Tapir.GetDetailsOfElements can
    # answer with the right detail shape for each.
    if command == "Tapir.GetElementsByType":
        elem_type = params.get("elementType", "Wall")
        guids = _elem_type_guids(elem_type)
        return _ok({"elements": [{"elementId": {"guid": g}} for g in guids]})

    # Details for whatever the type registry knows about each guid; an unknown
    # guid answers as a Wall (the most-read shape). Wall keys mirror Tapir's
    # WallDetails (begCoordinate/endCoordinate/begThickness/endThickness), Zone
    # keys its ZoneDetails (polygonOutline/name/isManual), Door keys its
    # DoorDetails (width/ownerElementId) — see memory `tapir-addon`.
    # --- topology -------------------------------------------------------------
    # Contact is decided GEOMETRICALLY from the same fixture outlines everything
    # else reads, so "the balcony touches the wall it hangs off" is true for the
    # same reason it is true in the model, and a command that matches slab to
    # wall by geometry gets a consistent world instead of an arbitrary pairing.
    if command == "EvP.GetCollisions":
        g1, g2, body, clear = [], [], [], []
        for a in params.get("guids1", []):
            for b in params.get("guids2", []):
                if _fixtures_touch(a, b):
                    g1.append(a); g2.append(b)
                    body.append(True); clear.append(False)
        return _ok({"elemGuids1": g1, "elemGuids2": g2,
                    "hasBodyCollision": body, "hasClearenceCollision": clear})

    if command == "EvP.GetConnectedElements":
        want = params.get("connectedElementType", "")
        others = _elem_type_guids(want) if want in _ELEM_TYPE_BASE else []
        flat, counts = [], []
        for a in params.get("guids", []):
            hits = [b for b in others if b != a and _fixtures_touch(a, b)]
            flat.extend(hits)
            counts.append(len(hits))
        return _ok({"connectedGuids": flat, "counts": counts})

    # --- layer attributes -----------------------------------------------------
    # The three-call walk a command needs to turn a layer NAME into the numeric
    # layerIndex an element detail carries. Answered together so the zip on
    # request order — which is what makes the walk correct — is really exercised.
    # Zone categories, reached the way a command must reach them: the zone's
    # categoryAttributeId -> the attribute's NAME. The property route returns a
    # CODE ("1", "V") and cannot get here at all.
    if command == "API.GetZoneCategoryAttributes":
        out = []
        for item in params.get("attributeIds", []):
            guid = item.get("attributeId", {}).get("guid", "")
            slot = next((i for i in range(len(_ZONE_CATEGORIES))
                         if guid == _ZONE_CAT_GUID % i), None)
            out.append({"zoneCategoryAttribute": {
                "attributeId": {"guid": guid},
                "name": _ZONE_CATEGORIES[slot],
                "categoryCode": ["1", "V"][slot % 2],
                "stampName": "Zone Stamp"}}
                if slot is not None else
                {"error": {"code": 1, "message": "no such zone category"}})
        return _ok({"attributes": out})

    if command == "API.GetAttributesByType":
        if params.get("attributeType") == "Surface":
            return _ok({"attributeIds": [{"attributeId": {"guid": _SURFACE_GUID % i}}
                                         for i in range(len(_SURFACES))]})
        if params.get("attributeType") != "Layer":
            return _ok({"attributeIds": []})
        return _ok({"attributeIds": [{"attributeId": {"guid": _LAYER_GUID % i}}
                                     for i in range(len(_LAYERS))]})

    if command == "API.GetSurfaceAttributes":
        out = []
        for item in params.get("attributeIds", []):
            guid = item.get("attributeId", {}).get("guid", "")
            slot = None
            for i in range(len(_SURFACES)):
                if guid == _SURFACE_GUID % i:
                    slot = i
                    break
            if slot is None:
                out.append({"error": {"code": 1, "message": "no such surface"}})
                continue
            name, material_type, shine, specular, transparency, diffuse = _SURFACES[slot]
            surface = {
                "attributeId": {"guid": guid},
                "name": name,
                "materialType": material_type,
                "ambientReflection": 60,
                "diffuseReflection": diffuse,
                "specularReflection": specular,
                "transparencyAttenuation": 0,
                "emissionAttenuation": 0,
                "surfaceColor": {"red": 0.6, "green": 0.6, "blue": 0.6},
                "specularColor": {"red": 1.0, "green": 1.0, "blue": 1.0},
                "emissionColor": {"red": 0.0, "green": 0.0, "blue": 0.0},
                "fillId": {"attributeId": {"guid": _LAYER_GUID % 0}},
                "transparency": transparency,
                "shine": shine,
            }
            # Only the first carries a texture, so the has-texture branch runs
            # and the no-texture branch runs, in one pass.
            if slot == 0:
                surface["texture"] = {"name": "concrete.jpg"}
            out.append({"surfaceAttribute": surface})
        return _ok({"attributes": out})

    if command == "API.GetLayerAttributes":
        out = []
        for item in params.get("attributeIds", []):
            guid = item.get("attributeId", {}).get("guid", "")
            i = _layer_slot(guid)
            out.append({"layerAttribute": {
                "attributeId": {"guid": guid}, "name": _LAYERS[i][0],
                "intersectionGroupNr": 1, "isLocked": False,
                "isHidden": False, "isWireframe": False}}
                if i is not None else {"error": {"code": 1, "message": "no such layer"}})
        return _ok({"attributes": out})

    if command == "API.GetAttributesIndices":
        out = []
        for item in params.get("attributeIds", []):
            guid = item.get("attributeId", {}).get("guid", "")
            i = _layer_slot(guid)
            out.append({"attributeIndexAndGuid": {"guid": guid, "index": _LAYERS[i][1]}}
                       if i is not None else {"error": {"code": 1, "message": "no such attribute"}})
        return _ok({"attributeIndicesAndGuids": out})

    if command == "Tapir.GetDetailsOfElements":
        return _ok({"detailsOfElements": [
            _tapir_detail_for(item.get("elementId", {}).get("guid", ""))
            for item in params.get("elements", [])
        ]})

    # The SAME two elements EvP.GetModelElements describes, so a command that
    # reads the selection and then asks about those elements gets a consistent
    # world rather than two unrelated fakes.
    #
    # ⚠️ NOT empty. It was, briefly, on the argument that the no-selection branch
    # should be visible offline — but that branch is almost always one early
    # return, while everything that actually cuts, connects and decomposes hangs
    # off a non-empty selection. An empty fake silently skipped ALL of it: a whole
    # rewritten Q6/Q7 block in ModelGeometryProbe dry-ran "clean" without
    # executing a line. Exercise the risky path; the early return needs no help.
    if command == "API.GetSelectedElements":
        return _ok({"elements": [{"elementId": {"guid": _GUID % 601}},
                                 {"elementId": {"guid": _GUID % 602}}]})

    # Never cancelled: a dry run has no palette and no Stop button. Present so a
    # command that polls (every webui await does) is not reported as calling an
    # unknown verb on every tick.
    if command == "EvP.PollCancel":
        return _ok({"cancelled": False})

    # --- E25 change token ---------------------------------------------------- #
    # The token ADVANCES on every read. That is not realism for its own sake: a
    # command built on evp.changes.wait_for_change() would otherwise spin here
    # until the dry run's own timeout, and "the harness hung" is exactly the
    # verdict that teaches you to stop running it. A monotonically rising token
    # with an idle model means the wait returns on its first or second tick and
    # the command's REACTION — the part worth checking — actually runs.
    if command == "EvP.WatchModel":
        if not params.get("enable", True):
            return _ok({"watching": False, "attached": 0, "token": _change["token"]})
        _change["watching"] = True
        # Background mode returns at once and says nothing about counts — a probe
        # that reads `attached` here must cope with its absence, which is exactly
        # the shape the real command returns.
        if params.get("scope") == "visible" and not params.get("guids"):
            # The visible set is deliberately SMALLER than the database — that is
            # the whole point of the mode, so the fake must not report them equal.
            return _ok({"watching": True, "mode": "visible",
                        "listed": len(_SNAP_GUIDS), "attached": len(_SNAP_GUIDS),
                        "failed": 0, "observed": len(_SNAP_GUIDS),
                        "token": _change["token"], "attachMs": 2, "elapsedMs": 40})
        if not params.get("guids") and not params.get("attachAll"):
            return _ok({"watching": True, "mode": "createOnly", "attached": 0,
                        "token": _change["token"],
                        "note": "handlers installed; NEW elements are reported"})
        if params.get("attachAll"):
            _change["arming"] = True
            return _ok({"watching": True, "mode": "background", "arming": True,
                        "token": _change["token"],
                        "note": "arming in the background"})
        n = len(params.get("guids") or _SNAP_GUIDS)
        return _ok({"watching": True, "scope": params.get("scope", "3d"),
                    "listed": n, "attached": n, "failed": 0, "observed": n,
                    "truncated": False, "cancelled": False,
                    "token": _change["token"],
                    # 0 ms listing on the guids path: naming the elements skips
                    # the listing pass, which is the whole point of that path.
                    "listMs": 0 if params.get("guids") else 3,
                    "attachMs": 1, "elapsedMs": 4})

    # The scalable path: one call, no observers. Stateful enough that a probe's
    # baseline-then-diff sequence behaves like the real thing — the first call
    # reports nothing (baseline), later ones report the elements this run wrote.
    if command == "EvP.GetModelDiff":
        first = not _change.get("diffBaseline") or params.get("reset")
        _change["diffBaseline"] = True
        changed = list(_ids.keys()) or list(_SNAP_GUIDS)
        if first:
            changed = []
        return _ok({"scope": params.get("scope", "3d"), "baseline": bool(first),
                    "new": [], "modified": changed, "deleted": [],
                    "newCount": 0, "modifiedCount": len(changed), "deletedCount": 0,
                    "environmentChanged": False, "elapsedMs": 18})

    if command == "EvP.SyncModel":
        _change["token"] += 1
        return _ok({"token": _change["token"]})

    # ---- ArchViz, the bgfx viewer (bgfx-archviz-plan.md Part I) ------------
    # STATEFUL, and deliberately NOT initialised on the first read: the viewer
    # comes up through a POSTED job, so a probe that polls until `initialized`
    # must actually go round its loop offline instead of falling straight
    # through. Two reads after the open and it is up, which is what the real one
    # does on a warm driver.
    if command == "EvP.OpenViewer":
        _archviz["open"] = True
        _archviz["polls"] = 0
        return _ok({"posted": True})

    if command == "EvP.CloseViewer":
        _archviz["open"] = False
        _archviz["polls"] = 0
        return _ok({"posted": True})

    # ---- Diligent (PLAT-RE19 / PLAT-RE22) ---------------------------------
    # The clear A/B lands a frame AFTER the device is up, so `clearChecked` is
    # false on the poll where `initialized` first turns true. A probe that waits
    # on `initialized` and then reads the verdict would report "no verdict" live;
    # offline it must hit the same thing.
    # EVP_DRYRUN_SCENARIO=diligent_black replays the three-run VirtualBox
    # failure (both arms zero) so the probe's four-way branch is exercised
    # rather than only its happy path.
    if command == "EvP.ProbeDiligentDevice":
        _diligent["deviceAttempts"] += 1
        _diligent["devicePolls"] = 0
        return _ok({"posted": True})

    if command == "EvP.DiligentProbeState":
        if _diligent["deviceAttempts"] > 0:
            _diligent["devicePolls"] += 1
        done = _diligent["deviceAttempts"] > 0 and _diligent["devicePolls"] >= 2
        return _ok({"attempted": _diligent["deviceAttempts"] > 0,
                    "running": _diligent["deviceAttempts"] > 0 and not done,
                    "succeeded": done,
                    "failureMessage": ""})

    if command == "EvP.StartD3D12FeasibilityProbe":
        confirmed = bool(params.get("confirm"))
        if not confirmed or _d3d12["attempted"]:
            return _ok({"started": False,
                        "error": "confirm=true is required" if not confirmed
                                 else "RE51.D1 was already attempted in this process"})
        _d3d12.update({"attempted": True, "running": True, "polls": 0})
        return _ok({"started": True, "error": ""})

    if command == "EvP.D3D12FeasibilityProbeState":
        if _d3d12["running"]:
            _d3d12["polls"] += 1
            if _d3d12["polls"] >= 5:
                _d3d12["running"] = False
        stages = ("starting", "child HWND", "overlay transparent",
                  "overlay half alpha", "complete")
        stage = stages[min(_d3d12["polls"], len(stages) - 1)] if _d3d12["attempted"] else ""
        completed = _d3d12["attempted"] and not _d3d12["running"]
        return _ok({
            "attempted": _d3d12["attempted"], "running": _d3d12["running"],
            "completed": completed, "cancelled": False, "cleanTeardown": completed,
            "stage": stage, "failureMessage": "", "deviceAttempted": _d3d12["attempted"],
            "deviceSucceeded": completed, "deviceFailure": "", "adapter": "Dry-run D3D12",
            "hardwarePreflightSucceeded": completed, "hardwareCreateResult": 0,
            "hardwareFeatureLevel": 0xC100, "d3d12Runtime": r"C:\\Windows\\System32\\d3d12.dll",
            "childAttempted": _d3d12["polls"] >= 1, "childSucceeded": completed,
            "childPresents": 8 if completed else 0, "childLastPresentResult": 0,
            "childFailure": "", "overlayAttempted": _d3d12["polls"] >= 2,
            "overlaySucceeded": completed, "overlayPresents": 16 if completed else 0,
            "overlayLastPresentResult": 0, "overlayFailure": "", "rayTracingFeature": 1,
            "rayTracingCaps": 3, "rayTracingStandalone": True, "rayTracingInline": True,
            "rayTracingIndirect": False, "maxRecursionDepth": 31, "maxRayGenThreads": 1073741824,
        })

    if command == "EvP.StopD3D12FeasibilityProbe":
        _d3d12["running"] = False
        return _ok({"stopped": True})

    if command == "EvP.OpenDiligentViewport":
        _diligent["open"] = True
        _diligent["polls"] = 0
        return _ok({"posted": True})

    if command == "EvP.CloseDiligentViewport":
        # Stateful, like every other write here: a command that closes the
        # viewport and then polls DiligentViewportState must see `running` go
        # false, or its wait loop spins to its timeout in the harness only.
        _diligent["open"] = False
        _diligent["polls"] = 0
        return _ok({"posted": True})

    if command == "EvP.StartDiligentCapture":
        _diligent_capture["id"] += 1
        _diligent_capture["polls"] = 0
        _diligent_capture["width"] = int(params.get("width", 0))
        _diligent_capture["height"] = int(params.get("height", 0))
        _diligent_capture["cancelled"] = False
        _diligent_capture["png"] = _fake_capture_png(
            _diligent_capture["width"], _diligent_capture["height"])
        return _v2({"id": _diligent_capture["id"], "status": "running"})

    if command == "EvP.DiligentCaptureState":
        capture_id = int(params.get("id", 0))
        if capture_id != _diligent_capture["id"]:
            return _err("the Diligent capture id is unknown or has expired")
        _diligent_capture["polls"] += 1
        if _diligent_capture["cancelled"]:
            status, stage = "cancelled", "cancelled"
        elif _diligent_capture["polls"] == 1:
            status, stage = "running", "extracting"
        elif _diligent_capture["polls"] == 2:
            status, stage = "running", "rendering"
        else:
            status, stage = "completed", "completed"
        url = "http://127.0.0.1:19191/screenshot/diligent?id=%d" % capture_id
        return _v2({
            "id": capture_id, "status": status, "stage": stage,
            "width": _diligent_capture["width"], "height": _diligent_capture["height"],
            "bytes": len(_diligent_capture["png"]) if status == "completed" else 0,
            "url": url if status == "completed" else "pending",
            "failureMessage": "",
        })

    if command == "EvP.CancelDiligentCapture":
        capture_id = int(params.get("id", 0))
        running = (capture_id == _diligent_capture["id"] and
                   _diligent_capture["polls"] < 3 and
                   not _diligent_capture["cancelled"])
        _diligent_capture["cancelled"] = running
        return _v2({"cancelled": running})

    if command == "EvP.SetOverlayInstruction":
        # The overlay's HUD banner (PLAT-RE111). Nothing here can show it, but a
        # command that calls it must not be reported as using an unknown verb --
        # and `shown` mirrors the real one so a caller branching on it behaves
        # the same offline.
        return _ok({"shown": bool(params.get("text", ""))})

    if command == "EvP.SetOverlayFrameLatency":
        _diligent["frameLatency"] = int(params.get("frames", 1))
        return _ok({"frames": _diligent["frameLatency"]})

    if command == "EvP.DiligentViewportState":
        if _diligent["open"]:
            _diligent["polls"] += 1
        up = _diligent["open"] and _diligent["polls"] >= 2
        checked = _diligent["open"] and _diligent["polls"] >= 3
        black = _SCENARIO == "diligent_black"
        matched = checked and not black
        report = ("PASS observed=39,221,255,255 expected=39,221,255,255 maxDelta=0"
                  if matched else
                  "FAIL observed=0,0,0,0 expected=39,221,255,255 maxDelta=255"
                  " -- all four bytes are zero (the PLAT-RE22 signature)")
        return _ok({"running": _diligent["open"],
                    # ⚠️ ECHOED FROM THE LAST SetOverlayFrameLatency, not a
                    # constant. The probe now WAITS for the render thread to
                    # acknowledge the setting before it starts measuring, and a
                    # fake that never acknowledges would exercise only the
                    # failure path -- so the harness would pass a probe whose
                    # success path had never run.
                    "frameLatency": _diligent.get("frameLatency", 0),
                    "stalePresents": 0,
                    "presentFailures": 0,
                    "lastPresentResult": 0,
                    "initialized": up,
                    "failed": False,
                    "failureMessage": "",
                    "frames": 240 if up else 0,
                    "fps": 60.0 if up else 0.0,
                    "width": 1280 if up else 0,
                    "height": 720 if up else 0,
                    "resizes": 0,
                    "clearChecked": checked,
                    "diligentClearMatched": matched,
                    "nativeClearMatched": matched,
                    "diligentClearReport": report if checked else "",
                    "nativeClearReport": report if checked else "",
                    "adapter": "NVIDIA GeForce RTX 4070" if up else "",
                    "featureLevel": 45056 if up else 0,   # 0xB000 == 11_0
                    "presentCount": 238 if up else 0,
                    "deviceRemovedReason": 0,
                    # PLAT-RE26: the debug cube, 12 triangles over 24 vertices.
                    # ⚠️ 24, not 8 -- a corner belongs to three faces with three
                    # normals, so a probe that reports "8 vertices" is reporting
                    # a mesh that would shade a flat box like a sphere.
                    "sceneReady": up,
                    # Archicad geometry arrives a poll AFTER the scene is ready:
                    # extraction is a live pass, so "the viewport is up" and "the
                    # building is in it" must stay separable offline as well.
                    "sceneElements": 412 if checked else 0,
                    "sceneTriangles": 214000 if checked else 12,
                    "sceneVertices": 388000 if checked else 24,
                    "sceneGpuBytes": (24 * 1024 * 1024) if checked else 744,
                    "scenePending": 0,
                    "sceneMaterials": 37 if checked else 0,
                    "materialMisses": 0,
                    "transparentRanges": 9 if checked else 0,
                    "sunApplied": checked,
                    "sunBelowHorizon": False,
                    "sunX": -0.41, "sunY": -0.55, "sunZ": 0.73,
                    "ambient": 0.35,
                    "sunAzimuthDegrees": 233.3,
                    "sunBearingDegrees": 216.7,
                    "northDegrees": 90.0,
                    "sunAltitudeDegrees": 46.9,
                    # PLAT-RE32/RE33. The shadow map is created at scene init, so
                    # it is ready as soon as the scene is; the FIT needs geometry,
                    # so it lands a poll later with the elements. Keeping those two
                    # apart here is the point -- the command's diagnostics are
                    # built entirely on telling them apart.
                    "shadowReady": up,
                    "shadowFitted": checked,
                    "shadowResolution": 2048 if up else 0,
                    "environmentLoaded": bool(_diligent.get("environmentLoaded")),
                    "environmentActive": bool(_diligent.get("environmentLoaded"))
                                         and bool(_diligent.get("environmentEnabled", True))
                                         and float(_diligent.get("environmentIntensity", 1.0)) > 0.0,
                    "environmentMipLevels": 10 if _diligent.get("environmentLoaded") else 0,
                    # The real cobblestone probe's measured mean, so a probe that
                    # checks for a black sky is exercised against a plausible one.
                    "environmentAverageR": 0.698594 if _diligent.get("environmentLoaded") else 0.0,
                    "environmentAverageG": 0.667868 if _diligent.get("environmentLoaded") else 0.0,
                    "environmentAverageB": 0.643446 if _diligent.get("environmentLoaded") else 0.0,
                    "environmentPath": _diligent.get("environmentPath", ""),
                    "environmentError": "",
                    "shadowTexelMetres": 0.031 if checked else 0.0,
                    # PLAT-RE36. The viewport reports back whatever was last
                    # pushed, so the overlay sync test measures a residual of
                    # zero offline -- which is the RIGHT fake: the harness proves
                    # the command's control flow and arithmetic, and says nothing
                    # about whether Archicad and Diligent actually agree. That is
                    # the in-Archicad run's job, and the command says so.
                    "cameraEyeX": _diligent["camera"][0],
                    "cameraEyeY": _diligent["camera"][1],
                    "cameraEyeZ": _diligent["camera"][2],
                    "cameraTargetX": _diligent["camera"][3],
                    "cameraTargetY": _diligent["camera"][4],
                    "cameraTargetZ": _diligent["camera"][5],
                    "cameraFovDegreesVertical": 42.0,
                    "cameraSyncs": _diligent["syncs"],
                    "cameraSource": ("floor plan (orthographic)"
                                     if (_diligent["overlay"] and _DILIGENT_PLAN)
                                     else "perspective"),
                    "overlay": _diligent["overlay"],
                    "renderMode": _diligent["renderMode"],
                    "callout": _diligent["callout"],
                    "sunOverridden": _diligent["sunOverride"],
                    # PLAT-RE65. ⚠️ REPORTED FROM WHAT SetPlanAnchors WAS
                    # ACTUALLY GIVEN, not hardcoded. A probe's value here is
                    # telling "the layer holds nothing" apart from "the layer is
                    # switched off", and a fake that always claims geometry
                    # makes both of those branches unreachable in a dry run.
                    "planAnchors": _plan_anchors["enabled"],
                    "planAnchorLayerReady": up,
                    "planAnchorVertices": _plan_anchors["vertices"],
                    "planAnchorWidthPixels": _plan_anchors["widthPixels"],
                    "debugView": _diligent["debugView"]})

    # --- the overlay (PLAT-RE37) and its knobs ------------------------------
    if command == "EvP.OpenDiligentOverlay":
        _diligent["open"] = True
        _diligent["overlay"] = True
        _diligent["polls"] = 0
        return _ok({"posted": True})

    if command == "EvP.CloseDiligentOverlay":
        _diligent["open"] = False
        _diligent["overlay"] = False
        _diligent["polls"] = 0
        return _ok({"posted": True})

    if command == "EvP.DiligentOverlayState":
        # The rect is a real maximised AC29 document canvas, like the plan
        # overlay fixtures above: a synthetic one would let a probe's size and
        # aspect reasoning pass on numbers that cannot occur.
        active = _diligent["overlay"]
        return _ok({"active": active,
                    "width": 1772 if active else 0,
                    "height": 1347 if active else 0,
                    "left": 74 if active else 0,
                    "top": 186 if active else 0,
                    "targetClass": "GXWindowClass" if active else "",
                    "how": ("frontmost document canvas under the centre of Archicad's main "
                            "window, class 'GXWindowClass'" if active else "no overlay"),
                    "trackPolls": 30 * max(_diligent["polls"], 1),
                    "trackMoves": 2})

    if command == "EvP.SetDiligentRenderMode":
        _diligent["renderMode"] = int(params.get("mode", 0))
        return _ok({"mode": _diligent["renderMode"]})

    if command == "EvP.SetDiligentCallout":
        _diligent["callout"] = bool(params.get("enabled"))
        return _ok({"enabled": _diligent["callout"]})

    if command == "EvP.SetDiligentSun":
        _diligent["sunOverride"] = bool(params.get("enabled"))
        _diligent["sunOverrideAz"] = float(params.get("azimuthDegrees", 0.0))
        _diligent["sunOverrideAlt"] = float(params.get("altitudeDegrees", 45.0))
        return _ok({"enabled": _diligent["sunOverride"],
                    "azimuthDegrees": _diligent["sunOverrideAz"],
                    "altitudeDegrees": _diligent["sunOverrideAlt"]})

    if command == "EvP.SetDiligentDebugView":
        _diligent["debugView"] = int(params.get("view", 0))
        return _ok({"view": _diligent["debugView"]})

    if command == "EvP.SetDiligentEnvironmentMap":
        # ⚠️ THE FAKE IS STATEFUL AND MODELS THE DEFERRED LOAD, because that is
        # the part a probe's control flow actually depends on: an empty path
        # UNLOADS, a non-empty one loads, and `environmentActive` needs the
        # enabled flag and the intensity as well -- a fake that just echoed the
        # path would let a probe's "did it come back active" branch pass without
        # ever being exercised.
        path = params.get("path", "")
        enabled = bool(params.get("enabled", True))
        intensity = float(params.get("intensity", 1.0))
        _diligent["environmentPath"] = path
        _diligent["environmentLoaded"] = bool(path)
        _diligent["environmentEnabled"] = enabled
        _diligent["environmentIntensity"] = intensity
        return _ok({
            "requested": path,
            "enabled": enabled,
            "intensity": intensity,
            "rotationDegrees": float(params.get("rotationDegrees", 0.0)),
        })

    if command == "EvP.GetArchicad3DCamera":
        # ⚠️ IT MOVES. A fixed camera would make the overlay sync test report
        # "the camera never moved during the run" every single time, which is the
        # command's INCONCLUSIVE branch -- so the branch that matters would never
        # be exercised offline. The orbit is slow and deterministic.
        _diligent["cameraTick"] += 1
        angle = _diligent["cameraTick"] * 0.05
        radius = 24.0
        return _ok({"valid": True, "source": "perspective",
                    "eyeX": radius * math.cos(angle),
                    "eyeY": radius * math.sin(angle),
                    "eyeZ": 12.0,
                    "targetX": 0.0, "targetY": 0.0, "targetZ": 3.0,
                    "viewConeDegreesHorizontal": 55.0})

    if command == "EvP.SetDiligentCameraSync":
        if params.get("enabled") and not _diligent["open"]:
            return _err("the Diligent viewport is not running")
        _diligent["cameraSync"] = bool(params.get("enabled"))
        return _ok({"enabled": _diligent["cameraSync"],
                    "intervalMs": int(params.get("intervalMs", 33))})

    if command == "EvP.SetCameraSyncMode":
        # ⚠️ THE REFUSALS ARE MODELLED, NOT JUST THE HAPPY PATH. The whole point
        # of the mode switch (PLAT-RE81) is that a request it cannot honour
        # leaves the current mode ALONE, and a probe's `finally` restore depends
        # on that. A fake that always says yes would let a probe that mishandles
        # a refusal dry-run clean.
        requested = str(params.get("mode", "legacy"))
        # ⚠️ EVERY MODE IS BUILT NOW, INCLUDING hookdraw. This list was stale, and
        # a stale refusal is worse than no fake at all: the matrix stopped at the
        # arm call, so target selection, compositing, the gestures, the
        # diagnostics, the restore and the teardown were never once exercised
        # offline while that mode was being developed.
        #
        # EVP_DRYRUN_SCENARIO=hookdraw_fails keeps the old behaviour for the one
        # branch that still needs it -- the probe's own handling of a refusal.
        if requested == "hookdraw" and _HOOKDRAW_FAILS:
            return _err("hookdraw refused; the current mode ('%s') is unchanged"
                        % _diligent["syncMode"])
        if requested != "off" and not _diligent["open"]:
            return _err("no Diligent viewport or overlay is running")
        _diligent["syncMode"] = requested
        _diligent["syncIntervalMs"] = int(params.get("intervalMs",
                                                     _diligent["syncIntervalMs"]))
        _diligent["syncScale"] = float(params.get("predictionScale",
                                                  _diligent.get("syncScale", 1.0)))
        if requested != "hookdraw":
            _diligent["modeStatePolls"] = 0
        _diligent["cameraSync"] = requested != "off"
        return _ok({"mode": _diligent["syncMode"],
                    "intervalMs": _diligent["syncIntervalMs"]})

    if command == "EvP.CameraSyncModeState":
        # ⚠️ THE hookdraw DIAGNOSTICS ARE MODELLED AS A SEQUENCE, not as a fixed
        # answer. The probe waits for `markerTargetChosen` and then for
        # `compositeReady`, and a fake that reports both immediately never
        # exercises either wait -- nor the timeout branches, which are the ones
        # that have actually fired in Archicad.
        state = {"mode": _diligent["syncMode"],
                 "intervalMs": _diligent["syncIntervalMs"],
                 "predictionScale": _diligent.get("syncScale", 1.0),
                 "experimentsBlocked": False,
                 "experimentsBlockedWhy": ""}
        if _diligent["syncMode"] == "hookdraw":
            _diligent["modeStatePolls"] = _diligent.get("modeStatePolls", 0) + 1
            polls = _diligent["modeStatePolls"]
            chosen = polls >= 2 and not _COMPOSITE_FAILS_TARGET
            ready = polls >= 4 and chosen and not _COMPOSITE_FAILS_READY
            state.update({
                "presentCalls": 400 * polls,
                "present1Calls": 0,
                "busiestFrameCount": 200 * polls,
                "markerEnabled": True,
                "markerTargetChosen": chosen,
                "markerDraws": 0 if not chosen else 120 * polls,
                "markerFailures": 0,
                "markerLastError": "",
                "compositeEnabled": True,
                "compositeReady": ready,
                "compositeBlits": 0 if not ready else 150 * polls,
                "compositeFramesConsumed": 0 if not ready else 60 * polls,
                "compositeReprojections": 0 if not ready else 148 * polls,
                "compositeFailures": 0,
                "compositeBackBufferFormat": 87,
                "compositeWidth": 3035,
                "compositeHeight": 1854,
                "compositeLastError": ("" if ready else
                                       "OpenSharedResource1 refused the overlay surface"),
            })
        return _ok(state)

    if command == "EvP.ViewerNavLog":
        _diligent["navLog"] = bool(params.get("enable"))
        if _diligent["navLog"]:
            _diligent["navRows"] = 0
        else:
            # Rows only exist if something marked the run. A run that enables and
            # immediately disables the log really does produce nothing, and a
            # probe that reads a plausible row count out of that would report a
            # measurement it never took.
            _diligent["navRows"] = 40 * max(_diligent["navMarks"], 0)
        return _ok({"running": _diligent["navLog"],
                    "intervalMs": int(params.get("intervalMs",
                                                 _diligent["syncIntervalMs"])),
                    "viewerRows": _diligent["navRows"],
                    "archicadRows": _diligent["navRows"],
                    "archicadFails": 0,
                    "maxArchicadGapMs": 210 if _diligent["navRows"] else 0})

    if command == "EvP.NavLogMark":
        _diligent["navMarks"] += 1
        return _ok({"marked": _diligent["navLog"]})

    if command == "EvP.SetDiligentCamera":
        if not _diligent["open"]:
            return _err("the Diligent viewport is not running")
        _diligent["camera"] = [
            params.get("eyeX", 0.0), params.get("eyeY", 0.0), params.get("eyeZ", 0.0),
            params.get("targetX", 0.0), params.get("targetY", 0.0), params.get("targetZ", 0.0)]
        _diligent["syncs"] += 1
        return _ok({"accepted": True})

    if command == "EvP.GetDiligentCamera":
        if not _diligent["open"]:
            return _err("a visible Diligent viewport with a perspective camera is required")
        camera = _diligent["camera"]
        if not any(camera):
            camera = [24.0, -18.0, 12.0, 4.0, 1.5, 1.5]
        return _v2({
            "valid": True, "source": "viewer", "orthographic": False,
            "viewMoving": False,
            "eyeX": camera[0], "eyeY": camera[1], "eyeZ": camera[2],
            "targetX": camera[3], "targetY": camera[4], "targetZ": camera[5],
            "viewConeDegreesHorizontal": 60.0,
        })

    if command == "EvP.ViewerState":
        if _archviz["open"]:
            _archviz["polls"] += 1
        up = _archviz["open"] and _archviz["polls"] >= 2
        return _ok({"running": _archviz["open"],
                    "initialized": up,
                    "failed": False,
                    "error": "",
                    "renderer": "Direct3D11" if up else "",
                    "width": 1280 if up else 0,
                    "height": 720 if up else 0,
                    "frames": 180 if up else 0,
                    "fps": 60.0 if up else 0.0,
                    "resets": 0,
                    "backends": "bgfx API 153, backends: Direct3D11",
                    # Phase 6: what is on the GPU. Zero until an extraction pass
                    # has run, so "the viewer is up" and "the building is in it"
                    # stay separable offline as well as in Archicad.
                    "elements": 412 if _archviz["extracted"] else 0,
                    "triangles": 214000 if _archviz["extracted"] else 0,
                    "vertices": 388000 if _archviz["extracted"] else 0,
                    "gpuBytes": 24 * 1024 * 1024 if _archviz["extracted"] else 0,
                    "pending": 0,
                    "materials": 37 if _archviz["extracted"] else 0,
                    "materialMisses": 0,
                    "transparentRanges": 9 if _archviz["extracted"] else 0,
                    # ⚠️ The sun the RENDERER is using, not the one the project
                    # has. False here until an extraction pass has run, because
                    # that distinction is the whole diagnosis for "the lighting
                    # does not match Archicad" — a hardcoded default and a real
                    # sun look equally plausible on a building.
                    "pickAvailable": True,
                    "pickSeq": 0,
                    "pickedGuid": "",
                    "selectedCount": 0,
                    "sunApplied": _archviz["extracted"],
                    "sunBelowHorizon": False,
                    "sunX": -0.42, "sunY": 0.31, "sunZ": 0.85,
                    "ambient": 0.35})

    if command == "EvP.ViewerRefresh":
        # Two queries to finish, so a probe's poll loop is actually exercised.
        if params.get("stop"):
            _archviz["live"] = False
            _archviz["extractPolls"] = 99
        elif not params.get("query"):
            _archviz["extractPolls"] = 0
            _archviz["extracted"] = False
            _archviz["live"] = bool(params.get("live"))
            _archviz["passes"] = 0
        else:
            _archviz["extractPolls"] += 1
            # Live mode: a pass lands every few polls, so a probe that waits for
            # one to happen actually goes round its loop instead of falling
            # straight through on a constant.
            if _archviz["live"]:
                _archviz["passes"] += 1
        done = _archviz["extractPolls"] >= 2
        if done:
            _archviz["extracted"] = True
        pushed = 412 if done else 176
        return _ok({"started": not params.get("query"),
                    "running": not done,
                    "done": done,
                    "gaveUp": False,
                    "total": 500, "extracted": pushed, "empty": 88,
                    "pushed": pushed, "materials": 37,
                    "triangles": 214000 if done else 90000,
                    "slices": 61 if done else 24,
                    "longestHoldMs": 11, "longestRoundTripMs": 240,
                    "acquireMs": 1830, "throttledMs": 0,
                    "elapsedMs": 4200 if done else 1600,
                    # Phase 7 — live sync.
                    "live": _archviz["live"],
                    "fullPasses": 1 if done else 0,
                    "partialPasses": max(0, _archviz["passes"] // 3),
                    "removed": 0,
                    "armed": 388 if (done and _archviz["live"]) else 0,
                    "armRefused": 0,
                    # ⚠️ ARMED IS NOT LISTENING. The real failure this models:
                    # AttachObserver links elements, InstallElementObserver is
                    # what delivers. A watch can report hundreds armed and be
                    # deaf, which is how live sync behaved until 2026-08-07.
                    "handlersInstalled": bool(done and _archviz["live"]),
                    "dirtyPending": 0,
                    "lastSyncMs": 640, "lastPassMs": 210,
                    "phase": ("watching" if (done and _archviz["live"])
                              else ("idle" if done else "extracting"))})

    if command == "EvP.ViewerNavLog":
        # The navigation comparison log (ArchViz/NavLog.hpp). Stateful like the
        # rest of this fake, so a probe that arms it and then queries it sees its
        # own effect rather than a constant.
        #
        # ⚠️ maxArchicadGapMs is deliberately answered as a STARVED poll — six
        # times the interval. The real WM_TIMER is low priority and this is the
        # outcome a probe must handle correctly; a fake that always reports a
        # healthy poll would let the "that is a starved poll" branch ship
        # untested, and that branch is the one carrying the warning.
        if not params.get("query"):
            _archviz["navlog"] = bool(params.get("enable", True))
            _archviz["navInterval"] = int(params.get("intervalMs", 50) or 50)
        interval = _archviz.get("navInterval", 50)
        running = _archviz.get("navlog", False)
        return _ok({"running": running,
                    "intervalMs": interval if running else 0,
                    "viewerRows": 1200 if running else 0,
                    "archicadRows": 340 if running else 0,
                    "archicadFails": 0,
                    "maxArchicadGapMs": interval * 6 if running else 0,
                    "path": r"%LOCALAPPDATA%\EvP\logs\archviz_nav.log"})


    if command == "EvP.GetObservedElements":
        # Everything the fake project knows about is watched, INCLUDING whatever
        # the command just wrote — otherwise a probe's "is my target observed?"
        # check answers False for a target the fake itself handed out.
        watched = list(dict.fromkeys(list(_SNAP_GUIDS) + list(_ids.keys())))
        asked = params.get("guids") or []
        out = {"count": len(watched)}
        if asked:
            out["observed"] = [True for _ in asked]
        return _ok(out)

    if command == "EvP.GetChangeToken":
        # Advances, then STOPS. Both halves are load-bearing: a token that never
        # moves makes a wait loop hang until the dry run's own timeout, and one
        # that moves forever means the "nothing more arrived" branch never runs.
        if _change["token"] < 12:
            _change["token"] += 1
        pending = list(_ids.keys()) or list(_SNAP_GUIDS)
        return _ok({"token": _change["token"],
                    "watching": _change["watching"],
                    "watchedCount": len(_SNAP_GUIDS) if _change["watching"] else 0,
                    # Well past any sane `settle`, so the settled branch is the
                    # one exercised rather than "still dragging".
                    "idleMs": 5000,
                    "dirtyCount": 0 if _change["drained"] else len(pending),
                    "arming": False,
                    "armProgress": {"running": False, "done": True,
                                    "listed": len(_SNAP_GUIDS), "attached": len(_SNAP_GUIDS),
                                    "failed": 0, "slices": 2, "listMs": 12,
                                    "longestHoldMs": 7, "longestRoundTripMs": 190,
                                    "gaveUp": False, "elapsedMs": 60,
                                    "phase": "idle"}})

    # The queue drain. STATEFUL: a drain empties the queue, so an `updates()`
    # loop yields ONE batch and then waits, exactly as it would live. A fake
    # that kept handing out the same batch would make a correct consumer look
    # like an infinite loop.
    if command == "EvP.TakeChanges":
        pending = [] if _change["drained"] else (list(_ids.keys()) or list(_SNAP_GUIDS))
        taken = pending[:max(0, int(params.get("max", 500)))]
        if not params.get("peek"):
            _change["drained"] = True
        return _ok({"guids": taken, "events": ["change"] * len(taken),
                    "count": len(taken), "remaining": 0, "overflowed": False,
                    "peeked": bool(params.get("peek")),
                    "token": _change["token"], "idleMs": 5000})

    # The scalable path: one call, no observers. Stateful enough that a probe's
    # baseline-then-diff sequence behaves like the real thing — the first call
    # reports nothing (baseline), later ones report the elements this run wrote.
    if command == "EvP.GetModelDiff":
        first = not _change.get("diffBaseline") or params.get("reset")
        _change["diffBaseline"] = True
        changed = list(_ids.keys()) or list(_SNAP_GUIDS)
        if first:
            changed = []
        return _ok({"scope": params.get("scope", "3d"), "baseline": bool(first),
                    "new": [], "modified": changed, "deleted": [],
                    "newCount": 0, "modifiedCount": len(changed), "deletedCount": 0,
                    "environmentChanged": False, "elapsedMs": 18})

    if command == "EvP.SyncModel":
        _change["token"] += 1
        return _ok({"token": _change["token"]})

    if command == "EvP.GetObservedElements":
        # Everything the fake project knows about is watched, INCLUDING whatever
        # the command just wrote — otherwise a probe's "is my target observed?"
        # check answers False for a target the fake itself handed out.
        watched = list(dict.fromkeys(list(_SNAP_GUIDS) + list(_ids.keys())))
        asked = params.get("guids") or []
        out = {"count": len(watched)}
        if asked:
            out["observed"] = [True for _ in asked]
        return _ok(out)

    if command == "EvP.GetChangeToken":
        # Advances, then STOPS. Both halves are load-bearing: a token that never
        # moves makes a wait loop hang until the dry run's own timeout, and one
        # that moves forever means the "nothing more arrived, window closed"
        # branch never runs — and a probe that watches for 25s would burn all 25
        # offline. Twelve bumps is enough to exercise a multi-round watcher.
        if _change["token"] < 12:
            _change["token"] += 1
        state = {"token": _change["token"],
                 "watching": _change["watching"],
                 "watchedCount": len(_SNAP_GUIDS) if _change["watching"] else 0,
                 # Well past any sane `settle`, so the settle branch is the one
                 # exercised rather than the "still dragging" one.
                 "idleMs": 5000,
                 "arming": False,
                 # A finished pass, with a slice time that would mean no stutter.
                 "armProgress": {"running": False, "done": True,
                                 "listed": len(_SNAP_GUIDS), "attached": len(_SNAP_GUIDS),
                                 "failed": 0, "slices": 2, "listMs": 12,
                                 "longestHoldMs": 7, "longestRoundTripMs": 190,
                                 "gaveUp": False, "elapsedMs": 60,
                                 "phase": "idle"}}
        since = params.get("since")
        if since is not None:
            # The elements this run actually wrote, if any — the same
            # consistent-world rule the rest of the fakes follow. A probe that
            # writes an element and then asks "which changed?" must see ITS
            # element come back, or it reports a mismatch that only the harness
            # caused. Falls back to the snapshot guids when nothing was written.
            changed = list(_ids.keys()) or list(_SNAP_GUIDS)
            entries = changed[:max(0, int(params.get("maxGuids", 200)))]
            state["changedCount"] = len(entries)
            state["complete"] = True
            if params.get("includeGuids", True):
                state["guids"] = entries
                state["events"] = ["change"] * len(entries)
        return _ok(state)

    # The 3D window's camera. PERSPECTIVE, because that is the branch with the
    # unit trap in it: `viewCone` is a HORIZONTAL field of view in DEGREES, and
    # a consumer that reads it as vertical or as radians still produces a
    # plausible-looking frame. The numbers frame a small model from the
    # south-east, so a viewer that applies them lands somewhere sensible.
    if command == "EvP.Get3DProjection":
        return _ok(dict(_projection, ok=True))

    # ⚠️ THE WRITE IS STATEFUL, AND IT HAS TO BE. A probe that poses the camera
    # and then reads it back is checking that its own sweep moved anything at
    # all; a fake that accepted the write and kept answering with the same
    # numbers would make every pose in the sweep identical and every hypothesis
    # score the same, which reads as "the maths is wrong".
    if command == "EvP.Set3DProjection":
        before = dict(_projection)
        if "azimuthDelta" in params:
            _projection["azimuth"] += float(params["azimuthDelta"])
        for key in ("azimuth", "distance", "cameraZ", "targetZ", "viewCone"):
            if key in params:
                _projection[key] = float(params[key])
        return _ok({"changed": True, "before": before, "after": dict(_projection)})

    if command == "EvP.Get3DWindowSets":
        # 1280x720 logical at a 1.0 DPI factor: the canvas rect the window chain
        # reports matches, so a probe measuring G8 gets 1.0 and a probe that has
        # its DPI arithmetic backwards still gets 1.0 — the harness cannot decide
        # that one, and says so rather than inventing a factor.
        return _ok({"hSize": 1280, "vSize": 720,
                    "zoomScaleX": 1.0, "zoomScaleY": 1.0,
                    "zoomDispX": 0.0, "zoomDispY": 0.0})

    if command == "EvP.ModelToScreen":
        # ⚠️ AN INDEPENDENT IMPLEMENTATION, ON PURPOSE. This projects with the
        # convention the plan believes is right — Z-up, right-handed lookAt,
        # viewCone as the HORIZONTAL fov in degrees, y flipped for a top-left
        # origin — written out here rather than imported from the command, so a
        # probe scoring hypotheses against it is compared with something that
        # does not share its bugs. H2 should win offline; if it does not, the
        # probe's maths is wrong and that is exactly what this catches.
        w, h = 1280.0, 720.0
        eye = (_projection["posX"], _projection["posY"], _projection["cameraZ"])
        tgt = (_projection["targetX"], _projection["targetY"], _projection["targetZ"])
        f = [tgt[i] - eye[i] for i in range(3)]
        fl = math.sqrt(sum(c * c for c in f)) or 1.0
        f = [c / fl for c in f]
        up = (0.0, 0.0, 1.0)
        r = [up[1] * f[2] - up[2] * f[1],
             up[2] * f[0] - up[0] * f[2],
             up[0] * f[1] - up[1] * f[0]]
        rl = math.sqrt(sum(c * c for c in r)) or 1.0
        r = [c / rl for c in r]
        u = [f[1] * r[2] - f[2] * r[1],
             f[2] * r[0] - f[0] * r[2],
             f[0] * r[1] - f[1] * r[0]]
        aspect = w / h
        cone = math.radians(_projection["viewCone"])
        tan_h = math.tan(cone * 0.5)
        tan_v = tan_h / aspect
        # ⚠️ TWO ARRAYS, BECAUSE THE REAL CALL IS TWO CALLS. ACAPI_View_ModelToScreen
        # yields an API_Coord in DRAWING METRES; only ACAPI_View_CoordToPoint turns
        # that into pixels. A one-array fake taught the opposite: three real runs of
        # ModelOverlayMatrix scored four camera hypotheses against metres, every RMS
        # landed at 726-754 px (= sqrt(640^2+360^2), the distance from the canvas
        # centre to the origin), and the "winner" flipped run to run. The harness
        # reproduces the SHAPE so a consumer that confuses them fails here instead.
        rng = math.sqrt(sum((eye[i] - tgt[i]) ** 2 for i in range(3))) or 1.0
        px_per_m = w / (2.0 * rng * tan_h)
        out, mid = [], []
        for p in params.get("points", []):
            if len(p) < 3:
                refused = {"ok": False, "why": "a point needs three coordinates"}
                out.append(refused)
                mid.append(dict(refused))
                continue
            d = [p[i] - eye[i] for i in range(3)]
            z = sum(d[i] * f[i] for i in range(3))
            if z <= 1e-6:
                refused = {"ok": False, "why": "behind the camera"}
                out.append(refused)
                mid.append(dict(refused))
                continue
            x = sum(d[i] * r[i] for i in range(3))
            y = sum(d[i] * u[i] for i in range(3))
            ndc_x = (x / z) / tan_h
            ndc_y = (y / z) / tan_v
            px = (ndc_x * 0.5 + 0.5) * w
            py = (1.0 - (ndc_y * 0.5 + 0.5)) * h
            # The drawing coordinate this pixel came from: centre-origin metres,
            # which is the ~±5 cluster the real call returns for points straddling
            # the target by 5 m. Two orders of magnitude off the pixel, as observed.
            mid.append({"ok": True,
                        "x": (px - w * 0.5) / px_per_m,
                        "y": (py - h * 0.5) / px_per_m})
            # ⚠️ API_Point is two SHORTS. A point that projects far off-screen wraps
            # rather than refusing, so the fake wraps too — a consumer that trusts
            # ok:true without checking the window rect meets that here, not in
            # Archicad.
            out.append({"ok": True,
                        "x": float(((int(px) + 32768) % 65536) - 32768),
                        "y": float(((int(py) + 32768) % 65536) - 32768)})
        return _ok({"windowType": 3, "is3DWindow": True,
                    "count": len(out),
                    "failed": sum(1 for e in out if not e["ok"]),
                    "screen": out,
                    "coord": mid})

    # ---- the native sun study -------------------------------------------
    #
    # A SESSION, so the fake has to be one too. The whole point of the command
    # split is that a study is advanced across many calls; a fake that returned
    # "converged" to the first Advance would let a caller with a broken loop
    # pass, and a fake that never converged would hang one that works. So this
    # keeps a tiny amount of state and counts down exactly like the real thing.
    if command == "EvP.StartSunStudy":
        step_minutes = max(1, int(params.get("timestep", 60)))
        hour_from = int(params.get("hourFrom", 0))
        hour_to = int(params.get("hourTo", 24))
        steps = max(1, (hour_to - hour_from) * 60 // step_minutes)

        # ⚠️ THE HORIZON FILTER MUST MATCH THE ONE THE FAKE GetPlaceInfo IMPLIES,
        # or a caller that enumerates the day itself and then asks the native
        # core for the same day sees two different step counts. The sun study
        # command compares exactly that and refuses to run natively when they
        # disagree -- so a fake whose two sun sources contradict each other
        # sends every offline run down the fallback path, and the native path
        # is then never exercised offline at all. That is how this was found.
        min_alt = float(params.get("minAltitudeDeg", 0.0))
        above = 0
        for index in range(steps):
            minute_of_day = hour_from * 60 + index * step_minutes
            hour = minute_of_day // 60
            if math.degrees(math.radians(max(-10.0, 55.0 - abs(hour - 12) * 9.0))) > min_alt:
                above += 1
        above = max(1, above)
        grid = float(params.get("grid", 2.0))
        columns = max(1, int(40.0 / grid) + 1)

        # ⚠️ samples='explicit' MEANS THE CALLER'S POINTS, so the fake must
        # count THOSE and not the grid it would have built. A fake that keeps
        # inventing its own sample count returns a result of the wrong length,
        # and the caller can only report a size mismatch -- which looks exactly
        # like a defect in the command under test.
        explicit = params.get("positions") or []
        packed_positions = params.get("positionsPacked") or ""
        if str(params.get("samples", "surfaces")) == "explicit":
            if packed_positions:
                # base64 of float64 xyz triples: 24 bytes a sample.
                sample_count = len(base64.b64decode(packed_positions)) // 24
            elif explicit:
                sample_count = len(explicit) // 3
            else:
                sample_count = columns * columns
        else:
            sample_count = columns * columns
        _SUN_STUDY.clear()
        _SUN_STUDY.update({"id": "sun-1", "total": above, "resolved": 0,
                           "samples": sample_count, "ms": 0.0})
        return _v2({"studyId": "sun-1",
                    "resolvedSteps": 0, "totalSteps": above,
                    "sampleCount": sample_count, "generation": 1,
                    "converged": False, "empty": False,
                    "daylightHours": above * int(params.get("timestep", 60)) / 60.0,
                    "sourceStepCount": steps,
                    "gridColumns": columns, "gridRows": columns,
                    "groundZ": 0.1, "groundPad": 30.0,
                    "sampleMode": str(params.get("samples", "surfaces")),
                    "undersizedFaces": 0, "degenerateFaces": 0,
                    "closedGroups": 1, "flippedGroups": 0,
                    "latitude": 54.6872, "longitude": 25.2797, "northDeg": 0.0,
                    "year": int(params.get("year", 2026)),
                    "month": int(params.get("month", 3)),
                    "day": int(params.get("day", 21)),
                    "timestep": int(params.get("timestep", 60))})

    if command == "EvP.AdvanceSunStudy":
        if not _SUN_STUDY:
            return _v2({"studyId": "", "advanced": 0, "resolvedSteps": 0,
                        "totalSteps": 0, "converged": False, "empty": True})
        remaining = _SUN_STUDY["total"] - _SUN_STUDY["resolved"]
        advanced = min(remaining, max(1, int(params.get("maxSteps", 4))))
        _SUN_STUDY["resolved"] += advanced
        _SUN_STUDY["ms"] += advanced * 1.5
        return _v2({"studyId": _SUN_STUDY["id"], "advanced": advanced,
                    "resolvedSteps": _SUN_STUDY["resolved"],
                    "totalSteps": _SUN_STUDY["total"],
                    "sampleCount": _SUN_STUDY["samples"], "generation": 1,
                    "converged": _SUN_STUDY["resolved"] >= _SUN_STUDY["total"],
                    "empty": False,
                    "analysisMilliseconds": _SUN_STUDY["ms"]})

    if command == "EvP.SunStudyState":
        if not _SUN_STUDY:
            return _v2({"studyIds": [], "studyCount": 0, "studyId": "", "live": False})
        return _v2({"studyIds": [_SUN_STUDY["id"]], "studyCount": 1,
                    "studyId": _SUN_STUDY["id"], "live": True,
                    "resolvedSteps": _SUN_STUDY["resolved"],
                    "totalSteps": _SUN_STUDY["total"],
                    "sampleCount": _SUN_STUDY["samples"], "generation": 1,
                    "converged": _SUN_STUDY["resolved"] >= _SUN_STUDY["total"],
                    "empty": False,
                    "analysisMilliseconds": _SUN_STUDY["ms"]})

    if command == "EvP.GetSunStudyResults":
        if not _SUN_STUDY:
            return _v2({"studyId": "", "hours": [], "count": 0,
                        "converged": False, "empty": True})
        count = _SUN_STUDY["samples"]
        span = _SUN_STUDY["total"] * 1.0
        # A gradient with genuinely shaded and genuinely lit samples, so the
        # command's own "every sample is identical" checks are exercised rather
        # than trivially satisfied.
        hours = [0.0 if i % 7 == 0 else span * ((i % 5) / 4.0) for i in range(count)]
        out = {"studyId": _SUN_STUDY["id"], "hours": hours, "count": count,
               "resolvedSteps": _SUN_STUDY["resolved"],
               "totalSteps": _SUN_STUDY["total"],
               "sampleCount": count, "generation": 1,
               "converged": _SUN_STUDY["resolved"] >= _SUN_STUDY["total"],
               "empty": False}
        if params.get("includePositions"):
            _pos = [float((i % 20) - 10) if a == 0 else
                    (float((i // 20) - 10) if a == 1 else 0.1)
                    for i in range(count) for a in range(3)]
            if params.get("packed"):
                out["positionsPacked"] = base64.b64encode(
                    struct.pack("<%dd" % len(_pos), *_pos)).decode("ascii")
                out["normalsPacked"] = base64.b64encode(struct.pack(
                    "<%dd" % (count * 3),
                    *[0.0 if a < 2 else 1.0 for _ in range(count)
                      for a in range(3)])).decode("ascii")
                return _v2(out)
            out["positions"] = _pos
            # Normals travel with positions in the real command, so they do
            # here: a caller that reproduces the back-face cull needs both, and
            # a fake that ships one without the other lets that mistake pass.
            out["normals"] = [0.0 if a < 2 else 1.0
                              for _ in range(count) for a in range(3)]
        if params.get("includeSteps"):
            steps = _SUN_STUDY["total"]
            want_packed = bool(params.get("packed"))
            # ⚠️ SAMPLE-MAJOR, AND THE BITS MUST AGREE WITH `hours` ABOVE. A
            # caller cross-checks one against the other, so a fake whose two
            # answers contradict each other would fail a command that is right.
            bits = []
            for i in range(count):
                lit = int(round((hours[i] / span) * steps)) if span > 0 else 0
                bits.extend([1] * lit + [0] * (steps - lit))
            out["stepStride"] = steps
            if want_packed:
                # ⚠️ ONE BIT PER FLAG, LSB FIRST, exactly as the native command
                # packs it. A fake that answered with a byte per flag would let
                # a caller's unpacking bug through: it would read eight times
                # too much data and still find plausible values.
                blob = bytearray((len(bits) + 7) // 8)
                for i, bit in enumerate(bits):
                    if bit:
                        blob[i // 8] |= 1 << (i % 8)
                out["stepBitsPacked"] = base64.b64encode(bytes(blob)).decode("ascii")
            else:
                out["stepBits"] = bits
        return _v2(out)

    if command == "EvP.CancelSunStudy":
        erased = 1 if _SUN_STUDY else 0
        _SUN_STUDY.clear()
        return _v2({"studyId": params.get("studyId", ""), "erased": erased})

    # Vilnius, midday-ish — real-looking numbers in the DevKit's MIXED units
    # (lat/long degrees, north/sun radians), because a command that converts is
    # the one thing this fake can usefully break.
    if command == "EvP.GetPlaceInfo":
        # SELF-CONSISTENT, in both the ways a consumer checks. The degree fields
        # are DERIVED from the radians (a fake that rounded them independently
        # made a correct probe report a unit mismatch), and the sun MOVES with
        # the requested hour (a constant one made a correct probe report that the
        # timestamp override was ignored). Neither is realism for its own sake —
        # both are checks a real command makes, and a fake that fails them
        # teaches you to stop reading the harness.
        hour = int(params.get("hour", 12))
        sun_z = math.radians(max(-10.0, 55.0 - abs(hour - 12) * 9.0))
        sun_xy = math.radians(180.0 + (hour - 12) * 15.0)
        return _v2({"longitude": 25.2797, "latitude": 54.6872, "altitude": 112.0,
                    "north": 0.0, "northDeg": 0.0,
                    "sunAngXY": sun_xy, "sunAngZ": sun_z,
                    "sunAngXYDeg": math.degrees(sun_xy),
                    "sunAngZDeg": math.degrees(sun_z),
                    "year": int(params.get("year", 2026)),
                    "month": int(params.get("month", 6)),
                    "day": int(params.get("day", 21)),
                    "hour": int(params.get("hour", 12)),
                    "minute": int(params.get("minute", 0)),
                    "second": int(params.get("second", 0)),
                    "sunDirX": math.cos(sun_z) * math.cos(sun_xy),
                    "sunDirY": math.cos(sun_z) * math.sin(sun_xy),
                    "sunDirZ": math.sin(sun_z),
                    # Compass bearing = north - sunAngXY (SETTLED against NOAA at
                    # two north values). north is 0 here, so a regression to the
                    # tempting `90 - sunAngXY` shows up as an 90 deg error offline.
                    "sunAzimuthDeg": (0.0 - math.degrees(sun_xy)) % 360.0,
                    "sunAltitudeDeg": math.degrees(sun_z),
                    "summerTime": True, "timeZoneInMinutes": 120,
                    "timeOverridden": any(k in params for k in
                                          ("year", "month", "day", "hour", "minute", "second"))})

    # The 3D VIEW's own sun, which is NOT GetPlaceInfo's (PLAT-RE67). The fake
    # deliberately answers a DIFFERENT date from the place fake above — the two
    # disagreeing is the ordinary state of a real project, and a probe that
    # silently assumed they matched would pass against a fake that agreed.
    if command == "EvP.GetViewSunInfo":
        sun_z = math.radians(28.1)
        sun_xy = math.radians(211.8)
        return _v2({"isPersp": True, "sunPositionMode": "byDate",
                    # RAW, and in RADIANS here purely as one plausible reading:
                    # the DevKit states no unit, so a consumer that prints both
                    # readings is right and one that picks a unit is guessing.
                    "sunAzimuthRaw": sun_xy, "sunAltitudeRaw": sun_z,
                    "viewYear": 2017, "viewMonth": 3, "viewDay": 22,
                    "viewHour": 10, "viewMinute": 0, "viewSecond": 0,
                    "viewSummerTime": False,
                    "north": 0.0, "northDeg": 0.0,
                    "computedSunAngXY": sun_xy, "computedSunAngZ": sun_z,
                    "computedSunAngXYDeg": math.degrees(sun_xy),
                    "computedSunAngZDeg": math.degrees(sun_z),
                    "computedSunDirX": math.cos(sun_z) * math.cos(sun_xy),
                    "computedSunDirY": math.cos(sun_z) * math.sin(sun_xy),
                    "computedSunDirZ": math.sin(sun_z),
                    "computedAzimuthDeg": (0.0 - math.degrees(sun_xy)) % 360.0,
                    "computedAltitudeDeg": math.degrees(sun_z)})

    if command == "EvP.GetStatus":
        return _v2({"serverRunning": True, "port": 19723, "modelOpen": True,
                    "snapshotId": 0, "retainedBytes": 0})

    # --- properties ------------------------------------------------------- #
    # Stateful, like the element-ID family: a value written here reads back, so a
    # command that verifies its own write is testing its comparison rather than
    # this fake's guess.
    # The response key is `properties`, NOT `propertyIds` — evp.properties.ids
    # reads the former, and a fake that spelled it the other way would make
    # builtin_id() raise KeyError on a perfectly correct command.
    # The propertyId a name resolves to is DERIVED FROM THE NAME and remembered,
    # so GetPropertyValuesOfElements below can answer per property. Handing out
    # positional guids (the old behaviour) made every property indistinguishable,
    # so a command reading area + category got the same fake for both and could
    # not be exercised at all.
    if command == "API.GetPropertyIds":
        out = []
        for entry in params.get("properties", []) or [{}]:
            if entry.get("type") == "UserDefined":
                name = "/".join(str(p) for p in entry.get("localizedName", []))
            else:
                name = str(entry.get("nonLocalizedName", ""))
            guid = _GUID % (500 + _property_slot(name))
            _property_names[guid] = name
            out.append({"propertyId": {"guid": guid}})
        return _ok({"properties": out})

    # Values keyed by the property NAME, so a zone really does come back with a
    # net area and a Lithuanian zone category — the two reads mode 4's daylight
    # ratio is built on. An unknown property answers `userUndefined`, which is
    # also the shape a command must survive.
    if command == "API.GetPropertyValuesOfElements":
        pids = [(p.get("propertyId") or {}).get("guid", "")
                for p in params.get("properties", []) or []]
        per_element = []
        for i, entry in enumerate(params.get("elements", []) or []):
            guid = (entry.get("elementId") or {}).get("guid", "")
            values = []
            for pid in pids:
                value = _property_value_for(_property_names.get(pid, ""), guid, i)
                values.append({"propertyValue": {"type": "string", "status": "normal",
                                                 "value": value}}
                              if value is not None else
                              {"propertyValue": {"type": "string",
                                                 "status": "userUndefined"}})
            per_element.append({"propertyValues": values})
        return _ok({"propertyValuesForElements": per_element})

    if command == "API.SetPropertyValuesOfElements":
        results = []
        for item in params.get("elementPropertyValues", []) or []:
            guid = (item.get("elementId") or {}).get("guid", "")
            value = (item.get("propertyValue") or {}).get("value", "")
            _ids[guid] = value
            results.append({"success": True})
        return _ok({"executionResults": results})

    if command == "API.Get3DBoundingBoxes":
        boxes = []
        for entry in params.get("elements", []) or []:
            guid = (entry.get("elementId") or {}).get("guid", "")
            cx, cy, cz = _POLY_BBOX.get(guid, (0.0, 0.0, 1.5))
            boxes.append({"boundingBox3D": {
                "xMin": cx - 1.0, "xMax": cx + 1.0,
                "yMin": cy - 1.0, "yMax": cy + 1.0,
                "zMin": cz - 1.0, "zMax": cz + 1.0}})
        return _ok({"boundingBoxes3D": boxes})

    # --- 2D drafting ------------------------------------------------------- #
    # Per-item results, not one flat ok: EvP.CreateText reports each text on its
    # own, and a caller that reads the batch as a single boolean must fail here.
    if command == "EvP.CreateText":
        items = params.get("texts", []) or []
        guids = [_GUID % (750 + i) for i in range(len(items))]
        return _ok({"results": [{"ok": True, "guid": g} for g in guids],
                    "guids": guids, "count": len(guids)})

    # --- geometry snapshot / slice ------------------------------------------ #
    if command == "EvP.BuildSnapshot":
        # ⚠️ THE KEY NAMES ARE THE REAL COMMAND'S. This answered `elements` /
        # `triangles` while SnapshotCommands.cpp returns `elementCount` /
        # `triangleCount` / `meshCount`, so a probe reading the real names got
        # None here and concluded the snapshot was empty — a false negative that
        # looks exactly like a project with no 3D. Both spellings are sent now;
        # the real ones are what a new command should read.
        mesh_count = 1 if _SCENARIO == "topography" else len(_SNAP_GUIDS)
        return _v2({"elementCount": mesh_count, "vertexCount": 24 * mesh_count,
                    "triangleCount": 12 * mesh_count, "snapshotId": 1,
                    "scope": params.get("scope", "all"), "hasMetadata": False,
                    "metaLevel": "none", "metadataCancelled": False,
                    "retainedBytes": 4096})

    # ⚠️ The mesh table is what makes evp.geometry.snapshot() return anything.
    # Without `guids`/`vertexCounts`/`triangleCounts` the Snapshot has ZERO
    # meshes, so every command that loops over `snap.meshes` dry-ran clean while
    # executing none of its own serialisation — the same class of false pass the
    # empty-selection fake used to cause. The old `elements`/`triangles`/`live`
    # keys are kept alongside for anything reading the raw response.
    if command == "EvP.GetSnapshotInfo":
        guids = [_SNAP_GUIDS[0]] if _SCENARIO == "topography" else list(_SNAP_GUIDS)
        elem_types = [18] if _SCENARIO == "topography" else [1, 10]
        if _SCENARIO == "roofs":
            # _SNAP_GUIDS[0] is the poly roof, so it must report as one (ModelerAPI
            # type 3) — otherwise the fallback rejects it as "not a roof" and the
            # measure path still never runs.
            elem_types = [3, 10]
        return _v2({"meshCount": len(guids),
                    "snapshotId": 1,
                    "scope": "selection",
                    "guids": guids,
                    "elemTypes": elem_types,
                    "vertexCounts": [24] * len(guids),
                    "triangleCounts": [12] * len(guids),
                    "retainedBytes": 4096})

    if command == "EvP.ReleaseSnapshot":
        return _v2({"freedBytes": 4096, "retainedBytes": 0})

    # --- raycasting ---------------------------------------------------------- #
    #
    # ⚠️ ONE ENTRY PER RAY, always. `ray_all_batch` walks `hitCounts` in step with
    # the origins it sent, so a fake that returns a short list silently drops
    # rays and the caller reshapes an array of the wrong size — which is a
    # confusing ValueError deep in numpy, nowhere near the cause. Every ray here
    # pierces one synthetic slab: an ENTER at 0.4 m and an EXIT at 2.6 m, so a
    # floor/ceiling reader gets a plausible ~2.2 m clear height instead of a
    # degenerate zero.
    if command in ("EvP.RaycastAllBatch", "EvP.RaycastAll", "EvP.Raycast"):
        if command == "EvP.RaycastAllBatch":
            origins = params.get("origins") or []
            ray_count = len(origins) // 3
        else:
            ray_count = 1
        ts, enters, guids, types, points, normals = [], [], [], [], [], []
        for r in range(ray_count):
            ox, oy, oz = (origins[r * 3:r * 3 + 3] if command == "EvP.RaycastAllBatch"
                          else params.get("origin", [0.0, 0.0, 0.0]))
            for t, entering, nz in ((0.4, True, -1.0), (2.6, False, 1.0)):
                ts.append(t)
                enters.append(entering)
                guids.append(_SNAP_GUIDS[r % len(_SNAP_GUIDS)])
                types.append(45)
                points.extend([ox, oy, oz + t])
                normals.extend([0.0, 0.0, nz])
        if command == "EvP.Raycast":
            return _ok({"hit": True, "t": ts[0], "guid": guids[0], "elemType": types[0],
                        "point": points[0:3], "normal": normals[0:3]})
        data = {"t": ts, "enter": enters, "guids": guids, "elemTypes": types,
                "points": points, "normals": normals}
        if command == "EvP.RaycastAllBatch":
            data["hitCounts"] = [2] * ray_count
            data["truncated"] = [False] * ray_count
        else:
            data["truncated"] = False
        return _ok(data)

    # --- PLAT-9 the two catalogue reads behind evp.LibraryPart / evp.Favourite -- #
    #
    # Both HONOUR THEIR FILTERS here rather than returning one fixed list, because
    # the filter is the thing a command's own logic reacts to: a probe that checks
    # "did the subtype filter let a non-door through" must be able to see the
    # answer offline. A canned list that ignored `subtype` would report a passing
    # filter no matter what the native command does.
    if command == "EvP.ListLibraryParts":
        # ⚠️ OPENINGS ARE REFUSED, and the harness has to refuse them too — the
        # probe asserts that a door request comes back as a SENTENCE rather than
        # as an empty list. A canned response that quietly returned [] would make
        # the probe's own refusal check pass while proving nothing.
        #
        # The refusal is spelled out HERE rather than left to the input schema on
        # purpose: `subtype` is a plain string precisely so that the message says
        # what to write instead. See ResolveSubtype in LibraryObjectCommands.cpp.
        subtype = (params.get("subtype") or "Object")
        if subtype.lower() in ("door", "window", "skylight"):
            return _err("subtype \"%s\" is not available from this picker. An opening "
                        "is cut into a host wall or roof, so choosing one needs that "
                        "host's context rather than a list of library parts. Use "
                        "\"Object\" (the default), \"Lamp\", \"ZoneStamp\", \"Label\", "
                        "or \"all\"." % subtype)

        catalog = [
            {"name": "Chair 29", "file": "Chair 29.gsm", "unID": "{1D0C4BF0-1}",
             "type": "Object", "location": r"C:\Lib\Chair 29.gsm",
             "placeable": True, "missing": False,
             "treePath": ["Loaded Libraries", "Object Library", "1. BASIC LIBRARY", "1.1 Furnishing", "Chairs"]},
            {"name": "Slope Symbol 29", "file": "Slope Symbol 29.gsm", "unID": "{1D0C4BF0-2}",
             "type": "Object", "location": r"C:\Lib\Slope Symbol 29.gsm",
             "placeable": True, "missing": False,
             "treePath": ["Loaded Libraries", "Object Library", "1. BASIC LIBRARY", "1.7 2D Elements"]},
            # Embedded, and at the library's top level: the shortest tree path a
            # real project produces, so a picker that assumed >= 2 levels breaks
            # here rather than in Archicad.
            {"name": "My Object", "file": "My Object.gsm", "unID": "{1D0C4BF0-3}",
             "type": "Object", "location": "", "placeable": True, "missing": False,
             "treePath": ["Embedded Library"]},
            {"name": "Ceiling Light 29", "file": "Ceiling Light 29.gsm", "unID": "{1D0C4BF0-4}",
             "type": "Lamp", "location": r"C:\Lib\Ceiling Light 29.gsm",
             "placeable": True, "missing": False,
             "treePath": ["Loaded Libraries", "MEP Library", "3. Electrical"]},
        ]
        if subtype.lower() != "all":
            catalog = [p for p in catalog if p["type"].lower() == subtype.lower()]
        wanted = (params.get("nameFilter") or "").lower()
        if wanted:
            catalog = [p for p in catalog
                       if wanted in p["name"].lower() or wanted in p["file"].lower()]
        total = len(catalog)
        limit = int(params.get("limit") or 2000)
        return _ok({"parts": catalog[:limit], "total": total, "truncated": total > limit})

    if command == "EvP.GetLibraryPartPreviewInfo":
        # ⚠️ ANSWERS "GIF" HERE ON PURPOSE, and therefore decodable=False. The
        # section the preview lives in is NAMED for GIF and NativeImage decodes
        # only JPEG/PNG, so GIF is the case that BLOCKS thumbnails — which makes
        # it the case a probe reading this offline must exercise. Whether the
        # stock library actually stores GIF is unknown and is exactly what the
        # in-Archicad run settles; the harness must not pre-answer it optimistically.
        name = params.get("name") or ""
        return _ok({"name": name, "previewMime": "image/gif",
                    "previewBytes": 2048, "decodable": False})

    if command == "EvP.ListFavorites":
        catalog = [
            {"name": "Exterior 300", "elementType": "Wall", "folder": ["Walls", "Exterior"]},
            {"name": "Partition 100", "elementType": "Wall", "folder": ["Walls"]},
            # Root-level, so the empty folder path is exercised too.
            {"name": "Chair", "elementType": "Object", "folder": []},
        ]
        wanted_type = (params.get("elementType") or "").lower()
        if wanted_type:
            catalog = [f for f in catalog if f["elementType"].lower() == wanted_type]
        wanted = (params.get("nameFilter") or "").lower()
        if wanted:
            catalog = [f for f in catalog if wanted in f["name"].lower()]
        return _ok({"favorites": catalog, "total": len(catalog)})

    if command == "EvP.GetProjectInfo":
        # The custom plot-area field is there on purpose: it is the field
        # MassingFeasibility's tapioca.ProjectField picker defaults to, and its
        # value is written the way a user really types one (comma decimal, spaced
        # thousands) so mc.parse_number is exercised rather than bypassed.
        #
        # ⚠️ TWO FIELDS SHARE A DESCRIPTION PREFIX, deliberately. "Sklypo plotas,
        # m2" and "Sklypo plotas bendras" both contain the substring a readable
        # default spells, and they are ordered so a name-substring match lands on
        # the FIRST — which is the right one only by luck. A caller that resolves by
        # KEY gets the field it asked for either way, and that is the difference
        # this fixture exists to catch.
        return _v2({"projectName": "Dry Run Project", "projectPath": r"C:\dryrun\project.pln",
                    "untitled": False,
                    "fieldNames": ["Project Name", "Sklypo plotas, m2",
                                   "Sklypo plotas bendras", "Client"],
                    "fieldKeys": ["PROJECTNAME", "SKLYPOPLOTAS", "SKLYPOPLOTASBENDRAS",
                                  "CLIENT"],
                    "fieldValues": ["Dry Run Project", "4 520,00 m2", "9 999,00 m2",
                                    "Dry Run Client"],
                    "count": 4})

    if command == "EvP.GetStories":
        return _v2({"firstStory": 0, "lastStory": 2, "actStory": 0,
                    "indices": [0, 1, 2],
                    "names": ["Ground floor", "1st floor", "Roof"],
                    "levels": [0.0, 3.1, 6.2], "count": 3})

    if command == "EvP.SliceZ":
        # A single square loop per requested guid, in the FLAT PARALLEL ARRAY form
        # the native command really emits (coords xyz-interleaved + per-loop point
        # counts) — the shape evp.geometry.slice_z has to unpack.
        # ⚠️ No `guids` means the WHOLE MODEL, which is how a viewer or an
        # overlay slices a storey. Returning nothing for that case made every
        # such command report "0 regions" offline and look like a working
        # command with an empty project.
        z = float(params.get("z", 0.0))
        guids = params.get("guids") or list(_SNAP_GUIDS)
        coords, counts, closed, loop_guids, loop_types = [], [], [], [], []
        for guid in guids:
            cx, cy, _cz = _POLY_BBOX.get(guid, (0.0, 0.0, 0.0))
            for x, y in ((cx - 1, cy - 0.5), (cx + 1, cy - 0.5),
                         (cx + 1, cy + 0.5), (cx - 1, cy + 0.5)):
                coords.extend([x, y, z])
            counts.append(4)
            closed.append(True)
            loop_guids.append(guid)
            loop_types.append(45)
        return _ok({"coords": coords, "loopPointCounts": counts, "loopClosed": closed,
                    "loopGuids": loop_guids, "loopElemTypes": loop_types})

    # --- E24 structured model reads (evp.model) ------------------------------ #
    #
    # ONE synthetic body, and it is deliberately the awkward case: a square face
    # with a square HOLE in it. Polygon 1 therefore carries the contour-break
    # convention (a ZERO edge index between the two rings), which is the single
    # thing evp.model.polygon_loops exists to handle and the single thing a
    # simpler fixture would never exercise. A probe that reassembles contours
    # here fails offline instead of drawing a wall with a bite out of it.
    if command == "EvP.GetModelInfo":
        return _ok({"guid": "DRYRUN-MODEL", "generated": True,
                    "elementCount": 2, "colorCount": 4,
                    "materialCount": 2, "textureCount": 1, "fillCount": 3, "lightCount": 3,
                    "bounds": {"xMin": 0.0, "yMin": 0.0, "zMin": 0.0,
                               "xMax": 4.0, "yMax": 4.0, "zMax": 3.0}})

    if command == "EvP.GetModelElements":
        rows = [{"index": 1, "elementId": {"guid": _GUID % 601}, "type": 1, "typeName": "wall",
                 "invalid": False, "genId": 1, "tessellatedBodyCount": 1,
                 "meshBodyCount": 1, "nurbsBodyCount": 0, "pointCloudCount": 0,
                 "lightCount": 0,
                 "bounds": {"xMin": 0.0, "yMin": 0.0, "zMin": 0.0,
                            "xMax": 4.0, "yMax": 0.3, "zMax": 3.0}},
                {"index": 2, "elementId": {"guid": _GUID % 602}, "type": 10, "typeName": "shell",
                 "invalid": False, "genId": 1, "tessellatedBodyCount": 1,
                 "meshBodyCount": 1, "nurbsBodyCount": 1, "pointCloudCount": 0,
                 "lightCount": 0,
                 "bounds": {"xMin": 0.0, "yMin": 0.0, "zMin": 2.0,
                            "xMax": 4.0, "yMax": 4.0, "zMax": 3.0}}]
        # ⚠️ THE walls SCENARIO'S WALLS MUST BE ENUMERABLE HERE TOO, or a command
        # that says "every wall on the storey" finds none of the walls the rest
        # of this harness knows about — and dry-runs clean over an empty set,
        # which is the least useful pass available.
        if _SCENARIO == "walls":
            rows = [{"index": 10 + i, "elementId": {"guid": guid}, "type": 1, "typeName": "wall",
                     "invalid": False, "genId": 1, "tessellatedBodyCount": 1,
                     "meshBodyCount": 1, "nurbsBodyCount": 0, "pointCloudCount": 0,
                     "lightCount": 0,
                     "bounds": {"xMin": 0.0, "yMin": 0.0, "zMin": 0.0,
                                "xMax": 6.0, "yMax": 4.0, "zMax": 3.0}}
                    for i, guid in enumerate(_WALL_DETAILS)]
        wanted = params.get("guids") or []
        if wanted:
            rows = [r for r in rows if r["guid"] in wanted]
        types = params.get("types") or []
        if types:
            rows = [r for r in rows if r["typeName"] in types]
        return _ok({"totalCount": len(rows), "count": len(rows), "offset": 0,
                    "modelElementCount": 2, "generated": True,
                    "coordinateSystem": "world", "elements": rows})

    if command == "EvP.GetBodyGeometry":
        # Outer ring vertices 1-4, hole ring 5-8. Edge index 0 at the corner
        # BETWEEN them is the contour break.
        return _ok({
            "guid": params.get("guid") or _GUID % 601,
            "elementIndex": 1, "elementType": "wall", "source": params.get("source", "tessellated"),
            "bodyIndex": 1, "bodyCount": 1, "coordinateSystem": "world",
            "body": {"isWireBody": False, "isSurfaceBody": False, "isSolidBody": True,
                     "isClosed": True, "hasSharpEdge": True, "vertexCount": 8,
                     "edgeCount": 8, "polygonCount": 1, "polygonVectorCount": 1,
                     "hasColor": False, "materialIndex": {"index": 1},
                     "bounds": {"xMin": 0.0, "yMin": 0.0, "zMin": 0.0,
                                "xMax": 4.0, "yMax": 0.0, "zMax": 3.0}},
            "vertices": [0.0, 0.0, 0.0,  4.0, 0.0, 0.0,  4.0, 0.0, 3.0,  0.0, 0.0, 3.0,
                         1.0, 0.0, 1.0,  2.0, 0.0, 1.0,  2.0, 0.0, 2.0,  1.0, 0.0, 2.0],
            "verticesTruncated": False,
            "polygons": {"count": 1, "truncated": False, "skipped": 0,
                         "materialIndex": [1], "normalVectorIndex": [1], "polygonId": [1],
                         "invisible": [False], "visibleIfContour": [False],
                         "isComplex": [True], "isGravity": [False],
                         "hasMaterialTexture": [False], "hasPolygonTexture": [False],
                         "materialTextureIndex": [-1], "polygonTextureIndex": [-1],
                         "edgeCounts": [9],
                         "vertexIndices": [1, 2, 3, 4, 0, 5, 6, 7, 8],
                         "edgeIndices":   [1, 2, 3, 4, 0, 5, 6, 7, 8]},
            "edges": {"count": 8, "truncated": False,
                      "vertex1": [1, 2, 3, 4, 5, 6, 7, 8],
                      "vertex2": [2, 3, 4, 1, 6, 7, 8, 5],
                      "polygon1": [1, 1, 1, 1, 1, 1, 1, 1],
                      "polygon2": [-1, -1, -1, -1, -1, -1, -1, -1],
                      "invisible": [False] * 8, "visibleIfContour": [False] * 8,
                      "hasColor": [False] * 8, "colorIndex": [1] * 8},
            "convex": {"count": 1, "polygonIndex": [1], "vertexCounts": [4],
                       "vertexIndices": [1, 2, 3, 4],
                       "normals": [0.0, -1.0, 0.0] * 4}})

    if command == "EvP.GetModelMaterials":
        return _ok({"materialCount": 2, "count": 2, "materials": [
            # hasTexture on the FIRST one, pointing at the texture pool below:
            # a fake whose texture pool nothing references means the whole
            # texture-fetch path never runs.
            #
            # ⚠️ `textureIndex` is an OBJECT, not an int. AttributeIndexToObjectState
            # (ModelAccessUtils.cpp) emits every AttributeIndex as
            # {index, originalModelerIndex, originalIndex, valid}, and that is true
            # of textureIndex/materialIndex/fillIndex/colorIndex everywhere, while
            # `modelIndex` — added by hand as the pool ordinal — really is a bare
            # int. A fake that spelled this as `1` shipped a TypeError to Archicad:
            # the command dry-ran clean and then died on int(dict) on its first real
            # run. Keep the two kinds spelled differently here; that difference is
            # the whole value of the fixture.
            {"modelIndex": 1, "name": "Concrete", "typeName": "matte", "type": 2,
             "surfaceColor": {"red": 0.6, "green": 0.6, "blue": 0.6},
             "transparency": 0.0, "hasTexture": True,
             "textureIndex": {"index": 1, "originalModelerIndex": 1,
                              "originalIndex": 1, "valid": True},
             "fillIndex": {"index": 3, "originalModelerIndex": 3,
                           "originalIndex": 3, "valid": True},
             "textureName": "brick.jpg", "shining": 400.0,
             "specularReflection": 0.1},
            {"modelIndex": 2, "name": "Glass - Clear", "typeName": "glass", "type": 5,
             "surfaceColor": {"red": 0.8, "green": 0.9, "blue": 1.0},
             "transparency": 0.85, "hasTexture": False}]})

    if command == "EvP.GetModelColors":
        return _ok({"colorCount": 1, "count": 1,
                    "colors": [{"modelIndex": 1, "red": 0.0, "green": 0.0, "blue": 0.0}]})

    if command == "EvP.GetModelTextures":
        return _ok({"textureCount": 1, "count": 1, "textures": [
            {"modelIndex": 1, "used": True, "name": "brick.jpg", "available": True,
             "pixelMapXSize": 512, "pixelMapYSize": 512, "xSize": 1.0, "ySize": 1.0,
             "checksum": "dryrun", "fingerprint": "dryrun", "bumpMapPattern": False}]})

    if command == "EvP.GetTexturePixels":
        width = int(params.get("width") or 2)
        height = int(params.get("height") or 2)
        # The CAP, modelled: truncation is by whole ROWS so the result stays a
        # decodable rectangle. A fake that never truncates cannot tell a probe's
        # cap check from a broken cap, and a probe that always reports SUSPECT
        # offline is one nobody reads.
        cap = int(params.get("maxPixels") or 0)
        truncated = False
        if cap and width * height > cap:
            height = max(1, cap // max(1, width))
            truncated = True
        # Opaque ARGB: byte 0 is the constant alpha, bytes 1..3 the colour.
        return _ok({"name": "brick.jpg", "pixelMapXSize": 512, "pixelMapYSize": 512,
                    "x": 0, "y": 0, "width": width, "height": height,
                    "truncated": truncated,
                    "pixels": [255, 128, 64, 32] * (width * height)})

    if command == "EvP.GetModelLights":
        # skippedCount is present and ZERO on the happy path, deliberately: a
        # caller that only ever sees the key missing will never handle it, and
        # this is the field that says "the list is shorter than the model's
        # lightCount because some lights could not be described".
        # Shaped like EVERY real project observed so far, because the shape is the
        # thing a caller gets wrong: Light.hpp fixes AMBIENT=1, CAMERA=2, SUN=3, so
        # the MODEL-scope list's first three entries ARE the specials, and with
        # `specials` on each one appears twice under two scopes. GetModelInfo's
        # lightCount (3 here) counts the model-scope list ONLY — a probe comparing
        # it against the total would count the specials twice.
        def _light(scope, index, name, type_id, direction, shadow):
            return {"scope": scope, "lightIndex": index, "type": type_id,
                    "typeName": name, "castsShadow": shadow,
                    "color": {"red": 1.0, "green": 1.0, "blue": 1.0},
                    "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                    "direction": direction,
                    "upVector": {"x": 0.0, "y": 0.0, "z": 1.0}, "radius": 0.0,
                    "parameters": []}

        zero = {"x": 0.0, "y": 0.0, "z": 0.0}
        sun_dir = {"x": -0.5, "y": -0.5, "z": -0.707}
        lights = [_light("model", 1, "ambient", 9700, zero, False),
                  _light("model", 2, "camera", 9800, zero, False),
                  _light("model", 3, "sun", 9500, sun_dir, True)]
        if params.get("specials", True):
            lights += [_light("ambient", 9700, "ambient", 9700, zero, False),
                       _light("camera", 9800, "camera", 9800, zero, False),
                       _light("sun", 9500, "sun", 9500, sun_dir, True)]
        # skippedCount is present and ZERO on the happy path, deliberately: a
        # caller that only ever sees the key missing will never handle it, and
        # this is the field that says "the list is shorter than the model's
        # lightCount because some lights could not be described".
        return _ok({"coordinateSystem": "world", "count": len(lights),
                    "skippedCount": 0, "lights": lights})

    if command == "EvP.GetTextureCoordinates":
        n = len(params.get("points") or []) // 3
        return _ok({"guid": params.get("guid"), "bodyIndex": 1,
                    "polygonIndex": params.get("polygon"), "count": n,
                    "u": [0.25 * i for i in range(n)], "v": [0.5] * n})

    if command == "EvP.GetNurbsBody":
        return _ok({
            "guid": params.get("guid"), "elementIndex": 2, "elementType": "shell",
            "bodyIndex": 1, "bodyCount": 1, "coordinateSystem": "world",
            "body": {"vertexCount": 4, "edgeCount": 4, "trimCount": 4, "loopCount": 1,
                     "faceCount": 1, "shellCount": 0, "lumpCount": 0,
                     "curve2DCount": 4, "curve3DCount": 4, "surfaceCount": 1,
                     "material": {"name": "Concrete"}},
            "faces": {"count": 1, "shellIndex": [-1], "surfaceIndex": [0],
                      "loopCounts": [1], "loopIndices": [0], "material": [1],
                      "segmentationPen": [1], "textureCoordSys": [{"modeName": "box"}]},
            "loops": {"count": 1, "faceIndex": [0], "trimCounts": [4],
                      "trimIndices": [0, 1, 2, 3], "trimReversed": [False] * 4},
            "surfaces": [{"degreeU": 3, "degreeV": 3, "rational": False,
                          "controlPointUCount": 4, "controlPointVCount": 4,
                          "controlPoints": [0.0] * 48, "knotsU": [], "knotsV": [],
                          "weights": []}]})

    if command == "EvP.GetPointClouds":
        return _ok({"coordinateSystem": "world", "count": 0, "pointClouds": []})

    if command == "EvP.Get3DComponentCounts":
        return _ok({"bodyCount": 2, "lightCount": 1, "materialCount": 2})

    if command == "EvP.GetElement3DInfo":
        return _ok({"count": len(params.get("guids") or []), "elements": [
            {"guid": g, "found": True, "firstBody": 1, "lastBody": 1, "bodyCount": 1,
             "firstLight": 0, "lastLight": -1, "lightCount": 0,
             "bounds": {"xMin": 0.0, "yMin": 0.0, "zMin": 0.0,
                        "xMax": 4.0, "yMax": 0.3, "zMax": 3.0}}
            for g in (params.get("guids") or [])]})

    if command == "EvP.GetBodyComponents":
        # The SAME square-with-a-hole face, in the C API's polyEdge form: the
        # polygon spans polyEdges 1..9 and polyEdge 5 is the zero contour break.
        return _ok({
            "bodyIndex": params.get("body", 1),
            "body": {"index": 1, "elemIndexPlus1": 1, "bodyIndexPlus1": 1,
                     "parentGuid": _GUID % 601, "status": 1, "isClosed": True,
                     "isCurved": False, "multiMaterial": False, "multiColor": False,
                     "multiTexture": False, "color": 1, "materialIndex": 1,
                     "polygonCount": 1, "polyEdgeCount": 9, "edgeCount": 8,
                     "vertexCount": 8, "vectorCount": 1,
                     "transform": [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                                   0.0, 0.0, 1.0, 0.0]},
            "vertices": [0.0, 0.0, 0.0,  4.0, 0.0, 0.0,  4.0, 0.0, 3.0,  0.0, 0.0, 3.0,
                         1.0, 0.0, 1.0,  2.0, 0.0, 1.0,  2.0, 0.0, 2.0,  1.0, 0.0, 2.0],
            "vectors": [0.0, -1.0, 0.0],
            "polygons": {"count": 1, "materialIndex": [1], "normalIndex": [1],
                         "firstPolyEdge": [1], "lastPolyEdge": [9], "status": [0x20],
                         "invisible": [False], "curved": [False], "concave": [False],
                         "hasHoles": [True], "holesConvex": [True]},
            "polyEdges": {"count": 9, "edge": [1, 2, 3, 4, 0, 5, 6, 7, 8]},
            "edges": {"count": 8,
                      "vertex1": [1, 2, 3, 4, 5, 6, 7, 8],
                      "vertex2": [2, 3, 4, 1, 6, 7, 8, 5],
                      "polygon1": [1] * 8, "polygon2": [-1] * 8,
                      "color": [1] * 8, "status": [0] * 8,
                      "invisible": [False] * 8, "curved": [False] * 8},
            "materials": [{"index": 1, "fromGDL": False, "name": "Concrete"}]})

    if command == "EvP.DecomposePolygon":
        return _ok({"polygonIndex": params.get("polygon"), "subPolygonCount": 2,
                    "declaredSubPolygonCount": 2, "vertexCounts": [4, 4],
                    "vertexIndices": [1, 2, 6, 5, 3, 4, 8, 7]})

    if command == "EvP.GetTextureCoordAtPoint":
        # The SAME UVs as EvP.GetTextureCoordinates above. The dry-run body has an
        # identity transform, so world and local points coincide and the two routes
        # MUST agree — that agreement is what the probe's Q9 checks, and a fake that
        # returned different constants would report a permanent false mismatch.
        n = len(params.get("points") or []) // 3
        return _ok({"count": n, "u": [0.25 * i for i in range(n)], "v": [0.5] * n})

    if command == "EvP.GetCutPolygons":
        return _ok({"elemIdx": 0, "separateComponents": params.get("separateComponents", False),
                    "plane": params.get("plane", {}), "totalArea": 1.2, "bodyCount": 1,
                    "bodies": [{"bodyIdx": 0, "ok": True, "area": 1.2, "polygonCount": 1,
                                "polygons": [{"contourCount": 1, "contourVertexCounts": [4],
                                              "uv": [0.0, 0.0, 4.0, 0.0, 4.0, 0.3, 0.0, 0.3],
                                              "coords": [0.0, 0.0, 1.0, 4.0, 0.0, 1.0,
                                                         4.0, 0.3, 1.0, 0.0, 0.3, 1.0],
                                              "area": 1.2, "perimeter": 8.6}]}]})

    if command == "EvP.GetBodyBuildingMaterials":
        return _ok({"count": len(params.get("guids") or []), "elements": [
            {"guid": g, "found": True, "elemIdx": 0, "modelIndex": 1, "bodyCount": 2,
             "bodies": [{"bodyIdx": 0, "ok": True, "attributeIndex": "1", "name": "Concrete"},
                        {"bodyIdx": 1, "ok": True, "attributeIndex": "2", "name": "Insulation"}]}
            for g in (params.get("guids") or [])]})

    if command == "EvP.GetConnectionTable":
        guids = params.get("guids") or []
        if len(guids) < 2:
            return _ok({"elementCount": len(guids), "pairCount": 0, "connections": []})
        return _ok({"elementCount": len(guids), "pairCount": 1, "connections": [
            {"guid1": guids[0], "guid2": guids[1], "polygonCount": 1,
             "polygons": [{"vertexCount": 4,
                           "coords": [0.0, 0.0, 0.0, 4.0, 0.0, 0.0,
                                      4.0, 0.3, 0.0, 0.0, 0.3, 0.0],
                           "plane": {}}]}]})

    if command == "EvP.GetErrorTrail":
        return _ok({"total": 0, "entries": []})

    # --- Plan overlay probe (Win32 window tree) -----------------------------
    #
    # Rects, classes and styles are the REAL ones from a maximised AC29 floor
    # plan (logs/plan_overlay_probe-enum.log), not invented: the whole point of
    # this probe is the shape of the chain, and a synthetic shape would let the
    # tree-walking and the verdict logic pass on data that cannot occur.
    if command in ("EvP.ProbeWindowAt", "EvP.EnumChildWindows",
                   "EvP.ProbeOverlayShow", "EvP.ProbeOverlayState",
                   "EvP.ProbeOverlayHide", "EvP.ProbeSetClipSiblings",
                   "EvP.ProbeForceRedraw", "EvP.OverlayTransform",
                   "EvP.OverlayTrackStats", "EvP.SetOverlayGeometry",
                   "EvP.SetOverlayTracking", "EvP.OverlayCalibrate"):
        return _plan_overlay(command, params)

    # UI verbs have nothing to return; they exist so a refusal path can run to
    # completion offline instead of stopping at "no canned response".
    if command in ("EvP.SetTracing", "EvP.CheckCancel", "EvP.SetStatus",
                   "EvP.ShowAlert", "EvP.ShowResults", "EvP.ShowResultText"):
        return _ok({})

    unknown.add(command)
    # Loud, not silent: an unknown command returning a bare success would let a
    # misspelled command name look like it worked.
    return _ok({"__dryrun_unknown__": True})


# --------------------------------------------------------------------------- #
#  Web UI palette — the pair that would otherwise HANG the harness             #
# --------------------------------------------------------------------------- #
#
# ⚠️ This is not decoration. `webui.await_result` polls until a submit arrives or
# its timeout expires, and a command's timeout is measured in MINUTES (the model
# viewer asks for 900 s). With these two verbs unknown, every poll came back
# not-ok, the loop slept 300 ms and went round again, and the dry run sat there
# for the command's full timeout instead of finishing — which reads exactly like
# a hung command and is the reason nobody dry-ran a webui command before.
#
# So: the FIRST poll after a show reports a submit, as if the user pressed the
# page's primary button immediately. The nonce is lifted out of the HTML that was
# shown, because `await_result` DROPS a payload whose nonce does not match, and a
# fake that invented one would loop forever while looking like it answered.
_SCRIPTUI = {"nonce": None, "pending": []}


def _scriptui(command, params):
    if command == "Tapir.ShowScriptUI":
        html = params.get("htmlContent") or ""
        # Both page shapes spell it as a 16-hex string: `window.EVP_NONCE = "..."`
        # in a hand-built viewer, `window.__EVPDATA__ = {"nonce": "..."}` in
        # webui.report/page.
        match = re.search(r'"([0-9a-f]{16})"', html)
        _SCRIPTUI["nonce"] = match.group(1) if match else None
        # A page that can post diagnostics DOES post them, before anything else —
        # otherwise the command's handling of that payload never runs offline, and
        # a receiver that mishandles it (or, worse, treats it as "the user closed
        # the viewer" and returns) passes the dry run. Detected from the page
        # rather than assumed, so a page without the feature still sees exactly
        # one `closed`.
        _SCRIPTUI["pending"] = []
        if "'diagnostics'" in html or '"diagnostics"' in html:
            _SCRIPTUI["pending"].append({
                "action": "diagnostics", "booted": True, "failed": False,
                "userAgent": "dryrun/1.0",
                "lines": ["   0.00s  boot: page parsed, handlers installed",
                          "   0.30s  module: three.js r186 imported",
                          "   0.45s  render: first frame drawn"],
            })
        _SCRIPTUI["pending"].append({"action": "closed"})
        return _ok({"success": True})

    if command == "Tapir.GetScriptUIResult":
        nonce = _SCRIPTUI["nonce"]
        if nonce is None or not _SCRIPTUI["pending"]:
            return _ok({"hasResult": False})
        payload = dict(_SCRIPTUI["pending"].pop(0), nonce=nonce, __dryrun__=True)
        return _ok({"hasResult": True, "result": json.dumps(payload)})
    return None


def transport(command, params_json):
    params = json.loads(params_json)
    seen.append(command)

    # ⚠️ EVP_DRYRUN_CANCEL_AFTER=N REFUSES EVERY CALL FROM THE Nth ON, exactly as
    # the dispatcher does after a Stop -- INCLUDING the ones a `finally` block
    # makes, which is the part that surprises. A probe that arms an experimental
    # mode has to survive this without leaving the mode armed, and the only way
    # to know it does is to make it happen. `evp.api.call` turns this code into
    # `evp.Cancelled` when raise_on_error is on, and a command that passes
    # raise_on_error=False everywhere will NOT see it -- which is itself the
    # defect this scenario was written to catch.
    if _CANCEL_AFTER and len(seen) > _CANCEL_AFTER:
        return json.dumps(_err("the run was cancelled", code="Cancelled"))

    # The dispatcher validates BEFORE the command runs, so this must too --
    # otherwise the harness answers a call Archicad would have rejected.
    rejected = _validate_input(command, params)
    if rejected is not None:
        return json.dumps(rejected)

    # A transaction is the interesting case: replay each recorded step through the
    # same canned responses, so Handle.result() gets a REAL per-step payload and a
    # wrapper that misreads it fails here.
    if command == "Tapioca.CommitTransaction":
        results = []
        for step_json in params.get("steps", []):
            step = json.loads(step_json)
            inner = step.get("command", "")
            seen.append("  [tx] " + inner)

            # The STRUCTURAL refusal, mirrored from the native dispatcher. Those
            # commands are non-undoable data-structure modifiers that Archicad
            # refuses inside an undo scope, and the real ReplayBatch rejects the
            # step by name before running it. Faked here so a command that
            # wrongly puts one in a transaction fails OFFLINE, where it is cheap
            # — without this the harness accepted it and the mistake survived to
            # the in-Archicad run.
            if inner.replace("Tapioca.", "EvP.", 1) in _STRUCTURAL:
                return json.dumps({
                    "ok": False,
                    "error": {"kind": "CommandFailed", "message":
                              "%s is a STRUCTURAL command and cannot run inside evp.transaction: "
                              "Archicad refuses non-undoable data-structure modifiers while an undo "
                              "scope is open. Call it directly with evp.api.call instead." % inner,
                              "command": command},
                    "meta": {"backend": "dryrun", "duration_ms": 0.1}})

            step_params = step.get("params") or {}
            # Steps are dispatched individually by ReplayBatch, so each is
            # validated individually too.
            step_rejected = _validate_input(inner, step_params)
            if step_rejected is not None:
                return json.dumps(step_rejected)
            results.append(json.dumps(_one(inner, step_params).get("data") or {}))
        return json.dumps(_ok({"results": results, "count": len(results)}))

    return json.dumps(_one(command, params))


def load(folder):
    path = os.path.join(folder, "command.py")
    if not os.path.isfile(path):
        raise SystemExit("no command.py in %s" % folder)
    # Exactly what both runners do — own folder, `_lib/`, opted-in siblings — via
    # the one implementation. Doing it by hand here meant a command that imports a
    # shared module could not be dry-run at all, so the check that catches runtime
    # attribute errors was unavailable to precisely the commands with the most
    # sharing. No deactivate: this process runs one command and exits.
    from evp import _commandpath
    _commandpath.activate(folder)
    spec = importlib.util.spec_from_file_location("dryrun_cmd", path)
    module = importlib.util.module_from_spec(spec)
    # ⚠️ REGISTER BEFORE EXEC, the way both real runners do (EvPPy.cpp's
    # _evp_load and _evp_external_main both set sys.modules[name] first). Without
    # it, a command whose Pydantic models are written under
    # `from __future__ import annotations` cannot be validated at all: pydantic
    # resolves a stringified annotation through sys.modules[cls.__module__], and
    # an unregistered module has no entry there. It failed here while working in
    # Archicad — the exact divergence this harness exists to prevent.
    sys.modules["dryrun_cmd"] = module
    spec.loader.exec_module(module)
    return module


def main():
    # The Windows console is cp1252 and this file prints warning signs — and a
    # command's own transcript is routinely Lithuanian. Without this the harness
    # died with a UnicodeEncodeError while printing its SUCCESS summary, turning a
    # clean run into a non-zero exit for a purely cosmetic reason.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    # ⚠️ `name=value` OVERRIDES, BECAUSE DEFAULTS-ONLY LEAVES BRANCHES UNREACHED.
    # Every parameter had to take its default, so a command whose interesting
    # behaviour lives behind a non-default mode was exercised only down its
    # dullest path -- `hookdraw` was never once run offline while it was being
    # built. Values are parsed as int, then float, then left as a string, which
    # covers every annotation type the scanner accepts.
    overrides = {}
    args = []
    for argument in sys.argv[1:]:
        if argument == "--twice":
            continue
        if "=" in argument and not os.path.exists(argument):
            key, _, value = argument.partition("=")
            for cast in (int, float):
                try:
                    overrides[key] = cast(value)
                    break
                except ValueError:
                    continue
            else:
                overrides[key] = value
            continue
        args.append(argument)
    twice = "--twice" in sys.argv
    if len(args) != 1:
        raise SystemExit(__doc__)
    folder = os.path.abspath(args[0])

    evp_api._transport = lambda: transport          # the one seam
    module = load(folder)

    fn = getattr(module, "run", None)
    if fn is None or getattr(fn, "__evp_command__", None) is None:
        raise SystemExit("no run() decorated with @evp.command in %s" % folder)

    # --twice: run it AGAINST THE STATE THE FIRST RUN LEFT.
    #
    # ⚠️ A WHOLE CLASS OF DEFECT IS INVISIBLE TO A SINGLE RUN, and it cost a live
    # run to learn: LayoutApiProbe2 Q8 deliberately leaves a fixed-name 3D document
    # behind for the user to look at, so its SECOND run hit a duplicate name and
    # died — after passing every offline gate, because the collision only exists
    # between runs. Anything that leaves an artefact, claims find-or-create, or
    # writes a named thing should be checked with this. The fake's state is
    # process-global and deliberately NOT reset between the two passes; that is
    # the entire point.
    runs = 2 if twice else 1
    print("=" * 70)
    print("DRY RUN  %s%s%s" % (os.path.basename(folder),
                               "  (x2, shared state)" if twice else "",
                               ("  " + " ".join("%s=%s" % kv for kv in overrides.items()))
                               if overrides else ""))
    print("=" * 70)
    for attempt in range(1, runs + 1):
        if twice:
            print("\n" + "-" * 70)
            print("PASS %d of %d" % (attempt, runs))
            print("-" * 70)
        try:
            # Through evp._invoke, the same door the embedded and external
            # runners use, so a schema-style command is validated here exactly
            # as it will be in Archicad. Signature-style commands still land on
            # `fn(**overrides)` inside it -- every param has a default; see
            # `overrides`.
            from evp import _invoke
            _invoke.invoke(fn, overrides, folder=folder)
        except evp_api.Cancelled:
            # ⚠️ CANCELLATION IS A PASS, NOT A FAILURE, and treating it as one
            # would make the fault-injection scenario useless. `evp.Cancelled`
            # propagating out of run() is precisely the designed behaviour --
            # the palette reports "cancelled" and writes no traceback. What is
            # worth checking is what the `finally` blocks MANAGED to do on the
            # way out, so those calls are listed.
            print(chr(10) + "=" * 70)
            print("CANCELLED after %d call(s) — this is the DESIGNED outcome." % len(seen))
            print("=" * 70)
            teardown = [name for name in seen[_CANCEL_AFTER:]
                        if not name.startswith("  [tx]")]
            print("calls attempted after the cancel (the `finally` path):")
            for name in dict.fromkeys(teardown):
                print("     %s" % name)
            if not teardown:
                print("     (none — nothing tried to clean up, which is the bug)")
            return 0
        except Exception:
            print("\n" + "=" * 70)
            print("FAILED on pass %d — this is a real defect, not a fake-wire artefact:"
                  % attempt)
            print("=" * 70)
            traceback.print_exc()
            return 1

    print("\n" + "=" * 70)
    print("OK — ran to completion. %d bus call(s)." % len(seen))
    if unknown:
        print("\n⚠️  commands with no canned response (add them, or check the name):")
        for name in sorted(unknown):
            print("     %s" % name)
    print("⚠️  Only the WIRE was faked. Parameters WERE checked against the real")
    print("    input schemas, so a rejected enum or an out-of-range number would have")
    print("    failed above — but whether Archicad ACTS on them as intended is still")
    print("    an open question, and that is what the in-Archicad probe answers.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
