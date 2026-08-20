"""Every field a native command RETURNS must be in its response schema.

WHY THIS EXISTS.  The dispatcher validates each response against the schema in
the registry table, with `additionalProperties: false`.  So a field added to
`ExecuteNative` and not to the schema does not degrade the response -- it fails
the ENTIRE call, and the error names the field rather than the command, which
reads as a bad parameter somewhere upstream.

⚠️ IT HAS COST A FULL RUN TWICE.  DiligentViewportState.cpp carries a comment
about the first time (2026-08-11): `overlay` was `required` and absent from the
response, every call returned SchemaValidationFailed, and the probe reported "THE
RENDERER NEVER STARTED" while the viewport was rendering 11,118 frames.  On
2026-08-13 the same thing happened in the other direction -- six fields added to
`CameraSyncModeState`'s response, none added to its schema -- and a camera-sync
matrix run was lost to it.  A comment did not prevent the second one.

⚠️ NEITHER EXISTING GATE CAN SEE THIS.  `dryrun_command.py` fakes the wire, so no
schema is ever consulted; `check_python.py` reads the Python side only.  The
mismatch is invisible until Archicad runs the command.

WHAT IT CHECKS, and deliberately no more:
  * every `os.Add ("name", ...)` in a command class appears in that command's
    response-schema properties;
  * every name in `required` is actually added.

⚠️ IT DOES NOT CHECK INPUT ENUMS, and the 2026-08-13 failure had that half too:
`wakepredict` was added to `ParseCameraSyncMode` and to the probe's `evp.Enum`,
but not to the input schema's `"enum"`, so every call was rejected before the
command ran.  Catching that means relating a Python literal to a C++ string
table to a JSON literal across three files, and a check that guesses at those
relationships would produce false failures -- which is worse than this gap.  It
is caught by running the command, which is the one thing that always finds it.

It is regex-based and best-effort: a command whose class or schema cannot be
matched confidently is SKIPPED and counted, never guessed at.  Skipping is
honest; a false pass on a parse failure would be the same class of bug this
file exists to catch.

⚠️ FALSE POSITIVES ARE WORSE THAN GAPS HERE, because a gate that fails on correct
code gets bypassed and then catches nothing.  Two shapes in this repo produced
them on the first run and are handled explicitly:
  * a free function BETWEEN two command classes -- its `os.Add` calls belong to a
    NESTED object, not to any command response.  So only the text of
    `ExecuteNative` itself is scanned, not everything up to the next class.
  * a field added by a shared helper (`CommandUtils`' `retainedBytes`), which no
    amount of looking at the command class will reveal.  Keys added anywhere in
    CommandUtils count as available to every command.
"""

import os
import re
import sys

_SOURCE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "Sources", "AddOn", "NativeCommands")

# { "CommandName", &MakeRegisteredNativeCommand<ClassName>, bool, R"json(input)json",
#   R"json(response)json" }
_REGISTRY = re.compile(
    r'\{\s*"(?P<name>\w+)"\s*,\s*&MakeRegisteredNativeCommand<(?P<cls>\w+)>\s*,'
    r'\s*(?:true|false)\s*,\s*R"json\((?P<input>.*?)\)json"\s*,'
    r'\s*R"json\((?P<response>.*?)\)json"\s*\}',
    re.DOTALL)

_CLASS = re.compile(r'\bclass\s+(?P<cls>\w+)\s*:\s*public\s+\w*Command\b')
_EXECUTE = re.compile(r'\bNativeCommandResult\s+ExecuteNative\s*\(')
_OS_ADD = re.compile(r'\bos\.Add\s*\(\s*"(?P<key>\w+)"')
_PROPERTY = re.compile(r'"(?P<key>\w+)"\s*:\s*\{')


def _brace_block(text, start):
    """The {...} block beginning at or after `start`, or None."""
    open_brace = text.find("{", start)
    if open_brace < 0:
        return None
    depth = 0
    for index in range(open_brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace:index + 1]
    return None


def class_bodies(text):
    """Map class name -> the body of ITS ExecuteNative, not the whole class.

    ⚠️ ExecuteNative ONLY. Taking everything up to the next class swept in free
    functions defined between classes, whose `os.Add` calls fill nested objects
    and are not the command's response at all -- four false positives on the
    first run, every one of them correct code.
    """
    starts = [(m.group("cls"), m.start()) for m in _CLASS.finditer(text)]
    bodies = {}
    for index, (name, start) in enumerate(starts):
        end = starts[index + 1][1] if index + 1 < len(starts) else len(text)
        span = text[start:end]
        execute = _EXECUTE.search(span)
        if execute is None:
            continue
        body = _brace_block(span, execute.end())
        if body is not None:
            bodies[name] = body
    return bodies


