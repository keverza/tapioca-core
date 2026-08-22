"""evp.paths — the ONE place a command decides WHERE to write.

Every Tapioca command, probe, and test-dump writes only under ``%LOCALAPPDATA%\\Tapioca\\``,
in one of four buckets, each with a FIXED behaviour. Pick the bucket by what the
file is *for*, and the behaviour comes for free:

    logs/     <name>.log            a human reads it to see what happened.
                                    Appended; size-capped + rotated (see LOG_CAP_BYTES).
    output/   <name>.<ext>          a command's LATEST artifact (the current SVG, the
                                    current report). Overwritten in place every run —
                                    latest wins, no history.
    dumps/    <case>__<stamp>/...   a test / geometry capture you replay offline later.
                                    UNIQUE per run (timestamped), NEVER overwritten;
                                    the user prunes these by hand.
    presets/  <kind>/<name>.json    reusable user-authored settings. Named, durable,
                                    and replaced atomically only when requested.

Rule of thumb:
  * Overwrite every run, only the latest matters      -> output_path()
  * Keep a history / replay offline / compare runs     -> dump_dir() or dump_path()
  * A human reads it to follow the run                 -> log_path()

Hand-rolling ``os.path.join(os.environ["LOCALAPPDATA"], "Tapioca", ...)`` is exactly
what let "dump" vs "dumps" vs "output" vs "out" drift across commands, and let some
dumps overwrite the previous run while others accumulated. Don't hand-roll it — call
here, and the location and the overwrite/unique behaviour are decided once, for all.
"""

import datetime
import os

# One cap for every log this module rotates, matched to the C++ side
# (evp::AppendTextLine rotates commands.log / scan.log at the same size). Change
# both together if you change it — 5 MiB holds many big runs but never unbounded.
LOG_CAP_BYTES = 5 * 1024 * 1024


def evp_dir():
    """``%LOCALAPPDATA%\\Tapioca`` — the single root everything writes under.

    ``EVP_HOME`` overrides it, and exists for exactly one caller: the offline
    dry-run harness (``tests/dryrun_command.py``). Without it a dry run appends to
    the SAME log files a real Archicad run uses, and the two interleave with no
    marker between them — which happened, and cost a debugging session reading
    fake guids out of what looked like a live log. Not a general-purpose knob: a
    command must never set it, or its output stops being where the convention says.
    """
    override = os.environ.get("EVP_HOME")
    if override:
        return override
    return os.path.join(os.environ["LOCALAPPDATA"], "Tapioca")


def _bucket(name):
    path = os.path.join(evp_dir(), name)
    os.makedirs(path, exist_ok=True)
    return path


def logs_dir():
    return _bucket("logs")


def output_dir():
    return _bucket("output")


def dumps_dir():
    return _bucket("dumps")


def presets_dir(kind):
    r"""A reusable-settings directory under the Tapioca ``presets`` bucket."""
    path = os.path.join(_bucket("presets"), _slug(kind))
    os.makedirs(path, exist_ok=True)
    return path


def cache_dir(name):
    r"""``%LOCALAPPDATA%\Tapioca\cache\<name>`` — DERIVED state, not an artifact.

    A fourth bucket, and the only one, because the three above are all things a
    PERSON reads, keeps, or prunes. A cache is none of those: nothing here is
    authored, nothing is lost by deleting it, and it is rebuilt on the next run
    from the source it was derived from. Keeping it out of ``output/`` matters —
    ``output/`` is what a command produced and a user may go looking for.

    The rule this preserves is the one that counts: never hand-roll a path. A
    cache written to a path built by hand is exactly how ``dump``/``dumps``/
    ``output``/``out`` drifted in the first place.

    Currently one consumer: the command port-schema cache (``evp._schemacache``).
    """
    path = os.path.join(_bucket("cache"), _slug(name))
    os.makedirs(path, exist_ok=True)
    return path


def timestamp():
    """The shared stamp for unique names — ``YYYYMMDD_HHMMSS``, sortable, no colons
    (colons are illegal in Windows filenames). Second resolution is enough; a
    command that could fire twice in one second should add its own suffix."""
    return datetime.datetime.now().strftime("%Y%m%d_%H%M%S")


def _slug(text):
    """A filesystem-safe token: keep [A-Za-z0-9-_], collapse the rest to '_'."""
    clean = "".join(c if (c.isalnum() or c in "-_") else "_" for c in text).strip("_")
    return clean or "unnamed"


def rotate(path, cap_bytes=LOG_CAP_BYTES):
    """Size-triggered, single-generation rotate for a log FILE (mirrors the C++
    append path). If ``path`` is at or over ``cap_bytes``, move it to
    ``<name>.1<ext>`` (discarding any older ``.1``) so the live log starts fresh.
    Never truncates mid-file — a partial line would hide evidence. Best-effort:
    a failure to rotate must never stop a command from logging."""
    try:
        if not os.path.exists(path) or os.path.getsize(path) < cap_bytes:
            return
        root, ext = os.path.splitext(path)
        backup = root + ".1" + ext
        if os.path.exists(backup):
            os.remove(backup)
        os.replace(path, backup)
    except OSError:
        pass


def log_path(name, cap_bytes=LOG_CAP_BYTES):
    """``logs/<name>.log``, directory ensured, rotated first if it has grown past
    the cap. ``name`` is the bare command name (no extension) — e.g.
    ``log_path("place_slope_symbols")``. Open in append mode after calling."""
    path = os.path.join(logs_dir(), _slug(name) + ".log")
    rotate(path, cap_bytes)
    return path


RUN_SEPARATOR = "=" * 72


def append_log(name, text, cap_bytes=LOG_CAP_BYTES):
    """Append one run's output to ``logs/<name>.log``, behind a run banner.

    A log is APPEND-ONLY, so by the third run the interesting part is buried in
    the middle of the previous two and there is nothing in the text saying where
    one run stopped and the next began — two runs with different parameters read
    as one contradictory run. Every append goes through here so the boundary is
    always the same seventy-two ``=`` and a timestamp, greppable and skimmable.

    Returns the path, so a command can still tell the user where it wrote.
    """
    path = log_path(name, cap_bytes)
    stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    body = text if isinstance(text, str) else "\n".join(str(line) for line in text)
    with open(path, "a", encoding="utf-8") as handle:
        handle.write("\n%s\n%s  %s\n%s\n%s\n"
                     % (RUN_SEPARATOR, name, stamp, RUN_SEPARATOR, body))
    return path


def output_path(filename):
    """``output/<filename>`` — a command's latest artifact, overwritten in place.
    Pass the full filename WITH extension, e.g. ``output_path("selection_outline.svg")``.
    Directory ensured; the file is not touched here (the caller writes it)."""
    return os.path.join(output_dir(), filename)


def dump_dir(case):
    """Create and return a UNIQUE ``dumps/<case>__<stamp>/`` directory for a run
    that emits several linked files (e.g. dump.json + capture.png). Never collides
    with a previous run — that is the whole point of a dump vs an output."""
    path = os.path.join(dumps_dir(), "%s__%s" % (_slug(case), timestamp()))
    os.makedirs(path, exist_ok=True)
    return path


def dump_path(case, ext):
    """Return a UNIQUE single-file dump path ``dumps/<case>__<stamp>.<ext>`` (no
    directory created beyond ``dumps/``). Use for a one-file capture; use
    ``dump_dir()`` when a run emits several files that belong together. ``ext`` may
    be given with or without a leading dot."""
    ext = ext if ext.startswith(".") else "." + ext
    dumps_dir()
    return os.path.join(dumps_dir(), "%s__%s%s" % (_slug(case), timestamp(), ext))
