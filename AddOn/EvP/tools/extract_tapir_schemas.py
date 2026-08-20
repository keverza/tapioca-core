"""Extract Tapir command & schema definitions from JS files into plain JSON.

Reads the Tapir reference JS files (which are JSON wrapped with a var prefix),
parses them, and writes clean JSON to dist/tapir-reference/.

Called by package-release.ps1 at packaging time.
"""
import json
import os
import re
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))

TAPIR_REF = os.path.join(
    REPO_ROOT, "AddOn", "reference", "tapir-archicad-automation-main"
)
COMMANDS_JS = os.path.join(TAPIR_REF, "docs", "archicad-addon", "command_definitions.js")
SCHEMAS_JS = os.path.join(
    TAPIR_REF, "docs", "archicad-addon", "common_schema_definitions.js"
)
DIST_DIR = os.path.join(REPO_ROOT, "dist", "tapir-reference")


def _strip_js_prefix(content: str, var_name: str) -> str:
    content = content.strip()
    prefix = f"var {var_name} = "
    if content.startswith(prefix):
        content = content[len(prefix):]
    content = content.rstrip().rstrip(";").strip()
    return content


def _try_read_tapir_version() -> str:
    """Read Tapir AddOn version from AddOnVersion.hpp."""
    header = os.path.join(
        TAPIR_REF, "archicad-addon", "Sources", "AddOnVersion.hpp"
    )
    if not os.path.exists(header):
        return "unknown"
    with open(header, "r") as f:
        text = f.read()
    m = re.search(r'ADDON_VERSION\s+"([^"]+)"', text)
    return m.group(1) if m else "unknown"


def _try_read_tapir_commit() -> str:
    """Read the head commit of the Tapir reference checkout."""
    import subprocess
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True, cwd=TAPIR_REF
        )
        return result.stdout.strip()
    except Exception:
        return "unknown"


def extract() -> None:
    os.makedirs(DIST_DIR, exist_ok=True)

    with open(COMMANDS_JS, "r", encoding="utf-8") as f:
        raw = f.read()
    commands = json.loads(_strip_js_prefix(raw, "gCommands"))
    out = os.path.join(DIST_DIR, "command_definitions.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(commands, f, indent=2, ensure_ascii=False)
    groups = len(commands)
    total = sum(len(g["commands"]) for g in commands)
    print(f"Wrote {total} commands in {groups} groups → {out}")

    with open(SCHEMAS_JS, "r", encoding="utf-8") as f:
        raw = f.read()
    schemas = json.loads(_strip_js_prefix(raw, "gSchemaDefinitions"))
    out = os.path.join(DIST_DIR, "common_schema_definitions.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(schemas, f, indent=2, ensure_ascii=False)
    print(f"Wrote {len(schemas)} schema definitions → {out}")

    version = _try_read_tapir_version()
    commit = _try_read_tapir_commit()
    ver = os.path.join(DIST_DIR, "VERSION.txt")
    with open(ver, "w", encoding="utf-8") as f:
        f.write(f"Tapir version: {version}\n")
        f.write(f"Tapir commit: {commit}\n")
    print(f"Tapir {version} (commit {commit}) → {ver}")


if __name__ == "__main__":
    extract()
