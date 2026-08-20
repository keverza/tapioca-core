"""A run transcript that goes to the palette console AND to a log file.

The most-copied twenty lines in this repo: nearly every command and probe grows its
own ``_say()``/``_flush()`` pair, because the rule is that a user must never have to
read evidence out of an Archicad alert. Several of those copies also hand-roll
``os.path.join(os.environ["LOCALAPPDATA"], "EvP", "logs", ...)``, which is exactly
what ``evp.paths`` exists to prevent. Import this instead:

    import probelog

    log = probelog.start("place_slope_symbols")
    try:
        log.say("selection: %d element(s)" % len(guids))
    finally:
        log.flush()                     # ALWAYS in a finally -- see below

or, equivalently, as a context manager:

    with probelog.start("place_slope_symbols") as log:
        log.say("...")

⚠️ FLUSH IN A ``finally``. A probe that flushes only on the success path writes
nothing at all on the run that failed — which is the run whose transcript you needed.
The context-manager form does this for you.

The file is ``%LOCALAPPDATA%\\EvP\\logs\\<name>.log`` via ``evp.paths.log_path``, so it
is size-capped and rotated like every other EvP log. Each flush APPENDS one block
with a timestamp header, so consecutive runs stay readable instead of overwriting one
another.
"""
import datetime
import time

import evp

#: When THIS MODULE OBJECT was created — the only honest evidence that an edited
#: shared module really took effect. A file's mtime cannot tell you: it is read off
#: disk at report time, so it changes whether or not the loaded module was replaced.
#: A fresh import sets this to "seconds ago"; a stale one keeps the value from the
#: run that first imported it. `SharedLibProbe` compares it against the clock.
LOADED_AT = time.time()


class Log:
    """Buffered transcript. `say` echoes immediately, `flush` appends the block."""

    def __init__(self, name, echo=True):
        self.name = name
        self.echo = echo
        self.path = evp.paths.log_path(name)
        self._lines = []
        self._written = 0

    def say(self, text=""):
        """One line: to the console now, to the file at the next flush."""
        if self.echo:
            print(text)
        self._lines.append(str(text))

    def flush(self):
        """Append everything said since the last flush. Safe to call repeatedly.

        Never raises: this runs in a ``finally``, often while an exception is on its
        way up, and a logging failure must not replace the failure worth reading.
        """
        pending = self._lines[self._written:]
        if not pending:
            return self.path
        try:
            with open(self.path, "a", encoding="utf-8") as handle:
                if self._written == 0:
                    handle.write("\n===== %s  %s =====\n" % (
                        self.name,
                        datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")))
                handle.write("\n".join(pending) + "\n")
            self._written = len(self._lines)
            if self.echo:
                print("log -> %s" % self.path)
        except OSError as exc:
            if self.echo:
                print("(could not write %s: %s)" % (self.path, exc))
        return self.path

    @property
    def lines(self):
        """Everything said so far — for a command that also reports in the palette."""
        return list(self._lines)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.flush()
        return False                    # never swallow the exception


def start(name, echo=True):
    """Start a transcript named `name` (the log file's stem).

    Named `start`, not `open`: a module-level `open` would shadow the builtin
    INSIDE this module, and `Log.flush` needs the real one.
    """
    return Log(name, echo=echo)
