# Clang-Format — C++ Layout for the EvP Add-On

One `clang-format` style for every first-party C++ source, adopted **incrementally**:
you format the files your commit touches, in that commit, and the rest of the tree
stays as it is until something else touches it.

**Read `docs/guides/coding-standards.md` first** — it owns the adoption policy and the
identifier convention. This file is the C++ layout detail only.

**Related:** `.clang-format` (the style, at the repository root),
`tools/quality/check_cpp.py` (the checker), `AddOn/EvP/.githooks/pre-commit` (the gate),
`AGENTS.md` (the identifier convention and the day-to-day commands).

---

## Status at a glance

| Piece | State |
|---|---|
| Style config (`.clang-format`, repository root) | **LANDED** — measured against this codebase, not just inferred |
| Second copy in `AddOn/EvP/` | **DELIBERATELY NOT DONE** — see "One file, not two" below |
| `clang-format` provisioning | **LANDED** — pinned wheel in `dev-requirements.txt`, resolved by `check_cpp.py` |
| Touched-file check | **LANDED** — `check_cpp.py --style-only` |
| In-place formatter | **LANDED** — `check_cpp.py --style-only --fix <file>` |
| Tree-wide progress report | **LANDED** — `check_cpp.py --report`, informational, never fails |
| Pre-commit gate | **LANDED** — staged `.cpp`/`.hpp` go through the same checker |
| Whole-tree reformat (275 files) | **OPTIONAL, NOT DONE** — 34 of 275 conform; see "The one rule that bites" |
| Identifier convention | **LANDED** — the table in `AGENTS.md` |

---

## Getting the tool

```powershell
pip install -r dev-requirements.txt
```

That installs the pinned `clang-format` wheel, which carries its own
`clang-format.exe`. The wheel's `Scripts` directory is usually **not** on `PATH`, so
`check_cpp.py` resolves the wheel as well as `PATH` — that is what makes the one-line
install sufficient on a fresh machine.

**The pin is deliberate.** A formatter's output is version-specific: two clang-format
majors produce two different "correct" formats, and an unpinned tool makes the tree
oscillate — every machine reformats what the last one just formatted. Move the pin in
its own commit and re-run `--report` to see what the new version claims.

Verify:

```powershell
python tools\quality\check_cpp.py --report
```

---

## Day-to-day

```powershell
python tools\quality\check_cpp.py --style-only <file> ...         # check
python tools\quality\check_cpp.py --style-only --fix <file> ...   # rewrite in place
python tools\quality\check_cpp.py --report                        # tree progress, never fails
python tools\quality\check_cpp.py                                 # architecture gate + staged style
```

Enable the gate once per clone:

```powershell
git config core.hooksPath AddOn/EvP/.githooks
```

The hook checks **staged files only**, Python through `check_python.py` and C/C++
through `check_cpp.py --style-only`. A missing tool warns and passes: a style hook
that blocks a commit on a machine without the tool is bypassed with `--no-verify`
once and then always.

---

## What the style says, and how it was decided

The config is at the repository root and carries its reasoning inline. The settings
divide into three groups.

**Confirmed from Graphisoft's DevKit samples** — `BreakBeforeBraces: Stroustrup`,
`SpaceBeforeParens: Always`, `IndentCaseLabels: true`, the four `AllowShort*: None`
settings, `IndentWidth: 4`.

**Measured against this codebase, and changed because of it:**

| Setting | Was | Is | Why |
|---|---|---|---|
| `UseTab` | `Always` | **`Never`** | The EvP tree has **zero** tab-indented lines and 15,325 four-space ones. `Always` rewrote 35,294 lines across a 113-file sample; `Never` rewrites 10,764 of the same lines — the tabs alone were **69% of the churn**. |
| `SpaceAfterCStyleCast` | unset (`false`) | **`true`** | `(DWORD) strlen (utf8)` is how this code is written, and it matches `SpaceBeforeParens`. Worth ~1,000 lines of the sample. |

**Measured and kept.** Every remaining setting was re-checked by reformatting the
`NativeCommands/` + `Python/` + `Geometry/` sample with the alternative and counting
changed lines. All alternatives were worse, so these now describe the codebase rather
than guessing at it:

| Alternative tried | Changed lines | Kept value |
|---|---|---|
| `SpaceBeforeParens: ControlStatements` | 20,082 | `Always` (baseline 10,764) |
| `ColumnLimit: 0` | 8,713 | `120` — costs ~2,000 lines of rewrapping, kept to match `line-length` in `pyproject.toml` |
| `AlignConsecutiveDeclarations: true` | 11,128 | `false` |
| `AlignConsecutiveAssignments: true` | 11,004 | `false` |
| `BreakBeforeBraces: Attach` | 11,786 | `Stroustrup` |
| `PointerAlignment: Right` | 11,094 | `Left` |
| `Cpp11BracedListStyle: true` | 11,440 | `false` |
| `AlignAfterOpenBracket: AlwaysBreak` | 11,009 | `Align` |
| `IndentCaseLabels: false` | 11,062 | `true` |

