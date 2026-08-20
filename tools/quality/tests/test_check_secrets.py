"""The secret check must find real credentials and stay quiet about placeholders.

A scanner that never fires is indistinguishable from one that is broken, and a
scanner that fires on documentation examples gets switched off. Both halves are
tested here for that reason.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import check_secrets  # noqa: E402


def _repo(tmp_path: Path, files: dict[str, str]) -> Path:
    subprocess.run(["git", "init", "-q"], cwd=tmp_path, check=True)
    for rel, text in files.items():
        path = tmp_path / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    subprocess.run(["git", "add", "-A"], cwd=tmp_path, check=True)
    return tmp_path


@pytest.mark.parametrize(
    "line",
    [
        # Not AKIAIOSFODNN7EXAMPLE: that is AWS's own documentation key, and the
        # placeholder rule is supposed to let it through.
        'AWS_ID = "AKIAQ7RT4WPZJK2NBVCX"',  # pragma: allowlist secret
        'key = "sk-ant-abcdefghijklmnopqrstuvwxyz0123"',  # pragma: allowlist secret
        'password = "hunter2hunter2hunter2"',  # pragma: allowlist secret
        "-----BEGIN RSA PRIVATE KEY-----",  # pragma: allowlist secret
    ],
)
def test_a_credential_literal_is_found(tmp_path, line):
    root = _repo(tmp_path, {"a.py": f"{line}\n"})

    findings = check_secrets.check(root)

    assert [rel for rel, _, _ in findings] == ["a.py"]


@pytest.mark.parametrize(
    "line",
    [
        'api_key = "your-api-key-here"',
        'token = os.environ["GITHUB_TOKEN"]',
        'password = "${DB_PASSWORD}"',
        "def set_api_key(api_key):",
        'secret = "<paste yours>"',
    ],
)
def test_a_placeholder_or_reference_is_not_a_finding(tmp_path, line):
    root = _repo(tmp_path, {"a.py": f"{line}\n"})

    assert check_secrets.check(root) == []


def test_provider_configuration_in_add_on_code_is_a_violation(tmp_path):
    root = _repo(tmp_path, {"AddOn/EvP/Commands/X/command.py": "url = 'api.openai.com'\n"})

    findings = check_secrets.check(root)

    assert len(findings) == 1
    assert "provider configuration outside the evaluator" in findings[0][2]


def test_the_evaluator_may_name_its_own_provider(tmp_path):
    root = _repo(tmp_path, {"services/evaluator/provider.js": "const host = 'api.openai.com';\n"})

    assert check_secrets.check(root) == []


def test_vendored_and_archived_trees_are_not_our_problem(tmp_path):
    root = _repo(
        tmp_path,
        {
            "AddOn/reference/sdk/sample.cpp": 'auto k = "AKIAIOSFODNN7EXAMPLE";\n',
            "archive/docs/old.md": 'password = "hunter2hunter2hunter2"\n',  # pragma: allowlist secret
        },
    )

    assert check_secrets.check(root) == []


def test_an_untracked_env_file_is_not_scanned(tmp_path):
    # An ignored .env is where a key is supposed to live.
    root = _repo(tmp_path, {"a.py": "x = 1\n"})
    secret_line = 'OPENAI_API_KEY = "sk-abcdefghijklmnopqrstuvwx"\n'  # pragma: allowlist secret
    (root / ".env").write_text(secret_line, encoding="utf-8")

    assert check_secrets.check(root) == []


def test_the_pragma_waives_only_the_line_that_carries_it(tmp_path):
    root = _repo(
        tmp_path,
        {
            "a.py": (
                'waived = "AKIAQ7RT4WPZJK2NBVCX"  # pragma: allowlist secret\n'
                'live = "AKIAQ7RT4WPZJK2NBVCY"\n'  # pragma: allowlist secret
            )
        },
    )

    findings = check_secrets.check(root)

    assert [line for _, line, _ in findings] == [2]


def test_this_corpus_is_scanned_rather_than_excluded():
    """The fixtures above are tracked and real-looking, so a directory-wide
    exclusion would hide a genuine key dropped into a test. This asserts the file
    IS read -- the bug this replaces was the scanner passing only because its own
    corpus was still untracked."""

    root = Path(__file__).resolve().parents[3]
    rel = Path(__file__).resolve().relative_to(root).as_posix()

    assert rel in check_secrets.tracked_files(root)
    text = Path(__file__).read_text(encoding="utf-8")
    assert check_secrets.scan_file(rel, text) == []
    # ...and it would fire here without the waiver.
    assert check_secrets.scan_file(
        rel, text.replace("  # pragma: allowlist secret", "")
    )


def test_the_real_repository_is_clean():
    root = Path(__file__).resolve().parents[3]

    assert check_secrets.check(root) == []
