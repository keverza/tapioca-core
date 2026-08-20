"""Check that no secret is committed and that provider configuration stays isolated.

Two separate rules, both from the plan:

1. No tracked file carries a credential. Scanning the tracked set rather than the
   working tree is deliberate: an ignored `.env` is exactly where a key is SUPPOSED
   to live, and flagging it would train the reader to ignore this tool.

2. Plan sections 2.9 and 14: the evaluator owns provider configuration and secrets
   entirely. Add-on, command, and UI code may not name a provider environment
   variable or endpoint, because that is how a key ends up travelling through the
   Archicad process to reach the model.

Exit codes: 0 clean, 1 violations found, 2 the tool could not run.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

EXIT_OK = 0
EXIT_VIOLATIONS = 1
EXIT_TOOL_ERROR = 2

# Only extensions worth reading. A credential in a .png is not a thing, and walking
# the vendored reference tree byte by byte makes the check too slow to run often.
TEXT_SUFFIXES = {
    ".py", ".ps1", ".psm1", ".cpp", ".hpp", ".h", ".c", ".cc", ".js", ".jsx", ".ts",
    ".tsx", ".json", ".yaml", ".yml", ".toml", ".ini", ".cfg", ".md", ".txt", ".env",
    ".xml", ".html", ".sh", ".bat", ".cmake", ".grc",
}

# Vendored SDKs and archived material are not ours to fix, and the archive exists to
# preserve history verbatim. Both are excluded from the plan's active-code rules.
SKIP_PREFIXES = ("AddOn/reference/", "archive/", "memory/", "dist/")

# Each pattern is a shape that is a credential wherever it appears, not a word that
# merely sounds like one. `api_key` as an identifier is fine; `api_key = "sk-..."`
# is not.
SECRET_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("AWS access key id", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("OpenAI-style key", re.compile(r"\bsk-[A-Za-z0-9]{20,}\b")),
    ("Anthropic key", re.compile(r"\bsk-ant-[A-Za-z0-9_\-]{20,}\b")),
    ("GitHub token", re.compile(r"\bgh[pousr]_[A-Za-z0-9]{30,}\b")),
    ("Google API key", re.compile(r"\bAIza[0-9A-Za-z_\-]{35}\b")),
    ("Slack token", re.compile(r"\bxox[baprs]-[0-9A-Za-z\-]{10,}\b")),
    ("private key block", re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |PGP )?PRIVATE KEY-----")),
    (
        "assigned credential literal",
        re.compile(
            r"""(?ix)
            \b(?:api[_-]?key|secret|password|passwd|token|access[_-]?key|client[_-]?secret)
            \s*[:=]\s*
            ["'][^"'\s${}<>]{12,}["']
            """
        ),
    ),
)

# An explicit per-line waiver, for the one case a placeholder cannot cover: a
# credential-shaped string that must be REAL-looking to be useful, i.e. this
# check's own test corpus. Excluding the whole tests directory instead would put
# a blind spot in the tree, and a real key in a fixture is still a leaked key.
#
# ⚠️ This check reads `git ls-files`, so a fixture is invisible until it is
# committed: the suite passed while its own test file was untracked and failed on
# the next run. A new corpus file needs the pragma from the start.
ALLOWLIST_PRAGMA = "pragma: allowlist secret"

# A placeholder is the documented way to SHOW the shape of a credential. Treating
# these as findings is what makes a secret scanner get switched off.
PLACEHOLDER = re.compile(
    r"(?i)(your[_-]?|example|placeholder|dummy|sample|redacted|changeme|xxxx|\.\.\.|"
    r"<[^>]+>|\$\{|%[A-Z_]+%|f?['\"]?\{[a-z_]+\})"
)

# Plan 2.9: the evaluator is the only place these may be named.
PROVIDER_MARKERS = re.compile(
    r"(?i)\b(?:OPENAI_API_KEY|ANTHROPIC_API_KEY|GOOGLE_API_KEY|MISTRAL_API_KEY|"
    r"GROQ_API_KEY|AZURE_OPENAI_KEY|api\.openai\.com|api\.anthropic\.com|"
    r"generativelanguage\.googleapis\.com)\b"
)
# Where provider configuration IS allowed to be named.
PROVIDER_ALLOWED_PREFIXES = ("services/evaluator/", "docs/architecture/evaluator/",
                             "project/decisions/", "CLEANUP-GOVERNANCE-PLAN.md",
                             "tools/quality/check_secrets.py")
# Code that must never name a provider, per plan section 2.9.
PROVIDER_ISOLATED_PREFIXES = ("AddOn/", "services/webui/", "AddOn/EvP/NotebookUI/")


def tracked_files(root: Path) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files"], cwd=root, capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        raise RuntimeError(f"git ls-files failed in {root}")
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def scan_file(rel: str, text: str) -> list[tuple[str, int, str]]:
    findings = []
    for number, line in enumerate(text.splitlines(), start=1):
        if len(line) > 4096:  # minified or generated; not hand-written source
            continue
        if ALLOWLIST_PRAGMA in line:
            continue
        for label, pattern in SECRET_PATTERNS:
            match = pattern.search(line)
            if match and not PLACEHOLDER.search(match.group(0)):
                findings.append((rel, number, label))
                break
        if rel.startswith(PROVIDER_ISOLATED_PREFIXES) and not rel.startswith(
            PROVIDER_ALLOWED_PREFIXES
        ):
            provider = PROVIDER_MARKERS.search(line)
            if provider:
                findings.append(
                    (rel, number, f"provider configuration outside the evaluator ({provider.group(0)})")
                )
    return findings


def check(root: Path) -> list[tuple[str, int, str]]:
    findings = []
    for rel in tracked_files(root):
        if rel.startswith(SKIP_PREFIXES) or "/_archive/" in rel:
            continue
        path = root / rel
        if path.suffix.lower() not in TEXT_SUFFIXES or not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        findings.extend(scan_file(rel, text))
    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root (defaults to the root containing this tool)",
    )
    args = parser.parse_args(argv)
    root = args.root.resolve()
    if not root.is_dir():
        print(f"TOOL ERROR: repository root does not exist: {root}", file=sys.stderr)
        return EXIT_TOOL_ERROR

    try:
        findings = check(root)
    except (RuntimeError, OSError) as error:
        print(f"TOOL ERROR: {error}", file=sys.stderr)
        return EXIT_TOOL_ERROR

    if not findings:
        print("Secret check passed (no committed credential, provider config isolated).")
        return EXIT_OK

    for rel, number, label in findings:
        print(f"{rel}:{number}: {label}", file=sys.stderr)
    print(f"\n{len(findings)} secret/isolation violation(s).", file=sys.stderr)
    return EXIT_VIOLATIONS


if __name__ == "__main__":
    raise SystemExit(main())
