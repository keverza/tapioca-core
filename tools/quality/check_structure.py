"""Check the ControlPalette shell/sub-object seam.

The shell owns DG event dispatch, band layout, splitters, and run orchestration.
ParamPanel, ResultsTable, PreviewPanel, SelectionSetPanel, and CommandListPanel own their own
state and expose PlaceAt/handler methods. This check catches the old private-member
seam reappearing and verifies the public placement contract remains in the shell.

    python tools/quality/check_structure.py

A method is SPLIT when it touches both legacy private member groups. The current
shell should call sub-object methods instead, so the check also verifies the
orchestration calls in Layout directly.

⚠️ THE SHELL IS MORE THAN ONE FILE. ControlPaletteParams.cpp set that precedent
and ControlPaletteLayout.cpp / ControlPaletteRun.cpp follow it, which is what let
the band layout and the run state leave
ControlPalette.cpp without the size cap forcing a feature out instead. So this
check reads the shell as the SET of its implementation files: pass any one of
them (or nothing) and the siblings beside it are scanned too. Pinning Layout to
a single filename would have meant the architecture gate demanding an extraction
that this gate then rejected.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
DEFAULT = os.path.join(REPO, "AddOn", "EvP", "Sources", "AddOn", "Palette", "ControlPalette.cpp")

PARAM = [
    "paramControls",
    "ParamControl",
    "RebuildParamControls",
    "ClearParamControls",
    "CollectParamsJson",
    "paramsHeader",
    "UserControlTypeFor",
    "ParseLocalizedNumber",
    "ResolveDefaultFromNumber",
]
RESULT = [
    "resultsRows",
    "resultsHeaders",
    "resultsRowGuids",
    "resultsVisible",
    "resultsColumnCount",
    "resultsHScroll",
    "resultsTableHeight",
    "resultsTableTop",
    "resultsSplitter",
    "PopulateResultsTable",
    "BuildResultsListBox",
    "LayoutResultsColumns",
    "ResultsNeedsHScroll",
    "EnsureResultsScrollType",
    "ClearResults",
]

LAYOUT_CALLS = [
    "scroll.Begin",
    "commandsPanel.PlaceAt",
    "params.PlaceAt",
    "selectionSets.PlaceAt",
    "results.PlaceAt",
    "preview.PlaceAt",
    "scroll.End",
]

SHELL_METHODS = [
    "Layout",
    "RunSelected",
    "ButtonClicked",
    "PanelResized",
    "PanelIdle",
    "SplitterDragged",
    "SplitterDragExited",
]

SUBOBJECT_SOURCES = [
    "ParamPanel.cpp",
    "ResultsTable.cpp",
    "PreviewPanel.cpp",
    "SelectionSetPanel.cpp",
    "CommandListPanel.cpp",
]


SHELL_IMPLEMENTATION = [
    "ControlPalette.cpp",
    "ControlPaletteLayout.cpp",
    "ControlPaletteMenu.cpp",
    "ControlPaletteParams.cpp",
    "ControlPaletteRun.cpp",
]


def _shell_lines(path):
    """Every line of the shell, across its implementation files.

    Read in a FIXED order, not a directory listing's: a method's reported line
    number should not move because the filesystem felt differently today.
    """
    directory = os.path.dirname(os.path.abspath(path))
    lines = []
    for filename in SHELL_IMPLEMENTATION:
        candidate = os.path.join(directory, filename)
        if os.path.exists(candidate):
            lines.extend(open(candidate, encoding="utf-8").read().split("\n"))
    return lines


def main(path):
    if not os.path.exists(path):
        print(f"not found: {path}")
        print("Pass the shell .cpp explicitly if it has moved again.")
        return 2

    lines = _shell_lines(path)
    defs = []
    for i, ln in enumerate(lines):
        m = re.match(r"^[A-Za-z_][\w:<>,\s\*&]*\bControlPalette::(\w+)\s*\(", ln)
        if m:
            defs.append((i + 1, m.group(1)))
    if not defs:
        print("no ControlPalette:: method definitions found - wrong file?")
        return 2
    defs.append((len(lines) + 1, "<eof>"))

    print(f"{os.path.normpath(path)} + siblings  ({len(lines)} lines)\n")
    print(f"{'method':<32} {'lines':>6} {'param':>6} {'result':>7}   home")
    print("-" * 78)

    tally = {"ParamPanel": 0, "ResultsTable": 0, "shell": 0, "SPLIT": 0}
    splits = []
    method_bodies = {}
    for (start, name), (nxt, _) in zip(defs, defs[1:], strict=False):
        body = "\n".join(lines[start - 1 : nxt - 1])
        method_bodies[name] = body
        n = nxt - start
        p = sum(body.count(k) for k in PARAM)
        r = sum(body.count(k) for k in RESULT)
        if p and r:
            home = "SPLIT"
            splits.append((name, n))
        elif p:
            home = "ParamPanel"
        elif r:
            home = "ResultsTable"
        else:
            home = "shell"
        tally[home] += n
        print(f"{name:<32} {n:>6} {p:>6} {r:>7}   {home}{'  <<< straddles both' if home == 'SPLIT' else ''}")

    print("-" * 78)
    for k, v in tally.items():
        print(f"  {k:<14} {v:>5} lines")

    print()
    contract_failures = []
    for method in SHELL_METHODS:
        if method not in method_bodies:
            contract_failures.append(f"ControlPalette::{method} is missing from the shell.")

    layout = method_bodies.get("Layout", "")
    for call in LAYOUT_CALLS:
        if call not in layout:
            contract_failures.append(f"Layout does not place the shell sub-object through {call}.")

    shell_text = "\n".join(lines)
    stale_tokens = sorted({token for token in PARAM + RESULT if token in shell_text})
    if stale_tokens:
        contract_failures.append(
            "the shell contains extracted private-member tokens: " + ", ".join(stale_tokens)
        )

    palette_dir = os.path.dirname(path)
    for filename in SUBOBJECT_SOURCES:
        subobject_path = os.path.join(palette_dir, filename)
        if not os.path.exists(subobject_path):
            contract_failures.append(f"Palette/{filename} is missing.")
            continue
        text = open(subobject_path, encoding="utf-8").read()
        if "PlaceAt" not in text:
            contract_failures.append(f"Palette/{filename} has no PlaceAt contract.")
        if re.search(r"\bAttach\s*\(\s*\*this\s*\)", text):
            contract_failures.append(f"Palette/{filename} attaches itself instead of the shell observer.")

    if splits:
        print(f"{len(splits)} method(s) straddle both sub-objects ({sum(n for _, n in splits)} lines):")
        for name, n in splits:
            print(f"  * {name} ({n} lines)")
        print("\nPhase 2 is not finished while this list is non-empty.")
        return 1

    if contract_failures:
        print("Seam contract failures:")
        for failure in contract_failures:
            print(f"  * {failure}")
        return 1

    print("No method straddles both sub-objects - the seam is clean.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else DEFAULT))
