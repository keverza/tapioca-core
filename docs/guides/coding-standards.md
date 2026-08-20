# Coding Standards — One Style for C++, Python, and the Script Surface

Make the repo's style *mechanical* instead of remembered: one comment/naming rule
every agent follows, `ruff` enforcing PEP 8 on ~296 Python files, a clang-format
style measured against the add-on's own 275 C/C++ files, and one naming convention
for the build/sync scripts that every doc cites. **This doc is the single entry point —
read it, then the files it names.**

**Related:** `docs/guides/python-formatting.md` (Python detail — read it AFTER the corrections
in §Critical review below), `docs/guides/cpp-formatting.md` (C++ detail — same),
`historical source docs/archicad-quality-pipeline.md` (the CI plan this is the first step of),
`CLAUDE.md` + `AGENTS.md` (both get updated here), `.clang-format` at the repository
root (the C++ style, active), `pyproject.toml` (the Python one).

---

## Status at a glance

| Piece | State |
|---|---|
| §1 Comment/naming guide in `CLAUDE.md` + `AGENTS.md` | **LANDED** — why-not-what comments, informative names, and the ACAPI field-name exception are written in both |
| §2 `pyproject.toml` with `[tool.ruff]` | **LANDED** (internal work item) — one root config, not a second file under `AddOn/EvP/` |
| §2 `ruff` provisioned | **LANDED** — `dev-requirements.txt`; `check_python.py` warns and passes without it |
| §2 `ruff format` + `ruff check` (per-file, as touched) | **LANDED** — scanner guard first, then the touched-file gate; the whole-tree one-shot stays optional |
| §3 `.clang-format` style file | **LANDED** (internal work item) — at the repository root, the only copy, measured against this codebase; the inert dotless `AddOn/clang-format` is deleted |
| §3 `clang-format` provisioned | **LANDED** — pinned wheel in `dev-requirements.txt`, resolved by `check_cpp.py` off `PATH` too |
| §3 C++ reformatted (per-file, as touched) | **LANDED** — `check_cpp.py --style-only --fix`; ArchViz decided IN (§3.2); 34 of 275 files conform, the 241-file one-shot stays optional |
| §3 Identifier convention | **LANDED** — the table in `AGENTS.md`; no formatter can check naming |
| §4 Script naming convention | **SETTLED** — Verb-Noun PascalCase, `_29` suffix kept (§4.2) |
| §4 Rename + shim + documentation sweep | **LANDED** — canonical Verb-Noun names, permanent aliases/shims, and non-memory references updated |
| Enforcement (pre-commit / build gate) | **LANDED** — one versioned hook at `AddOn/EvP/.githooks/pre-commit` runs both languages over staged files; enable with `git config core.hooksPath AddOn/EvP/.githooks` |