**The one unavoidable deviation:** `SpaceBeforeParens: Always` also puts a space before
`reinterpret_cast<...>(args)`, which the DevKit reference does not. The EvP code already
writes casts with the space, so `Always` is right for this codebase. `Custom` with
sub-options was tested and cannot express "all function parens including empty, but not
cast parens" — there is no `AfterCastParens: Never` knob.

---

## One file, not two

`.clang-format` at the repository root is the **only** copy, and that is a decision, not
an omission. clang-format searches upward from each source file, so one root file already
covers `AddOn/EvP/Sources/` entirely — a second copy under `AddOn/EvP/` would add nothing
except something to drift from. The earlier plan for a copy predates the root file.

The old dotless `AddOn/clang-format` has been deleted for the same reason: no tool ever
read it (no leading dot), and it had already drifted from the root copy in its header
comment.

The vendored trees under `AddOn/reference/` and the build tree carry their own
`.clang-format` files. Those are upstream's business; `check_cpp.py` skips those paths
entirely (`EXCLUDED_PARTS`).

---

## Scope: what is first-party

**275 C/C++ files** under `AddOn/EvP/Sources/`:

```
ArchViz/         96   the Diligent rendering stack
NativeCommands/  72   native command handlers, dispatchers
Palette/         35   DG palette shell, param panels, results table
Python/          22   PythonHost, ApiDispatcher, CommandRunner, gate
Geometry/        19   mesh query, serialisation
other AddOn/     28   Notify, Metadata, Server, Screenshot, PlanOverlay, Notebook,
                      Diagnostics, and the lifecycle root
EvPPy/            3   the C bridge DLL
```

**`ArchViz/` is IN** — the split the earlier plan left open is decided. A file-by-file
survey found no upstream copyright banner, no `SPDX` marker and no "derived from"
comment anywhere in the 96 files; the bgfx mentions that remain are all in prose
comments explaining a past decision or a vertex-packing convention, not copied code.
`cmake/Bgfx.cmake` was deleted with internal work item and Diligent is now the sole renderer, so
there is no upstream diff left to keep readable. `ArchViz/` is ours, and it converts on
the same touched-file terms as everything else.

---

## The one rule that bites here

**A whole-tree reformat is a merge-conflict generator, and it is not required.** As of
this landing, 34 of 275 files conform; the other 241 convert as they are touched. That
is the repository's stated adoption policy, and it is why the gate is per-file.

If a dedicated cleanup session ever does want the one-shot:

```powershell
python tools\quality\check_cpp.py --report --fix
```

Then, in the same session:

1. Land or rebase every in-flight C++ branch **first** — otherwise every branch
   conflicts on every file.
2. Commit the reformat alone, touching nothing else.
3. Record the commit in `.git-blame-ignore-revs` (the file explains the format), or the
   commit owns every line of `git blame` for the whole tree.
4. Re-run `.\tools\build\Build-AddOn29.ps1`. Only whitespace changed, so it must pass
   identically.

**Do not use the PowerShell glob the earlier version of this doc gave.**
`clang-format -i --style=file Sources\**\*.cpp` silently does nothing: PowerShell does
not expand `**` recursively and does not glob at all when passing arguments to a native
`.exe`, so clang-format receives the literal string. `--report --fix` above is the
supported path; by hand it would be
`Get-ChildItem Sources -Recurse -Include *.cpp,*.hpp | ForEach-Object { clang-format -i --style=file $_.FullName }`.

---

## Definition of done

- [x] `.clang-format` is at the repository root, is the only copy, and records why each
      measured setting has the value it has.
- [x] `clang-format` is provisioned by `dev-requirements.txt` and resolved by
      `check_cpp.py` whether or not it is on `PATH`.
- [x] The `ArchViz/` in/out question is decided and written down (in).
- [x] Touched-file check, in-place `--fix`, and an informational `--report` all exist.
- [x] The pre-commit hook catches a deliberately misformatted staged `.cpp`, and
      `--fix` then lets the same file through.
- [x] `tools/quality/tests/test_check_cpp_format.py` pins the gate's three contracts
      and the two measured settings.
- [x] `check_cpp.py -v` and `pytest -q` pass.
- [x] The identifier convention clang-format cannot check is written in `AGENTS.md`.
- [x] `.git-blame-ignore-revs` exists for a future mass pass (empty by design).
- [ ] **User step:** load the add-on in Archicad and press Rescan. An agent cannot run
      Archicad. Nothing in this change touches behaviour — no C++ source was reformatted
      by it — so this is a smoke test, not a gate.
