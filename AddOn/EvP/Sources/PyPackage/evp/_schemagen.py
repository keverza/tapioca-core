"""Generate one command's port metadata by IMPORTING it — in a subprocess.

    python -m evp._schemagen <command folder>  ->  JSON on stdout

A command that declares `inputs=` carries its ports in a class body, which no
AST pass can read: the model's JSON Schema only exists once the module has run.
But the palette must not import command modules in its own process. Two reasons,
both already paid for:

  * Rescan walks 60+ folders. Importing them all would execute every command's
    module-level code inside Archicad, on the main thread, at the moment the
    user pressed a button.
  * A command that dies on import currently still APPEARS in the palette, with
    its error, because the AST pass does not care whether the module runs. Move
    the scan into the import and a broken command vanishes instead — which looks
    exactly like a failed build, the failure mode the whole scanner gate exists
    to prevent.

So the import happens out of process, its result is cached (evp._schemacache),
and a failure comes back as a diagnostic string rather than taking anything with
it. This module is deliberately standalone: it is executed as `__main__` by a
fresh interpreter and must not assume the caller's sys.path.
"""

from __future__ import annotations

import importlib.util
import json
import os
import sys
import traceback


def generate(folder):
    """Import `<folder>/command.py` and return its port metadata.

    Returns {"ok": True, "params": [...], "needs_preview": bool} or
    {"ok": False, "error": "..."} — never raises, because the caller is a
    scanner whose job is to report a broken command, not to fail with it.
    """
    path = os.path.join(folder, "command.py")
    if not os.path.isfile(path):
        return {"ok": False, "error": "no command.py in %s" % folder}

    try:
        # The command's own folder first, so `import slopegeom` resolves the way
        # it does at run time. _commandpath owns the full precedence chain in
        # Archicad; here only the folder itself is needed to import the module.
        if folder not in sys.path:
            sys.path.insert(0, folder)

        spec = importlib.util.spec_from_file_location("_tapioca_schemagen", path)
        module = importlib.util.module_from_spec(spec)
        # ⚠️ REGISTER BEFORE EXEC, the way both runners do. A model written under
        # `from __future__ import annotations` carries STRING annotations, and
        # pydantic resolves them through sys.modules[cls.__module__] — an
        # unregistered module has no entry, and model_json_schema() then fails
        # with "is not fully defined" on a model that is perfectly well defined.
        sys.modules["_tapioca_schemagen"] = module
        spec.loader.exec_module(module)

        run = getattr(module, "run", None)
        meta = getattr(run, "__evp_command__", None)
        if meta is None:
            return {"ok": False,
                    "error": "no run() decorated with @tapioca.command in %s" % path}

        inputs = meta.get("inputs")
        if inputs is None:
            # Not an error: a signature-style command has no schema to generate,
            # and the AST pass already has its ports.
            return {"ok": True, "params": None, "needs_preview": False,
                    "preview_kind": meta.get("preview_kind") or "text"}

        from . import _ports

        # Which titles the author actually WROTE. Pydantic titles every field,
        # so the schema alone cannot tell an authored title from a generated
        # one; model_fields can, and this is the only place that still has the
        # model. Without it `title="Spacing"` on a field named `spacing` was
        # indistinguishable from no title at all and the row rendered as the
        # raw identifier.
        explicit = {name for name, field in inputs.model_fields.items()
                    if field.title is not None}
        params = _ports.ports_from_schema(inputs.model_json_schema(),
                                          labels=meta.get("labels") or {},
                                          explicit_titles=explicit)
        return {"ok": True,
                "params": params,
                "needs_preview": bool(meta.get("needs_preview")),
                # The decorator already resolved this (declared wins, else derived
                # from whether preview= was given); the scanner only carries it.
                "preview_kind": meta.get("preview_kind") or "text"}

    except Exception:
        # The traceback is the whole value of running this out of process — it
        # names the line the command dies on, which an AST pass can never see.
        return {"ok": False, "error": traceback.format_exc(limit=6).strip()}


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: python -m evp._schemagen <command folder>\n")
        return 2
    sys.stdout.write(json.dumps(generate(argv[1]), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
