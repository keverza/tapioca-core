r"""EvP embedded-runtime environment manager (E7): consume `requires`, install on demand.

Run as a STANDALONE FILE by the runtime's own python.exe — NOT `-m evp._env`, and it
imports NOTHING from the `evp` package. That is deliberate: reset wipes site-packages,
and this code must keep working with the managed env torn down, so it can rebuild it.
Only the stdlib (present in runtime\Lib) is imported here.

    <runtime>\python.exe -s <pkg>\evp\_env.py ensure --require ezdxf==1.3.0 [--require ...]
    <runtime>\python.exe -s <pkg>\evp\_env.py reset  [--require <union of all commands> ...]

Contract with the .apx (ControlPalette): prints exactly ONE JSON object to STDOUT — the
machine result the caller parses — while human-readable progress goes to STDERR. Exit 0
on success, non-zero on failure. `ensure` is a no-op (and fast, no pip spawn) when every
requirement is already satisfied, so a second run installs nothing.

The install location is the runtime's own site-packages (sysconfig 'purelib'), which both
the embedded isolated interpreter and external python.exe already import from (see plan
E7 findings). A cross-process file lock guards the whole install so two Archicad instances
cannot corrupt site-packages at once.
"""

import argparse
import importlib.metadata as _md
import json
import os
import shutil
import subprocess
import sys
import time

# Kept in sync with Install-Runtime.ps1's -Baseline. reset() always restores these, so
# evp.geometry's numpy is present even after a wipe, independent of any command's requires.
#
# pydantic is here rather than in a command's `requires` because a schema-style
# command cannot even be SCANNED without it: the palette generates that command's
# controls by importing it, so a missing pydantic would not fail the run, it would
# make the command appear with no controls at all. A dependency the scanner needs
# is a baseline dependency.
BASELINE = ["numpy==2.0.2", "pillow", "requests", "pydantic>=2.7,<3"]

# The pip toolchain reset() must NOT delete — it is what performs the reinstall. Matched
# case-insensitively against the leading name of each site-packages entry.
_PRESERVE_PREFIXES = ("pip", "setuptools", "wheel", "pkg_resources",
                      "_distutils_hack", "distutils-precedence")

_VERSION_OPS = ("===", "~=", "==", "!=", ">=", "<=", ">", "<")


def _runtime_home():
    return sys.prefix


def _site_packages():
    import sysconfig
    return sysconfig.get_path("purelib")


def _lockfile():
    return os.path.join(_runtime_home(), "evp-env.lock.json")


# ---- requirement parsing (deliberately tiny; pip is the authority on installs) ------

def _split_spec(spec):
    """('ezdxf==1.3.0') -> ('ezdxf', '==', '1.3.0'); ('pillow') -> ('pillow', None, None).

    Handles the common pinned forms EvP uses. Anything it can't parse cleanly falls
    through to `op=None`, which makes _is_satisfied conservative (name-only), and pip
    remains the real resolver at install time.
    """
    s = spec.strip()
    # drop an extras marker: foo[bar]==1 -> foo==1
    if "[" in s:
        head, _, rest = s.partition("[")
        _, _, after = rest.partition("]")
        s = head + after
    for op in _VERSION_OPS:
        idx = s.find(op)
        if idx != -1:
            return s[:idx].strip(), op, s[idx + len(op):].strip()
    return s.strip(), None, None


def _name(spec):
    return _split_spec(spec)[0]


def _version_tuple(v):
    # Compare on the numeric release segments only (1.3.0 -> (1,3,0)); good enough for
    # the ==/>= pins EvP declares. Non-numeric suffixes (rc, dev) are ignored.
    parts = []
    for chunk in v.replace("-", ".").replace("+", ".").split("."):
        num = ""
        for ch in chunk:
            if ch.isdigit():
                num += ch
            else:
                break
        parts.append(int(num) if num else 0)
    return tuple(parts)


def _pad(a, b):
    n = max(len(a), len(b))
    return a + (0,) * (n - len(a)), b + (0,) * (n - len(b))


def _satisfies(installed, op, want):
    if op is None:
        return True
    a, b = _pad(_version_tuple(installed), _version_tuple(want))
    if op in ("==", "==="):
        return installed == want or a == b
    if op == "!=":
        return a != b
    if op == ">=":
        return a >= b
    if op == "<=":
        return a <= b
    if op == ">":
        return a > b
    if op == "<":
        return a < b
    if op == "~=":  # compatible release: >= want AND same major.minor prefix
        return a >= b and a[: len(b) - 1] == b[: len(b) - 1]
    return False


def _is_satisfied(spec):
    name, op, want = _split_spec(spec)
    try:
        installed = _md.version(name)
    except _md.PackageNotFoundError:
        return False
    return _satisfies(installed, op, want)


# ---- cross-process lock ------------------------------------------------------------

class _FileLock:
    """Exclusive lock on <runtime>\\.env.lock via msvcrt, so two Archicad instances
    can't pip-install into the same site-packages concurrently."""

    def __init__(self, timeout=180.0):
        self.path = os.path.join(_runtime_home(), ".env.lock")
        self.timeout = timeout
        self.fd = None

    def __enter__(self):
        import msvcrt
        self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o644)
        deadline = time.monotonic() + self.timeout
        while True:
            try:
                msvcrt.locking(self.fd, msvcrt.LK_NBLCK, 1)
                return self
            except OSError:
                if time.monotonic() >= deadline:
                    os.close(self.fd)
                    self.fd = None
                    raise TimeoutError(
                        "another EvP instance holds the environment lock (%s)" % self.path)
                time.sleep(0.25)

    def __exit__(self, *exc):
        import msvcrt
        if self.fd is not None:
            try:
                msvcrt.locking(self.fd, msvcrt.LK_UNLCK, 1)
            finally:
                os.close(self.fd)
                self.fd = None