def helper_keys(root):
    """Keys added by shared helpers, available to any command that calls one."""
    path = os.path.join(root, "CommandUtils.cpp")
    if not os.path.isfile(path):
        return set()
    text = open(path, "r", encoding="utf-8", errors="replace").read()
    return {m.group("key") for m in _OS_ADD.finditer(text)}


def schema_properties(schema):
    """Property names declared under the schema's top-level "properties" object.

    Parsed by brace matching rather than json.loads: these are hand-written
    one-line schemas and a single stray character would otherwise disable the
    check for that command silently.
    """
    marker = '"properties"'
    at = schema.find(marker)
    if at < 0:
        return None
    open_brace = schema.find("{", at)
    if open_brace < 0:
        return None
    depth = 0
    for index in range(open_brace, len(schema)):
        if schema[index] == "{":
            depth += 1
        elif schema[index] == "}":
            depth -= 1
            if depth == 0:
                block = schema[open_brace:index + 1]
                # Only the keys at depth 1 are properties; nested ones are types.
                keys, level = [], 0
                for match in re.finditer(r'"(\w+)"\s*:\s*\{|\{|\}', block):
                    token = match.group(0)
                    if token.endswith("{"):
                        level += 1
                        if level == 2 and match.group(1):
                            keys.append(match.group(1))
                    elif token == "}":
                        level -= 1
                return keys
    return None


def required_names(schema):
    match = re.search(r'"required"\s*:\s*\[(.*?)\]', schema, re.DOTALL)
    if not match:
        return []
    return re.findall(r'"(\w+)"', match.group(1))


def check_file(path, verbose, helpers):
    text = open(path, "r", encoding="utf-8", errors="replace").read()
    bodies = class_bodies(text)
    problems, checked, skipped = [], 0, 0

    for entry in _REGISTRY.finditer(text):
        name = entry.group("name")
        body = bodies.get(entry.group("cls"))
        if body is None:
            skipped += 1
            continue
        declared = schema_properties(entry.group("response"))
        if declared is None:
            skipped += 1
            continue

        added = []
        for match in _OS_ADD.finditer(body):
            if match.group("key") not in added:
                added.append(match.group("key"))
        if not added:
            # A command that returns something built elsewhere. Nothing to
            # compare against, so claiming it passed would be a lie.
            skipped += 1
            continue

        checked += 1
        rel = os.path.relpath(path, os.path.join(os.path.dirname(path), "..", "..", ".."))
        for key in added:
            if key not in declared:
                problems.append(
                    f"  * {rel}: {name} returns \"{key}\" but its response schema does not "
                    f"declare it. additionalProperties is false, so EVERY call fails with "
                    f"SchemaValidationFailed naming \"{key}\".")
        for key in required_names(entry.group("response")):
            if key in declared and key not in added and key not in helpers:
                problems.append(
                    f"  * {rel}: {name}'s response schema REQUIRES \"{key}\" but the command "
                    f"never adds it. Every call fails.")
        if verbose:
            print(f"  ok  {name}: {len(added)} field(s)")

    return problems, checked, skipped


def main():
    verbose = "-v" in sys.argv
    root = os.path.normpath(_SOURCE_DIR)
    if not os.path.isdir(root):
        print(f"schema check: no such directory {root}", file=sys.stderr)
        return 1

    helpers = helper_keys(root)
    problems, checked, skipped = [], 0, 0
    for entry in sorted(os.listdir(root)):
        if not entry.endswith(".cpp"):
            continue
        found, did, skip = check_file(os.path.join(root, entry), verbose, helpers)
        problems.extend(found)
        checked += did
        skipped += skip

    if problems:
        print(f"SCHEMA CHECK FAILED - {len(problems)} problem(s):")
        print()
        for problem in problems:
            print(problem)
        print()
        print("A response field and its schema are ONE EDIT. See the header of")
        print("tools/schema_check.py for the two runs this has already cost.")
        return 1

    print(f"Schema check passed ({checked} command(s) checked, {skipped} skipped).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