**Nothing here needs a rebuild to *decide*.** Two things need one to *land*:
reformatting `Sources/PyPackage/evp/` (it reaches Archicad only via `Build-AddOn29.ps1`
— see `CLAUDE.md`'s PyPackage rule) and any change to `Build-AddOn29.ps1` itself.
Reformatting `Commands/` needs `Sync-Commands.ps1` + Rescan, nothing more.

---

## The four tasks

1. **Comment and naming guide** — fewer comments, more informative variable names,
   with an explicit carve-out for the comments this repo cannot afford to lose.
2. **PEP 8 for Python** — `ruff format` + `ruff check`, one config, enforced.
3. **C++ style for the Archicad add-on** — clang-format for layout, a written
   naming rule for identifiers (PascalCase types, camelCase variables, `API_`
   reserved to Graphisoft).
4. **Normalise the sync and build command names** — one convention across
   `Build-AddOn29.ps1` / `Sync-Commands.ps1` / `Find-CMake.ps1`, which today use three.

Do them **in this order**. §1 is free and unblocks agent behaviour immediately.
§4 must land *before* §2 and §3 if the renames touch the same docs, or you pay the
doc sweep twice.

---

## Adoption policy — forward-looking, never blocking

**These standards take effect moving forward.** New code is written to them from the
moment §1 lands. **Existing files are brought up to standard when they are touched
for other reasons**, or in a dedicated cleanup session — never as a precondition for
doing actual work.

**No big-bang reformat is required to start.** If you are building a feature and the
file you are in is not yet formatted, format *that file* as part of the commit and
move on. If a mass pass is wanted, it gets its own session and its own commit; it is
not a gate on anything else.

**This is a real constraint on the design, not a caveat.** It changes three things
the reference handoffs assume:

| Assumption in the reference docs | What incremental adoption requires instead |
|---|---|
| One atomic commit reformats all 230 C++ / 296 Python files, and must land before anything else merges | **Optional.** Each phase's "one-shot" step becomes a *convenience*, available whenever the tree is quiet. Nothing waits on it. |
| Enforcement runs `--dry-run -Werror` / `format --check` over the **whole tree** | **Must check staged/changed files only.** A whole-tree gate on a partly-converted tree fails every commit — which means it gets bypassed once and then always. |
| The merge-conflict warning is the dominant risk | Largely dissolves. A per-file conversion riding along with a real change conflicts with nothing but itself. |

**Two consequences to honour:**

- **The pre-commit hook checks only what is staged.** Never add a whole-tree check
  as a blocking gate until the tree is actually clean. If a whole-tree report is
  wanted before then, make it **informational** — print the count of
  not-yet-conforming files, exit 0.
- **Formatting churn stays out of feature diffs where it would drown them.** If
  reformatting a 900-line file would bury a 5-line fix, split it: one commit
  `format: <file>`, one commit with the change. Same file, two commits, reviewable.

Two things are still **not** incremental and should land as single commits, because
they are cheap and half-done versions are worse than not started: §1 (the written
rule) and §4 (the script renames — a half-renamed script surface is strictly worse
than either end state).

---

## Critical review of the two reference handoffs

Both docs are sound in shape. These are the defects an implementer would otherwise
hit; **fix them in the source docs as you go**, don't just work around them here.

**Both source docs have now been corrected in place** — internal work item rewrote
`python-formatting.md`, internal work item rewrote `cpp-formatting.md`. The tables below are
kept as the record of what was wrong and why, not as an outstanding to-do list.

### `docs/guides/cpp-formatting.md` (was `HANDOFF-ClangFormat.md`)

| Claim in the doc | Reality | Action |
|---|---|---|
| Status: style config **"DONE — verified"** | `AddOn/clang-format` has **no leading dot** and sits one directory above `Sources/`. No editor, IDE, or `--style=file` invocation picks it up. It is a template, not a config. | Downgrade the status to "written, inert". Phase 1's copy is what makes it real. |
| `Full config at AddOn/clang-format:1-42` | **Violates `CLAUDE.md`'s "never cite a line number in a doc"** — the rule that cost the repo 51 stale references. | Replace with `AddOn/clang-format` (no line range). |
| `clang-format -i --style=file Sources\**\*.cpp Sources\**\*.hpp` | **This silently does nothing.** Windows PowerShell 5.1 does not expand `**` recursively, and does not glob at all when passing arguments to a native `.exe` — clang-format receives the literal string `Sources\**\*.cpp`. | Use:<br>`Get-ChildItem Sources -Recurse -Include *.cpp,*.hpp \| ForEach-Object { clang-format -i --style=file $_.FullName }` |
| Scope table lists `ArchViz/ 62 files` alongside our own code | `Sources/AddOn/ArchViz/` contains bgfx- and Diligent-derived sample code. Reformatting it makes every future upstream diff unreadable. | **DECIDED: in.** The premise did not survive the survey — see §3.2. |
| Phase 2 assumes `clang-format.exe` is available | Unverified on this machine. The doc's own WSL/Windows version warning is right, but there is no "is the tool present, and at what version" step. | Add Phase 0 (§3.1). |
| DoD includes "Archicad palette Rescan works" | Not verifiable by an agent — `CLAUDE.md`: *you cannot run Archicad*. | Mark it explicitly as a **user step**. |
| No mention of `git blame` | A 230-file whitespace commit destroys blame for the whole tree. | Add `.git-blame-ignore-revs` (§5). |

### `docs/guides/python-formatting.md` (was `HANDOFF-PEP8.md`)

| Claim in the doc | Reality | Action |
|---|---|---|
| **"204 Python files"** | **296** `.py` files under `AddOn/EvP` excluding `build_29/`, `__pycache__/`, `.pytest_cache/`, `_deps/`. The 92 archived probe files now live under `historical source probes/2026/`, outside `AddOn/EvP`. | Restate the count and keep the frozen archive outside the active formatting pass. |
| Scope is `AddOn/EvP` only | The standalone script folders (`01 Python Palette Only/`, `02 Tapir/`, `03 Tapir + GeometryServer/`) are Python too and are **not mentioned at all**. | Settle it: see §2.2. |
| Caveat: *"import sorting might reorder `_scanner.py`'s imports"* | This is the wrong risk, and it hides the real one. **`ruff check --fix` with `SIM` and `C4` selected rewrites literals into comprehensions and `dict()` calls.** `CLAUDE.md`'s loudest Python rule is that annotation arguments must be **literals**: `evp.Enum(["a","b"])` scans, anything computed does not — and the command then **silently vanishes from the palette**, which looks exactly like a failed build. `py_compile` and `dryrun_command` both PASS on the broken form. This is the single highest-consequence defect in either doc. | Add the guard in §2.3. It is mandatory. |
| Verify step = `pytest -q` + `check_python.py` exit code | `check_python.py` exiting 0 proves the files *parse*, not that the **parsed metadata is unchanged**. A rewritten `evp.Enum` argument can drop a parameter's choices while the scan still succeeds. | Diff the scanner's JSON output before vs. after, not the exit code. Add `dryrun_command.py` to the list. |
| *"No rebuild is needed for this task"* | True for `Commands/`. **False for `Sources/PyPackage/evp/`** — the running add-on loads `evp` from `build_29/PyPackage/`, so until a rebuild the built copy differs from source. Behaviour is identical (whitespace), but a `ruff format --check` run that wanders into `build_29/` will report drift. | Scope the checks to `Sources/`, `Commands/`, `tests/`, `tools/`; never `build_29/`. |
| Pre-commit hook hardcodes `/.../repo/.venv/bin/ruff` | The user commits via **the configured Git client** (`private notes commit-preference.md`), which runs hooks under its own bundled Git — the absolute POSIX path may not resolve. | Resolve the interpreter relative to the repo root inside the hook, and make a missing `ruff` a loud skip rather than a blocked commit. |
| `line-length = 120`, `quote-style = "double"` | Correct and well argued — 120 matches `ColumnLimit: 120` in `AddOn/clang-format`. | **Keep as-is.** |

---

## §1 — The comment and naming guide

The instruction is *fewer comments, more informative variables*. Applied naively
to this repo that deletes the most valuable text in it. The rule has to distinguish
two kinds of comment.

**Delete the comment; rename the thing instead.** A comment that restates what the
next line already says is a maintenance liability — it goes stale silently.

```cpp
// get the element                          ← delete
API_Element e = {};                         ← rename to `wallElement`
```

```python
n = len(pts)   # number of points           ← delete both; it is `pointCount`
```

**Keep it, and expand it if it is thin.** A comment that records something the code
*cannot* show. This repo runs on these, and every one of them was paid for:

- **An ACAPI gotcha.** `NativeCommands/ElementReadCommands.cpp` →
  `GetElementInfoCommand` explains why it exists at all (Tapir's
  `GetDetailsOfElements` returns `NotYetSupportedElementTypeDetails` for a Roof;
  `header.floorInd` is available for every type). Nothing in the code says that.
- **A contract.** `Sources/EvPPy/EvPPyApi.h`'s header comment and
  `Python/MainThreadGate.hpp`'s comments **are** the ABI and the gate rules.
- **A decision and its cost.** "A miss still emits a full RECORD — a short list
  would silently shift every later element's data onto the wrong guid."
- **A unit, a convention, or an empirically-established constant** — `viewCone` is
  a *horizontal* FOV in degrees; `distance` is the 2D plan distance. These are
  invisible in the type.

Write it as one rule in `CLAUDE.md` and one line in `AGENTS.md`:

> **Comments: why, not what.** Delete any comment a better variable name would make
> redundant. Never delete one that records an ACAPI gotcha, a contract, a unit or
> convention, or why an obvious-looking alternative was rejected — those are the
> only durable documentation this repo has. Prefer `wallElement` /
> `visibleStoryIndex` / `pointCount` over `e` / `idx` / `n`; a name that needs a
> comment is the wrong name.

**Naming, both languages:**

- No single letters outside a tight loop index or a genuine math variable (`x`, `y`,
  `t` in geometry code is fine and clearer than `parameterAlongEdge`).
- No abbreviation that isn't already domain vocabulary. `guid`, `acapi`, `dg`,
  `gdl`, `bbox` are vocabulary; `elemInf`, `cnt`, `tmp2` are not.
- A boolean reads as a predicate: `hasSelection`, `isDryRun`, `needsMainThread`.
- **Exception, and it matters:** a local that mirrors an ACAPI struct field keeps
  the ACAPI name. `nCoords`, `nArcs`, `nSubPolys` in `CreateCommands.cpp` and
  `RoofCreateCommands.cpp` are `API_Polygon` member names. Renaming them to
  `coordinateCount` breaks the visual correspondence with the struct being filled,
  which is the whole reason the code is readable. **Do not de-Hungarian these.**

---

## §2 — PEP 8 for Python

Follow `docs/guides/python-formatting.md` for the config and the phases, **with these
corrections applied.**

### 2.1 Real scope

296 files under `AddOn/EvP`, excluding `build_29/`, `_deps/`, `__pycache__/`,
`.pytest_cache/`. By risk:

| Group | Files | Risk if `ruff` rewrites it |
|---|---|---|
| `Commands/*/command.py` (active) | ~50 | **HIGH** — the AST scanner reads these; a rewritten literal makes the command vanish from the palette |
| `Commands/*/*.py` helpers + tests | ~60 | Low — ordinary modules, covered by pytest |
| `Sources/PyPackage/evp/` + `tapioca/` | 26 | **Medium** — needs `Build-AddOn29.ps1` to reach Archicad; `_scanner.py` lives here |
| `tests/`, `tools/` | ~27 | Low |
| `historical source probes/2026/` | 92 | **None** — deactivated evidence outside the active command root |

**Keep `historical source probes/2026/` frozen and exclude it from `ruff check`** via
`per-file-ignores` — 92 files of lint noise from code nobody is going to fix is how
a lint gate gets ignored.

### 2.2 Settled: repo scope

`AddOn/EvP` **only**, for now. The three standalone script folders
(`historical source legacy-scripts/01 Python Palette Only/`,
`historical source legacy-scripts/02 Tapir/`, `historical source legacy-scripts/03 Tapir + GeometryServer/`
— see `docs/guides/scripts.md`) stay out of the first pass: they
are independently-run scripts with their own history, and folding them in triples
the blast radius of the one commit that must not conflict. Add them in a follow-up
once the config has proven itself. Say so in `pyproject.toml` as a comment.

### 2.3 The scanner guard — mandatory, do this before any `--fix`

`CLAUDE.md`: annotation arguments must be literals. `ruff check --fix` with `SIM`
and `C4` selected can rewrite them. A broken command does not raise — it silently
does not appear.

**Capture the scanner's parsed metadata before and after, and diff it:**

```bash
# BEFORE any formatting or fixing
/.../repo/.venv/bin/python tools/quality/check_python.py --json > /tmp/scan-before.json
```

> ⚠️ **Unverified:** `check_python.py` may not have a `--json` flag. Grep it
> first. If it doesn't, drive `evp._scanner.scan_file` over every command folder
> directly — `AGENTS.md` has the exact incantation (run from
> `AddOn/EvP/Sources/PyPackage`, with `PYTHONIOENCODING=utf-8`). Adding the flag is
> a legitimate small task inside this one.

Then, after `ruff format` and `ruff check --fix`:

```bash
/.../repo/.venv/bin/python tools/quality/check_python.py --json > /tmp/scan-after.json
diff /tmp/scan-before.json /tmp/scan-after.json      # MUST be empty
```

**An empty diff is the gate.** Not the exit code. If it isn't empty, the offending
rewrite is a `ruff` rule that has to be disabled repo-wide, not patched per file.

Belt and braces — start with `select` **minus `SIM` and `C4`** for the first pass,
land it, then turn those two on as a separate commit with the same diff gate. The
formatter alone cannot rewrite a literal into a call; only the linter can.

### 2.4 Verification, in order

1. `diff` of the scanner metadata is empty (§2.3).
2. `/.../repo/.venv/bin/pytest -q` from the repo root — identical pass/fail to before.
3. `/.../repo/.venv/bin/python AddOn/EvP/tests/dryrun_command.py AddOn/EvP/Commands/<Name>`
   on three commands with rich annotations (`MassingFeasibility`, `SunStudy`,
   `MeshFromContour` are the biggest).
4. `Build-AddOn29.ps1` — **required**, because `Sources/PyPackage/evp/` was touched.
5. **User step:** Rescan in the palette; confirm the command count is unchanged.

---

## §3 — C++ style for the Archicad add-on

Two separate things, and the reference doc only covers the first:

- **Layout** (braces, indentation, column limit) → clang-format, mechanical.
- **Identifiers** (PascalCase / camelCase / `API_`) → a written rule; clang-format
  cannot check naming. This is the part to add.

### 3.1 Phase 0 — prove the tool exists (the reference doc skips this)

**Done, and the answer was "no".** clang-format was on neither `PATH`, nor in LLVM's
usual install location, nor in any Visual Studio toolset on this machine. The version
skew this section warned about is real, so it was solved by removing the choice: a
**pinned `clang-format` wheel** in `dev-requirements.txt` carries its own binary, and
`tools/quality/check_cpp.py` resolves the wheel as well as `PATH`. One
`pip install -r dev-requirements.txt` now provisions it on any machine, at the version
the tree was measured with (22.1.8).

Move the pin in its own commit, and re-run `check_cpp.py --report` afterwards: a new
major reformats what the old one just formatted, which is exactly the oscillation this
section predicted.

### 3.2 Settled: what gets reformatted

**Everything first-party under `AddOn/EvP/Sources/` — 275 files, `ArchViz/` included.**

`ArchViz/` was the open question, and the survey dissolved it. Across its 96 files
there is no upstream copyright banner, no `SPDX` marker, and no "derived from" comment;
the bgfx mentions that remain are prose comments explaining a past decision or a
vertex-packing convention, not copied code. `cmake/Bgfx.cmake` was deleted with
internal work item and Diligent is the sole renderer now, so there is no upstream diff left to
keep readable. Nothing is excluded, and no per-file provenance comments were needed.

The counts in the reference doc were stale (it said 230 across six directories). The
current shape is `ArchViz/` 96, `NativeCommands/` 72, `Palette/` 35, `Python/` 22,
`Geometry/` 19, the remaining `AddOn/` directories 28, `EvPPy/` 3.

Excluded, and not first-party: `AddOn/reference/` and `AddOn/EvP/build_29/`, both of
which carry upstream's own `.clang-format` files. `check_cpp.py` skips those paths.

### 3.2a The style file had to be measured, not inherited

The config was inferred from Graphisoft's samples and never checked against this
codebase. Two settings were wrong, and one of them was severe:

- **`UseTab: Always` — wrong, and 69% of all the churn.** The EvP tree has **zero**
  tab-indented lines and 15,325 four-space ones. Formatting a 113-file sample with
  `Always` rewrote 35,294 lines; with `Never`, 10,764 of the same lines. Under an
  incremental policy — format the file you touched, in the commit that touched it — a
  setting that rewrites every indented line of that file buries the change inside it,
  and the developer skips the formatting instead. **Now `Never`.**
- **`SpaceAfterCStyleCast` — unset, should be true.** `(DWORD) strlen (utf8)` is how
  this code is written, and it is the same look as `SpaceBeforeParens: Always`. Worth
  ~1,000 lines of the sample. **Now `true`.**

Every other setting was re-checked the same way — reformat the sample with the
alternative, count changed lines — and every alternative was worse, so the rest of the
file now *describes* the codebase instead of guessing at it. The per-option numbers are
in `docs/guides/cpp-formatting.md`; the reasoning is inline in `.clang-format` itself.

**There is one style file, at the repository root, and no second copy.** clang-format
searches upward from each source, so the root file already covers `Sources/` entirely.
The earlier plan's `AddOn/EvP/.clang-format` copy predates the root file and would only
add something to drift from; the inert dotless `AddOn/clang-format` (which no tool ever
read, and which had already drifted in its header comment) is deleted.

### 3.3 The naming rule to write down

The codebase **already follows this** — a survey found camelCase/PascalCase
dominant, with two pockets. So this is a written rule plus two small cleanups, not
a rewrite.

| Kind | Convention | Example |
|---|---|---|
| Types, classes, structs, enums | **PascalCase** | `GetElementInfoCommand`, `MainThreadGate`, `NativeCommandResult` |
| Functions and methods | **PascalCase** | `ExecuteNative`, `GetName`, `PlaceAt` |
| Variables, parameters, members | **camelCase** | `wallElement`, `guidString`, `elementId` |
| Constants / `constexpr` | **PascalCase** or `kPascalCase` — pick one and state it | `MaxCoordinates` |
| Namespaces | lowercase | `geomsrv` |
| **`API_` prefix** | **Graphisoft's. Never invent one.** | `API_Element`, `API_WallType`, `API_Guid` |

Two clarifications the DevKit's own code will mislead you about:

- **Hungarian notation is legacy, not a target.** The older parts of the ACAPI use
  `nLen`, `pElem`. **Do not adopt it for new EvP code.** The only place it belongs
  is a local mirroring an ACAPI struct field (§1) — `nCoords` because the struct
  member is `nCoords`.
- **`API_`-prefixed names are Graphisoft's namespace.** Interfacing with
  `API_Element` is correct; defining `API_MyThing` is not. Our types go in
  `namespace geomsrv` with plain PascalCase.

**The written table now lives in `AGENTS.md`**, which is the file an agent actually
reads before editing. This section stays as the reasoning behind it.

**Cleanups this implied — both resolved, one of them differently than planned:**

- `result_code` → `resultCode`: **already gone.** No occurrence survives in the tree.
- The `s_` pocket: **kept, and promoted to a written rule** — a deliberate reversal of
  the plan's "rename to camelCase with an explicit `static`". Three reasons. It is not
  the snake_case leak it resembles: all 182 occurrences are file-scope statics in
  first-party Win32 window code (`PlanOverlay/OverlayWindow.cpp` 90,
  `ArchViz/ViewportOverlayWindow.cpp` 73, `NativeCommands/PlanOverlayCommands.cpp` 19),
  and not one is a local or a member. It **carries information the replacement loses**:
  `static` on the definition is invisible at the 182 *use* sites, where static storage
  duration is exactly what a reader of a window procedure needs to know. And renaming
  182 identifiers is unverifiable offline — an agent cannot run Archicad, so it would
  ship on inspection alone for no behavioural gain. `AGENTS.md` therefore records
  `s_` + camelCase as the convention for file-scope statics, scoped tightly: a local or
  a member named `s_anything` is still wrong.

**A naming gate stays optional, and is still not built.** The place for it would be
`tools/quality/check_cpp.py`, which already runs before the compiler and already fails
the build on structure violations. A regex pass for `\b[a-z]+_[a-z_]+\s*=` over
first-party `.cpp` with an allowlist remains a genuine option; now that the `s_` pocket
is legitimised, its false-positive rate is the open question rather than its value.

---

## §4 — Normalise the sync and build command names

### 4.1 The problem, and the number that decides the approach

Three conventions coexist in one folder:

| File | Convention |
|---|---|
| `build_29.ps1`, `generate_project_29.ps1` | snake_case + version suffix |
| `sync-commands.ps1`, `provision-runtime.ps1`, `build-magnum.ps1`, `tests/cpp/run-tests.ps1`, `tools/shaderc/build-shaderc.ps1`, `tools/shaderc/compile-shaders.ps1` | kebab-case, lowercase |
| `Find-CMake.ps1`, `Find-VSToolset.ps1`, `EvP-Environment.ps1` | PowerShell Verb-Noun, PascalCase |

**The cost of a rename: 261 occurrences of those four script names across 75
markdown files** (`CLAUDE.md` ×5, `AGENTS.md` ×9, `DEV-SETUP.md` ×11,
`historical source docs/evp-command-system-plan.md` ×30, and ~60 command `HANDOFF.md`s), plus
`Build-AddOn29.ps1`'s own internal calls to `Find-CMake.ps1` and
`Initialize-Build29.ps1`.

**And a hard constraint the reference docs don't have to deal with:**
`private notes MEMORY.md` and the per-topic memory files reference these names, and
`AGENTS.md` says never to write to them — they get overwritten. **So old names must
keep working indefinitely, not just for a deprecation window.** That settles the
approach: rename **plus permanent one-line forwarding shims**, never a bare rename.

### 4.2 Proposed convention — PowerShell's own

Adopt **Verb-Noun PascalCase with approved verbs**. It is the language's own rule,
three files already follow it, and it makes tab-completion and `Get-Verb` meaningful.

**The `_29` suffix is KEPT** (settled with the user — option **b**). The Archicad
version stays visible in the name rather than hiding in a parameter: the AC27 port
was skipped, but the number is the one thing about these two scripts that will
change, and an explicit `29` makes an AC30 script a new file rather than a silent
behaviour change in an existing one. The suffix goes on the **two version-specific
scripts only** — nothing else here is Archicad-version-bound.

| Today | Proposed | Verb approved? |
|---|---|---|
| `build_29.ps1` | `Build-AddOn29.ps1` | `Build` ✓ (Lifecycle) |
| `generate_project_29.ps1` | `Initialize-Build29.ps1` | `Initialize` ✓ (`Generate` is **not** an approved verb) |
| `sync-commands.ps1` | `Sync-Commands.ps1` | `Sync` ✓ (Data) |
| `provision-runtime.ps1` | `Install-Runtime.ps1` | `Install` ✓ (`Provision` is **not** approved) |
| `build-magnum.ps1` | `Build-Magnum.ps1` | ✓ |
| `tests/cpp/run-tests.ps1` | `Invoke-CppTests.ps1` | `Invoke` ✓ |
| `tools/shaderc/build-shaderc.ps1` | `Build-Shaderc.ps1` | ✓ |
| `tools/shaderc/compile-shaders.ps1` | `Build-Shaders.ps1` | ✓ |
| `Find-CMake.ps1`, `Find-VSToolset.ps1` | unchanged | ✓ |
| `EvP-Environment.ps1` | unchanged (dot-sourced env, not a command) | n/a |

Python tooling (`tools/quality/check_python.py`, `check_cpp.py`,
`check_structure.py`) is **already correct** — snake_case is
PEP 8 for modules. **Do not touch it.** The normalisation is PowerShell-only.

> **Settled:** the convention is Verb-Noun PascalCase, and the `_29` version suffix
> is kept on `Build-AddOn29.ps1` / `Initialize-Build29.ps1`. Nothing in §4 is open —
> proceed straight to 4.3.

### 4.3 Landing it safely

1. `git mv` each script to its new name — **one commit, renames only.**
2. Update the internal call sites in `Build-AddOn29.ps1` (it dot-sources
   `Find-CMake.ps1` and invokes `Initialize-Build29.ps1`) and anything in
   `CMakeLists.txt` that names a script. Grep for each old name in `.ps1`,
   `.py`, `.txt`, `.cmake` — **not just `.md`.**
3. **Add a permanent shim at each old name**, so every existing doc, memory file,
   and habit keeps working:
   ```powershell
   # build_29.ps1 — renamed to Build-AddOn29.ps1. Kept so older docs and memory
   # files (which are regenerated and cannot be edited) still work.
   & "$PSScriptRoot\Build-AddOn29.ps1" @args
   exit $LASTEXITCODE
   ```
   ⚠️ `@args` splatting and the `exit $LASTEXITCODE` are both load-bearing —
   `Build-AddOn29.ps1` takes a positional config plus `-SkipArchCheck`, and callers
   check the exit code.
   On Windows, `build-magnum.ps1`/`Build-Magnum.ps1`, `sync-commands.ps1`/`Sync-Commands.ps1`,
   and `build-shaderc.ps1`/`Build-Shaderc.ps1` differ only by case. NTFS cannot store a
   separate shim for those pairs, so the canonical implementation is also the permanent
   case-insensitive alias; the five names that differ on disk use the forwarding shim
   above.
4. **Scripted doc sweep**, separate commit: rewrite the references across the
   non-memory `.md` files. **Exclude `private notes `** (regenerated, must not be hand-edited) and
   `.claude/skills/` + `.agents/skills/` unless you also intend to own those.
5. Verify: run each canonical name and its old shim or case-insensitive alias, confirming
   identical output.

---

## Pressure-test caveats

- **IF you take the one-shot route, the whitespace commits are merge-conflict
  generators.** Both reference docs say this and both are right — but per the
  adoption policy above, the one-shot is optional. If you do it: land every
  in-flight change first. `master` is currently clean apart from
  `docs/guides/python-formatting.md`, so **the window is open now** — it will not be cheaper
  later. If you don't, nothing is lost; files convert as they are touched.
- **Order matters where the passes overlap.** §4's doc sweep and §2/§3's reformat
  touch large numbers of files. Do §4 first (`.md` and 8 `.ps1`), then §2 (`.py`),
  then §3 (`.cpp`/`.hpp`). Disjoint file sets, no interaction.
- **A partly-converted tree is the normal state, not a failure.** Anything that
  reports on the whole tree — a CI check, a build step, a status line — must treat
  non-conforming legacy files as *informational* until the conversion is actually
  finished. The moment such a report blocks a commit, the standard dies.
- **`Build-AddOn29.ps1` runs `check_cpp.py` before the compiler.** If §4 renames
  the script or §2 reformats `tools/quality/check_cpp.py`, a mistake there fails
  every build with a message about architecture, not about formatting. Run
  `python tools/quality/check_cpp.py -v` standalone after both.
- **`CLAUDE.md` and `AGENTS.md` are themselves subject to `check_cpp.py`**
  — it fails the build on line-number citations in them. When you add the style
  sections, cite `file → symbol` only.
- **Don't format what you can't test.** `historical source probes/2026/` is 92 files nobody
  runs; format it, don't lint it, and don't hand-fix its warnings.
- **A silent failure mode dominates each task**, and each has a specific detector:
  Python → command vanishes from palette → scanner-metadata diff (§2.3).
  C++ → glob expands to nothing, "success" with zero files changed → count the
  files clang-format actually touched. Scripts → a doc cites a name that no longer
  exists → the shims mean this degrades to noise, not breakage.

---

## §5 — Preserve `git blame`

Neither reference doc mentions this and both destroy blame for their whole tree.
`.git-blame-ignore-revs` now exists at the repository root with the format and the rule
written into it. It is **empty by design** — the conversion is incremental, so no commit
has earned an entry. After any future mass-reformat commit:

```bash
git log -1 --format=%H >> .git-blame-ignore-revs
git config blame.ignoreRevsFile .git-blame-ignore-revs
```

Commit `.git-blame-ignore-revs`. GitHub honours it automatically. One line per
whitespace commit, with a `#` comment naming what it was.

---

## Enforcement (do this last, once each style is landed)

**LANDED.** One hook, both languages, in `AddOn/EvP/.githooks/pre-commit`, enabled
once per clone with `git config core.hooksPath AddOn/EvP/.githooks` — versioned, unlike
`.git/hooks/`. It delegates: Python to `check_python.py --no-scan`, C/C++ to
`check_cpp.py --style-only`, so the selection and exclusion rules live in one place per
language rather than being restated in shell.

The whole-tree architecture gate stays out of the hook. `Build-AddOn29.ps1` runs it
before the compiler, which is where it belongs; repeating it on every commit would make
committing a doc change wait on a full source walk.

- Resolve the interpreter **relative to the repo root**, not an absolute path
  (§Critical review — the configured Git client runs hooks under its own Git).
- A missing `ruff` or `clang-format` **warns and passes**. A style hook that blocks
  a commit on a machine without the tool gets bypassed with `--no-verify` once and
  then always.
- Check staged files only. Exclude `build_29/`, `_deps/`, `reference/`.

The `check_cpp.py` gate is the model to follow for tone: cheap, blocking,
and it tells you exactly which rule and how to fix it.

---

## Definition of done

- [x] §1 Comment/naming rule is in `CLAUDE.md` **and** `AGENTS.md`, including the
      "keep the why-comments" carve-out and the ACAPI-mirroring-name exception.
- [x] §2 `pyproject.toml` carries `[tool.ruff]`; `ruff` is provisioned by `dev-requirements.txt`.
- [x] §2 Scanner metadata diff before/after is **empty** (the gate, not the exit code).
- [x] §2 `pytest -q` identical; `dryrun_command.py` passes on annotation-heavy commands.
- [x] §2 `ruff check` + `ruff format --check` clean **on every file the commit
      touches** (with `historical source ` lint-excluded). Whole-tree cleanliness over
      `Sources/ Commands/ tests/ tools/` is the cleanup-session goal, not a gate.
- [ ] §2 `Build-AddOn29.ps1` run (PyPackage was touched) — **user step:** Rescan, command
      count unchanged.
- [x] §3 `.clang-format` exists at the repository root, is the only copy, and records
      why each measured setting has the value it has. The plan's `AddOn/EvP/.clang-format`
      copy is deliberately **not** created (§3.2a); dotless `AddOn/clang-format` deleted.
- [x] §3 `clang-format` is provisioned (pinned wheel) and resolved off `PATH` too.
- [x] §3 The ArchViz in/out split is decided (**in**) and written down.
- [x] §3 Every first-party `.cpp`/`.hpp` **the commit touches** passes
      `clang-format --style=file --dry-run -Werror`. Whole-tree, same as above:
      cleanup-session goal, not a gate. 34 of 275 conform today.
- [x] §3 The identifier convention table is in `AGENTS.md`; the `s_` pocket is resolved
      (kept and written down as a rule, not renamed — §3.3 says why).
- [x] §3 `check_cpp.py -v` passes.
- [ ] §3 `Build-AddOn29.ps1` passes — **user step.** This change reformatted no C++
      source, so the build is unaffected by construction.
- [x] §4 Renames + shims landed (`Build-AddOn29.ps1`, `Initialize-Build29.ps1`,
      `Sync-Commands.ps1`, `Install-Runtime.ps1`, …); old names still work via the
      shims or Windows' case-insensitive alias; internal call sites and `CMakeLists.txt`
      updated.
- [x] §4 Documentation sweep done, `private notes ` untouched.
- [x] §5 `.git-blame-ignore-revs` exists, with one entry per *mass* reformat commit
      (none needed — the conversion is incremental, so the file is empty by design).
- [x] Both reference handoffs corrected in place (the line-number citation, the
      broken PowerShell glob, the file count, the scanner risk).
- [x] Pre-commit hook catches a deliberately misformatted `.py` and `.cpp`.

---

## Task Registry

The standards outcomes are registered in `private development tasks/platform.yaml`; this guide
does not duplicate task state.

---

## First move for the next session

**All four sections are landed.** §1 is in `CLAUDE.md` and `AGENTS.md`; §2 (Ruff) came
in with internal work item; §3 (clang-format plus the identifier convention) with internal work item;
§4 (the script renames and the doc sweep) is complete.

Setup on a new clone, once:

```powershell
pip install -r dev-requirements.txt
git config core.hooksPath AddOn/EvP/.githooks
git config blame.ignoreRevsFile .git-blame-ignore-revs
```

What is deliberately left open, in the order it would pay off:

1. **The whole-tree C++ conversion** — 241 of 275 files are unconverted. It needs its
   own session and its own commit (`check_cpp.py --report --fix`), a quiet tree with no
   in-flight C++ branches, and an entry in `.git-blame-ignore-revs`. Nothing waits on
   it; every file converts for free the next time someone touches it.
2. **The whole-tree Python conversion** — same shape, same optionality, and it must
   still go through the §2.3 scanner-metadata diff, which is the safety net for
   anything that touches `Commands/`.
3. **A naming gate** (§3.3) — optional, not built, and its open question is the
   false-positive rate now that `s_` is a legitimate prefix.

None of these three is a gate on any other work.