# ---- pip + lockfile ----------------------------------------------------------------

# WinError 5 on a `.pyd`/`.dll` here almost always means the running Archicad session has
# already imported that package — Windows locks a loaded binary, so pip cannot overwrite
# it. The fix is not permissions; it is that the interpreter holding the lock must exit.
_LOCKED_MESSAGE = (
    "cannot install/upgrade a package the running Archicad session has already imported "
    "(Windows locks a loaded .pyd/.dll — e.g. numpy after any geometry command). "
    "Recovery: CLOSE Archicad, then run  AddOn\\EvP\\Install-Runtime.ps1 -Force  to "
    "rebuild the runtime cleanly; or restart Archicad and install/reset BEFORE running "
    "any command that imports these packages.")


def _looks_locked(text):
    t = text or ""
    return "WinError 5" in t or "Access is denied" in t or "[Errno 13]" in t


def _pip_install(specs):
    """Install/upgrade specs into the runtime's site-packages. Raises on failure."""
    cmd = [sys.executable, "-s", "-m", "pip", "install", "--no-warn-script-location",
           "--disable-pip-version-check", "--no-input", *specs]
    print("pip install %s" % " ".join(specs), file=sys.stderr, flush=True)
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace")
    sys.stderr.write(proc.stdout or "")
    sys.stderr.flush()
    if proc.returncode != 0:
        tail = "\n".join((proc.stdout or "").strip().splitlines()[-8:])
        if _looks_locked(proc.stdout):
            raise RuntimeError(_LOCKED_MESSAGE + "\n\npip said:\n" + tail)
        raise RuntimeError("pip install failed (exit %d):\n%s" % (proc.returncode, tail))


def _write_lock():
    """Record the fully resolved env for reproducibility + Reset's baseline. Not the
    gate for `ensure` (importlib.metadata is), but the audit trail of what's present."""
    installed = {}
    for dist in _md.distributions():
        name = dist.metadata["Name"]
        if name:
            installed[name] = dist.version
    data = {
        "python": sys.version.split()[0],
        "site_packages": _site_packages(),
        "installed": dict(sorted(installed.items(), key=lambda kv: kv[0].lower())),
    }
    with open(_lockfile(), "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2, sort_keys=True)
    return data


# ---- operations --------------------------------------------------------------------

def ensure(requires):
    """Make every requirement importable, installing only what's missing. Returns a
    result dict; no-op (no pip) when all are already satisfied."""
    requires = [r for r in (requires or []) if r and r.strip()]
    if not requires:
        return {"ok": True, "action": "noop", "requires": []}
    with _FileLock():
        missing = [r for r in requires if not _is_satisfied(r)]
        if not missing:
            return {"ok": True, "action": "noop", "requires": requires}
        _pip_install(missing)
        # Re-check: pip claims success, but a name typo could still leave it unimportable.
        still = [r for r in missing if not _is_satisfied(r)]
        if still:
            raise RuntimeError("still unsatisfied after install: %s" % ", ".join(still))
        _write_lock()
        return {"ok": True, "action": "install", "installed": missing, "requires": requires}


def reset(union):
    """Recovery path: wipe managed packages (keeping the pip toolchain), then reinstall
    the baseline + the union of every command's `requires`. Recovers a wedged env without
    a full re-provision, since pip survives the wipe."""
    sp = _site_packages()
    with _FileLock():
        removed, locked = [], []
        if os.path.isdir(sp):
            for entry in os.listdir(sp):
                low = entry.lower()
                if low.startswith(_PRESERVE_PREFIXES) or low == "__pycache__":
                    continue
                full = os.path.join(sp, entry)
                try:
                    if os.path.isdir(full):
                        shutil.rmtree(full)   # NOT ignore_errors: a lock must be seen
                    else:
                        os.remove(full)
                    removed.append(entry)
                except OSError as exc:
                    # A loaded C-extension (numpy/PIL after a geometry command) is mapped
                    # by the running process and can't be deleted. Collect and stop before
                    # the doomed reinstall, which would only emit a cryptic WinError 5.
                    if _looks_locked(str(exc)) or isinstance(exc, PermissionError):
                        locked.append(entry)
                    else:
                        raise
        if locked:
            raise RuntimeError(
                "cannot reset — these managed packages are in use by the running Archicad "
                "session: %s. %s" % (", ".join(sorted(locked)[:8]), _LOCKED_MESSAGE))
        try:
            os.remove(_lockfile())
        except OSError:
            pass
        wanted = list(dict.fromkeys(BASELINE + [r for r in (union or []) if r and r.strip()]))
        _pip_install(wanted)
        lock = _write_lock()
        return {"ok": True, "action": "reset", "removed": removed,
                "reinstalled": wanted, "installed_count": len(lock["installed"])}


# ---- CLI ---------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(prog="evp._env", add_help=True)
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name in ("ensure", "reset"):
        p = sub.add_parser(name)
        p.add_argument("--require", action="append", default=[], dest="require")
    args = parser.parse_args(argv)

    try:
        if args.cmd == "ensure":
            result = ensure(args.require)
        else:
            result = reset(args.require)
    except Exception as exc:   # every failure becomes a structured result the .apx reads
        json.dump({"ok": False, "error": str(exc), "type": type(exc).__name__}, sys.stdout)
        sys.stdout.write("\n")
        sys.stdout.flush()
        return 1

    json.dump(result, sys.stdout)
    sys.stdout.write("\n")
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
