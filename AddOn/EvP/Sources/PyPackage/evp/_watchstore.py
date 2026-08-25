"""Best-effort persistence for the last successful armed watch trace."""

from __future__ import annotations

import hashlib
import json
import os

from . import paths

__all__ = ["load", "save", "forget", "MAX_PERSISTED_BYTES"]

MAX_PERSISTED_BYTES = 4 * 1024 * 1024
_CACHE = "watch"


def _path(folder):
    name = os.path.basename(os.path.normpath(folder)) if folder else "command"
    tag = hashlib.sha256(
        os.path.normcase(os.path.abspath(folder or name)).encode("utf-8")
    ).hexdigest()[:12]
    return os.path.join(paths.cache_dir(_CACHE), "%s-%s.json" % (name, tag))


def encode(payload):
    """Return the exact compact UTF-8 representation written to disk."""
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def save(folder, payload):
    """Atomically replace a command's trace. Returns False and never raises on error."""
    temporary = None
    try:
        body = encode(payload)
        if len(body) > MAX_PERSISTED_BYTES:
            return False
        path = _path(folder)
        temporary = path + ".tmp-%d" % os.getpid()
        with open(temporary, "wb") as handle:
            handle.write(body)
        os.replace(temporary, path)
        return True
    except (OSError, TypeError, ValueError):
        if temporary is not None:
            try:
                os.remove(temporary)
            except OSError:
                pass
        return False


def load(folder):
    """Return a stored trace, or None when it is missing or unreadable."""
    try:
        with open(_path(folder), encoding="utf-8") as handle:
            payload = json.load(handle)
    except (OSError, TypeError, ValueError):
        return None
    return payload if isinstance(payload, dict) else None


def forget(folder=None):
    """Drop one command's trace, or every stored trace. Intended for reset/tests."""
    if folder is not None:
        try:
            os.remove(_path(folder))
        except OSError:
            pass
        return
    try:
        directory = paths.cache_dir(_CACHE)
        names = os.listdir(directory)
    except OSError:
        return
    for name in names:
        if name.endswith(".json"):
            try:
                os.remove(os.path.join(directory, name))
            except OSError:
                pass
