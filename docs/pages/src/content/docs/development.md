---
title: Development
description: Build, test, and contribute to the Tapioca Archicad add-on.
sidebar:
  label: Development
  order: 5
---

The public core keeps native code, the shipped Python package, examples, tests,
and build tools together. The add-on build is Windows-only.

## Prerequisites

- Visual Studio with C++ tools, MSVC v143 x64/x86, and C++ ATL for v143.
- Archicad 29 and a licensed interactive desktop session for live validation.
- Python 3.12, CMake, and Node.js/npm.
- The catalog-listed AC29 DevKit and dependencies in the ignored
  `AddOn/reference` tree.

Install the development packages from the repository root:

```powershell
python -m pip install -r dev-requirements.txt
```

## Build from source

Run from the public repository root in Windows PowerShell, with Archicad closed:

```powershell
powershell -File tools/build/provision-reference.ps1 -FromUpstream
powershell -File tools/build/Build-AddOn29.ps1
powershell -File AddOn/EvP/Sync-Commands.ps1
```

The reference provisioner fills the ignored dependency tree. The build produces
the AC29 add-on, and the sync script deploys public commands to the local
palette directory.

## Quality gates

Run the checks relevant to the files you touched:

```powershell
python tools/quality/check_secrets.py
python tools/quality/check_python.py
python tools/quality/check_cpp.py -v
python tools/quality/check_structure.py
pytest -q
powershell -File AddOn/EvP/tests/cpp/Invoke-CppTests.ps1
```

Offline checks do not prove that Archicad loads the binary, that a live SDK
operation behaves correctly, or that a palette or viewer looks right. Record
live evidence separately.

## Contributing

1. Read [`AGENTS.md`](https://github.com/keverza/tapioca-core/blob/main/AGENTS.md)
   and the owning specification.
2. Make the smallest change that preserves the native/Python boundary.
3. Run the quality checks for the files you touched.
4. Open an issue or pull request with the verification performed.

Technical documentation belongs close to the code it explains. Start with the
repository's [guides](https://github.com/keverza/tapioca-core/tree/main/docs/guides)
and [architecture specifications](https://github.com/keverza/tapioca-core/tree/main/docs/architecture).
