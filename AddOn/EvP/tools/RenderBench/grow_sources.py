"""Grow tools/RenderBench/Sources.txt until TapiocaRenderBench links.

Builds the bench, reads the linker's unresolved symbols, finds the source under Sources/AddOn that
defines each one, and appends it -- unless that source reaches the Archicad DevKit, in which case
it is reported for a decision (a stub, or cutting the dependency) and left out.

    python tools/RenderBench/grow_sources.py [--rounds N]
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

EVP = Path(__file__).resolve().parents[2]
ADDON = EVP / "Sources" / "AddOn"
LIST = EVP / "tools" / "RenderBench" / "Sources.txt"
DEVKIT = re.compile(r'#include\s+[<"](ACAPinc|APIEnvir|GSRoot|UniString|DG|ModelerAPI|GSUtils|Location)\b')

UNRESOLVED = re.compile(r'unresolved external symbol "(.*?)" \(')


def build():
    out = subprocess.run(
        ["cmake", "--build", str(EVP / "build_29"), "--config", "RelWithDebInfo", "--target",
         "TapiocaRenderBench", "--", "/v:m", "/nologo"],
        capture_output=True, text=True, errors="replace")
    return out.returncode, out.stdout + out.stderr


def symbol_key(sym):
    """('Class::Method' or 'Function', bare name) from an undecorated MSVC symbol."""
    sym = re.sub(r"^(public|private|protected): ", "", sym)
    sym = re.sub(r"^(static |virtual )+", "", sym)
    head = sym.split("(")[0]
    head = re.sub(r"<.*>", "", head)
    name = head.split(" ")[-1]
    parts = [p for p in name.split("::") if p]
    if not parts:
        return None
    last = parts[-1].lstrip("~")
    if len(parts) >= 2 and parts[-2][0].isupper() and parts[-2] not in ("Diligent",):
        return parts[-2] + "::" + parts[-1], last
    return parts[-1], last


def find_definition(key):
    qualified, bare = key
    files = []
    pattern = (re.compile(r"\b" + re.escape(qualified) + r"\s*\(") if "::" in qualified
               else re.compile(r"^[^\s/#][^;]*\b" + re.escape(bare) + r"\s*\([^;]*$", re.M))
    for path in ADDON.rglob("*.cpp"):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if pattern.search(text):
            files.append(path)
    return files


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=12)
    args = parser.parse_args()
    for round_ in range(args.rounds):
        code, log = build()
        if code == 0:
            print("linked")
            return 0
        compile_errors = [l for l in log.splitlines() if re.search(r"error C\d+", l)]
        if compile_errors:
            print("\n".join(dict.fromkeys(compile_errors)))
            return 1
        symbols = sorted(set(UNRESOLVED.findall(log)))
        listed = set(l.strip() for l in LIST.read_text().splitlines() if l.strip() and not l.startswith("#"))
        added, refused, unknown, why = [], {}, [], {}
        for sym in symbols:
            key = symbol_key(sym)
            if key is None:
                unknown.append(sym)
                continue
            files = find_definition(key)
            if not files:
                unknown.append(key[0])
                continue
            for f in files:
                rel = f.relative_to(ADDON).as_posix()
                if rel in listed or rel in added:
                    continue
                if DEVKIT.search(f.read_text(encoding="utf-8", errors="replace")):
                    refused.setdefault(rel, set()).add(key[0])
                    continue
                added.append(rel)
                why.setdefault(rel, set()).add(key[0])
        print(f"round {round_}: {len(symbols)} unresolved, +{len(added)} sources")
        for rel in added:
            print("  +", rel, "<-", ", ".join(sorted(why[rel]))[:160])
        for rel, keys in refused.items():
            print("  DEVKIT", rel, "<-", ", ".join(sorted(keys))[:200])
        for u in sorted(set(unknown))[:40]:
            print("  ?", u[:160])
        with LIST.open("a", encoding="utf-8") as f:
            for rel in added:
                f.write(rel + "\n")
        if not added or round_ + 1 == args.rounds:
            return 1
    return 1


if __name__ == "__main__":
    sys.exit(main())
