"""Zone C entry point — the bundled python.exe runs THIS, never the command directly.

Mirrors EvPPy.dll's in-process runner (`_evp_run_command`) deliberately: same
canonical-form checks, same fresh-namespace load, same failure text. Where the two
runners differ, a command behaves differently by zone — exactly the transparency
this file exists to preserve. Change one, change the other.

Contract with ExternalRunner.cpp:
    stdin           the params JSON, then EOF
    %EVP_ENDPOINT%  http://127.0.0.1:<port> of the add-on's server
    %EVP_COMMAND_DIR% the command folder (contains command.py)
    stdout/stderr   streamed back to the palette console
    exit code       0 = run() returned, 1 = anything else (traceback on stderr),
                    0xE9 = the run was cancelled (E9; see CANCEL_EXIT_CODE)
"""

import importlib.util
import json
import os
import sys
import traceback

# E9 -- "this run was cancelled". Must match ExternalCancelExitCode in
# ExternalRunner.cpp, which uses the same value to TerminateProcess a subprocess
# that will not stop on its own. Kept under 256 so it survives any low-byte
# truncation of an exit status unchanged.
CANCEL_EXIT_CODE = 0xE9


def _fail(message):
    sys.stderr.write(message + "\n")
    sys.stderr.flush()
    return 1


def _handshake():
    """Refuse a major-version mismatch BEFORE running anything.

    Failing here beats failing halfway through a transaction: the add-on and this
    subprocess disagreeing about the API is not something a script can recover from.
    """
    from urllib import request
    from urllib.error import URLError

    import evp

    endpoint = os.environ["EVP_ENDPOINT"].rstrip("/")
    try:
        with request.urlopen(endpoint + "/evp/version") as response:
            remote = json.loads(response.read().decode("utf-8"))
    except URLError as exc:
        # This is the FIRST call the subprocess makes, so it is where a stopped
        # server surfaces. A bare URLError here would be an information-free
        # failure -- say what was unreachable and what to do about it.
        raise RuntimeError(
            "The EvP add-on is not answering at %s (%s). The external runtime "
            "reaches Archicad only through that server -- make sure the EvP panel's "
            "server is running and Archicad is still open." % (endpoint, exc)
        ) from None

    theirs = remote.get("api_version", "0.0.0")
    if theirs.split(".")[0] != evp.API_VERSION.split(".")[0]:
        raise RuntimeError(
            "evp API major mismatch: this package is %s, the add-on serves %s. "
            "The bundled evp package and EvP.apx are from different builds." % (evp.API_VERSION, theirs)
        )


def main():
    # Strip whitespace and a stray BOM: ExternalRunner.cpp writes bare UTF-8, but a
    # human testing this by hand pipes through a shell that may not.
    params_json = sys.stdin.read().lstrip("﻿").strip() or "{}"

    action = os.environ.get("EVP_ACTION") or ""
    watch_armed = os.environ.get("EVP_WATCH", "").strip().lower() not in ("", "0", "false", "no", "off")
    # Set only when the palette dispatched a @evp.menu entry: where the click
    # landed. Rides in the environment for the same reason EVP_ACTION does —
    # stdin carries the user's parameter values and nothing else should have to
    # be filtered back out of them.
    region = os.environ.get("EVP_MENU_REGION") or ""
    folder = os.environ.get("EVP_COMMAND_DIR")
    if not folder:
        return _fail("%EVP_COMMAND_DIR% is unset -- this script is launched by EvP, not by hand.")

    entry = os.path.join(folder, "command.py")
    if not os.path.isfile(entry):
        return _fail("No command.py in %s" % folder)

    try:
        _handshake()
    except Exception:
        return _fail("EvP handshake failed:\n" + traceback.format_exc())

    # The command folder, `_lib/` at the scripts root, and opted-in sibling folders
    # all become importable here — the SAME call Zone B makes, which is the point:
    # when the two zones each carried their own copy of this, a helper module worked
    # externally and failed in-process. After the handshake, so a version mismatch
    # still reports as one. No `deactivate` as in Zone B: this is a one-shot
    # subprocess, so sys.modules starts fresh each run anyway.
    from evp import _commandpath

    _commandpath.activate(folder)

    name = "evp_command_" + os.path.basename(folder.rstrip("\\/"))
    try:
        spec = importlib.util.spec_from_file_location(name, entry)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)

        fn = getattr(module, "run", None)
        if fn is None:
            raise AttributeError("command.py defines no run()")
        if getattr(fn, "__evp_command__", None) is None:
            raise TypeError("run() is not decorated with @evp.command")

        # How a command is CALLED -- signature form vs schema form -- is decided
        # in one place, evp._invoke, which the embedded runner and the offline
        # dry-run harness call too. Spelling `fn(**params)` here again is how the
        # two conventions drift apart with nothing to report the difference.
        from evp import _invoke

        # An ACTION is not a run: it acts on what the LAST run stored, because
        # re-running the command to export its result would repeat every write
        # that run performed. Mirrored in EvPPy.cpp's runner -- change one,
        # change the other.
        if action:
            _invoke.run_action(fn, action, folder=folder, region=region)
        else:
            _invoke.invoke(fn, json.loads(params_json), folder=folder, watch_armed=watch_armed)
    except Exception as exc:
        # E9 -- a cancel is a CLEAN exit, not a crash. Stop, a palette close and
        # timeout_s all arrive here as evp.Cancelled (every bus call is refused once
        # the run is cancelled, so an ordinary command unwinds by itself). Exiting
        # with a dedicated code lets ExternalRunner.cpp report "cancelled" and print
        # no traceback -- and it matches the code it TerminateProcess'es with, so a
        # cooperative stop and a kill look identical to the palette.
        # Mirrored in EvPPy.dll's _evp_run_command; change one, change the other.
        import evp

        if isinstance(exc, evp.Cancelled):
            sys.stdout.write("cancelled: %s\n" % (exc or "stopped"))
            sys.stdout.flush()
            return CANCEL_EXIT_CODE

        # Record the traceback together with the bus calls that preceded it, into
        # the same logs\api_errors.log the add-on writes to. Zone C is a separate
        # process, so without this its failures leave no trace in the shared trail
        # -- and "it works embedded but not external" is exactly the bug you would
        # be chasing. Guarded: never let logging replace the real failure.
        # Mirrored in EvPPy.dll's _evp_run_command; change one, change the other.
        try:
            evp.errors.report_exception(exc, where=name)
        except Exception:
            pass
        return _fail(traceback.format_exc())
    finally:
        sys.modules.pop(name, None)

    return 0


if __name__ == "__main__":
    sys.exit(main())
