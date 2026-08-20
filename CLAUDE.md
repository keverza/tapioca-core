# CLAUDE.md

This repository is the public Tapioca core: a clonable Archicad 29 add-on,
its shipped Python package, build tools, quality gates, examples, and contracts.
Read the component README before changing build or runtime boundaries.

## Code Rules

- Archicad SDK calls run only on the main thread through `MainThreadGate`.
- Never capture a local by reference in a gate lambda; a timed-out job may run later.
- Never open an undo scope inside a native command; the dispatcher owns transactions.
- Keep the EvP.apx, EvPPy.dll, and Python package boundary as extern-C POD data.
- Never build UI in Python. Python requests native UI through the `tapioca` surface.
- Use `Tapioca.*` command names in new callers and diagnostics.
- Grep the checked-out DevKit or the official Python package before citing an API.
- Keep native domain operations in `AddOn/EvP/Sources/AddOn/NativeCommands/`.
- Format only touched C++ files with `python tools/quality/check_cpp.py --style-only --fix`.
- Keep command annotation arguments AST-readable literals or the palette will omit them.

## Layout

- Native add-on: `AddOn/EvP/Sources/AddOn/`
- CPython bridge: `AddOn/EvP/Sources/EvPPy/`
- Shipped Python package: `AddOn/EvP/Sources/PyPackage/`
- Public examples: `Examples/`
- Shared command helpers: `AddOn/EvP/Commands/_lib/`
- Canonical probe: `AddOn/EvP/Diagnostics/Probes/PlanAnchorProbe/`
- Offline tests: `AddOn/EvP/tests/` and `AddOn/EvP/tests/cpp/`
- Build and quality tools: `tools/build/` and `tools/quality/`

## Validation

```powershell
python tools/quality/check_secrets.py
python tools/quality/check_python.py
python tools/quality/check_cpp.py -v
python tools/quality/check_structure.py
pytest -q
powershell -File AddOn/EvP/tests/cpp/Invoke-CppTests.ps1
```

Native builds require Windows PowerShell, Visual Studio, the AC29 DevKit, and
Archicad closed. Provision the ignored reference tree with
`tools/build/provision-reference.ps1 -FromUpstream`.
