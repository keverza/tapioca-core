"""Layer 1 — the explicit, envelope-based command bus.

    res = evp.api.call("EvP.GetStatus")
    res.ok        # bool
    res.data      # dict — response payload, schema per command
    res.error     # None | {"code", "message", "detail"}
    res.meta      # backend, zone, duration_ms, main_thread_ms, api_version, call_id

Namespaced command names state the backend explicitly — no hidden routing:
    Tapioca.* native add-on commands, in-process (canonical from API 2)
    EvP.*    native add-on commands, in-process (legacy compatibility spelling)
    API.*    Archicad's official JSON API, executed in-process through the gate
    Tapir.*  an installed Tapir, proxied via API.ExecuteAddOnCommand, until absorbed

Two transports, one API. Embedded (Zone B) calls the builtin `_evp` module; external
(Zone C, `runtime="external"`) POSTs the identical envelope to the add-on over
loopback HTTP. Both reach the same dispatcher, so a script cannot tell them apart —
that transparency is a design requirement. Ask `evp.runtime.mode` if you need to know
for diagnostics; never branch on it for behaviour.
"""

import json as _json
import os as _os

API_VERSION = "2.0.0"
NATIVE_NAMESPACE = "Tapioca"
LEGACY_NATIVE_NAMESPACE = "EvP"

_debug = False


def _make_transport():
    """Pick the transport for this process. Embedded wins when available.

    The choice is by CAPABILITY, not configuration: `_evp` exists only inside the
    add-on's own interpreter, so it cannot be imported by accident in a subprocess,
    and the endpoint is only set by the runner that spawned us.

    Resolved LAZILY on first use, never at import. `import evp` must not be able to
    fail: the AST scanner and offline tooling import this package in processes that
    legitimately have no transport at all, and a raising import turns that into a
    baffling failure far from its cause.
    """
    try:
        import _evp
    except ImportError:
        pass
    else:
        return "embedded", _evp.call

    endpoint = _os.environ.get("EVP_ENDPOINT")
    if not endpoint:
        raise RuntimeError(
            "evp: no transport. The builtin _evp module is absent (so this is not "
            "Archicad's embedded interpreter) and %EVP_ENDPOINT% is unset (so no EvP "
            "runner started this process). A command folder is not a standalone "
            "script -- run it from the EvP palette."
        )

    from urllib import request as _request
    from urllib.error import URLError as _URLError

    def _http_call(command, params_json):
        url = "%s/evp/call?command=%s" % (endpoint.rstrip("/"), command)
        req = _request.Request(
            url, data=params_json.encode("utf-8"),
            headers={"Content-Type": "application/json"}, method="POST",
        )
        try:
            # No timeout by design: a call can legitimately block for as long as the
            # user takes to answer a pick. A dead add-on shows up as a refused
            # connection, which is a different (and immediate) error.
            with _request.urlopen(req) as response:
                return response.read().decode("utf-8")
        except _URLError as exc:
            raise RuntimeError(
                "evp: the add-on is unreachable at %s (%s). Is the EvP server still "
                "running, and did Archicad stay open?" % (endpoint, exc)
            ) from exc

    return "external", _http_call


_resolved = None   # (mode, transport), cached after the first call


def _transport():
    global _resolved
    if _resolved is None:
        _resolved = _make_transport()
    return _resolved[1]


def __getattr__(name):
    """PEP 562: `evp.api.mode` resolves the transport on first access, not on import."""
    if name == "mode":
        global _resolved
        if _resolved is None:
            _resolved = _make_transport()
        return _resolved[0]
    raise AttributeError("module %r has no attribute %r" % (__name__, name))


class EvpError(RuntimeError):
    """A failed Archicad call. Carries the envelope's structured error.

    The message is deliberately self-contained: it names the command, the
    structured code, the decoded reason, the call_id that ties it to both log
    files, and where to find the rest. A traceback line that reads
    ``EvpError: EvP.CreateRoof [CommandFailed] ACAPI_Element_Create failed:
    APIERR_IRREGULARPOLY (-2130313098) ...`` is the difference between knowing
    what happened and starting an investigation.
    """

    def __init__(self, command, error, meta, params=None):
        self.command = command
        self.code = (error or {}).get("code", "Unknown")
        self.message = (error or {}).get("message", "")
        self.detail = (error or {}).get("detail")
        self.params = params
        self.meta = meta or {}
        call_id = self.meta.get("call_id", "?")
        text = "%s [%s] %s (call_id=%s)" % (command, self.code, self.message, call_id)
        if params:
            text += "\n  params: %s" % (params if len(params) <= 300 else params[:300] + "...")
        text += "\n  trail:  logs\\api_errors.log"
        super().__init__(text)


class Cancelled(Exception):
    """The user cancelled — e.g. Esc during a pick. Not an error; let it unwind."""


class Result:
    __slots__ = ("ok", "data", "error", "meta")

    def __init__(self, envelope):
        self.ok = envelope.get("ok", False)
        self.data = envelope.get("data")
        self.error = envelope.get("error")
        self.meta = envelope.get("meta") or {}

    def __repr__(self):
        return "<evp.Result ok=%r backend=%s duration_ms=%.2f call_id=%s>" % (
            self.ok,
            self.meta.get("backend"),
            self.meta.get("duration_ms", 0.0),
            self.meta.get("call_id"),
        )


def debug(enabled=True):
    """Log every request/response envelope (with call_id) to the console + log file."""
    global _debug
    _debug = bool(enabled)
    call("EvP.SetTracing", {"enabled": _debug}, raise_on_error=False)


def call(command, params=None, raise_on_error=True):
    """Execute one command. Raises EvpError on failure by default.

    Errors are structured and never swallowed. Pass raise_on_error=False to get
    the envelope back instead of an exception.
    """
    # All shipped Python callers now use the v2 native spelling even while their
    # source is being migrated. The dispatcher still accepts an explicit EvP.*
    # request from older external clients as the temporary compatibility path.
    if command.startswith(LEGACY_NATIVE_NAMESPACE + "."):
        command = NATIVE_NAMESPACE + command[len(LEGACY_NATIVE_NAMESPACE):]

    params_json = _json.dumps(params if params is not None else {})

    if _debug:
        print("[evp] --> %s %s" % (command, params_json))

    envelope = _json.loads(_transport()(command, params_json))
    result = Result(envelope)

    if _debug:
        print("[evp] <-- %r" % (result,))

    code = (result.error or {}).get("code") if not result.ok else None

    # The error trail, always on. `debug` is a firehose you have to remember to
    # open BEFORE the run that goes wrong; this records the shape of every call
    # cheaply (one deque append) and writes to disk only on failure. Cancelled is
    # excluded for the same reason the C++ side excludes it: after a Stop, every
    # later call is refused, and dozens of those would bury the real failure.
    #
    # Guarded because instrumentation must never be able to break the thing it
    # instruments -- a broken logger that swallows a real error is worse than no
    # logger, and this runs on EVERY call.
    try:
        from . import errors as _errors
        _errors.record(command, params_json, result.ok, result.meta)
        if not result.ok and code != "Cancelled":
            _errors.report_call_failure(command, params_json, result.error, result.meta)
    except Exception:                                # noqa: BLE001 — see above
        pass

    if not result.ok and raise_on_error:
        if code == "Cancelled":
            raise Cancelled((result.error or {}).get("message", "cancelled"))
        raise EvpError(command, result.error, result.meta, params_json)
    return result
