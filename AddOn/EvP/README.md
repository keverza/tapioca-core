# EvP component

This directory contains the Tapioca Archicad add-on component. `Sources/AddOn`
is the native add-on, `Sources/EvPPy` is the CPython bridge, and
`Sources/PyPackage` is the Python package staged beside the add-on at build time.

`rust/` provides the linked version/legacy geometry-validation foundation; no
geometry algorithm is migrated and attributed identity remains a separate gate.
Default add-on/offline-test builds require the pinned Rust toolchain. See
`rust/README.md`; `TAPIOCA_LINK_RUST_GEOMETRY=OFF` explicitly builds the legacy
no-Rust rollback path instead, not a Rust-validator acceptance run.

The repository root owns the build and quality entry points. Provision the
ignored reference tree before configuring the AC29 project:

```powershell
powershell -File tools/build/provision-reference.ps1 -FromUpstream
powershell -File tools/build/Build-AddOn29.ps1
```

The offline C++ suite needs its separate test profile:

```powershell
powershell -File tools/build/provision-reference.ps1 -FromUpstream -Profile test
powershell -File AddOn/EvP/tests/cpp/Invoke-CppTests.ps1
```

## UI boundaries

The retired Notebook and standalone WebUI panels are not built or shipped.
`GraphUI/` and `Palette/GraphEditorPalette.*` own the supported embedded editor;
`cmake/EmbeddedUI.cmake` remains its shared frontend build infrastructure.
The native control palette and HTTP data plane are unchanged.

`Python/CommandControlDispatch.*` and `Python/CommandRunState.*` retain the shared
command discovery, Run/Cancel/status/results and server-control bus verbs. The
Python `evp.webui` report helpers are independent of the removed panel and remain
available. The former panel's `/ui` HTTP routes are no longer registered.
