---
title: Troubleshooting
description: Diagnose Tapioca installation, command scanning, runtime, and API issues.
sidebar:
  label: Troubleshooting
  order: 4
---

Most first-run problems happen at one of four handoffs: installation, runtime
provisioning, command scanning, or the native API boundary.

## The command is not in the palette

**Likely cause:** The command was not synced, Rescan was not pressed, or the
scanner could not parse the entry file.

**Solution:** Run the sync script from the repository root, press **Rescan**,
and check the command shape:

```powershell
python tools/quality/check_python.py --scan-only
powershell -File AddOn\EvP\Sync-Commands.ps1
```

## The runtime is missing

**Likely cause:** Tapioca's managed CPython runtime has not been provisioned
under the local application data folder.

**Solution:** Close Archicad and run `dist\Install-Runtime.ps1`. Do not repair
this first by changing the user's external Python installation.

## A native change is not visible

**Likely cause:** Archicad is still holding the previous add-on binary, or the
rebuild happened while the host was open.

**Solution:** Close Archicad, rebuild, restart Archicad, and reload the rebuilt
`.apx` through Add-On Manager.

## An API call fails before it runs

**Likely cause:** The namespace, request shape, or required field does not match
the registered contract.

**Solution:** Use canonical `Tapioca.*` names for new callers, inspect the
generated catalog in `dist/TAPIOCA-API-V2.md`, and keep the root request object
strict.

## The result is empty

**Likely cause:** The command expects a selection or a project property that is
not available in the current Archicad context.

**Solution:** Select elements before running a selection command, check for an
empty result in the command, and resolve optional properties rather than
assuming they exist.

## Offline checks pass but live behavior is wrong

Offline checks cannot prove Archicad loading, SDK behavior, palette layout, or
visual output. Treat live Archicad verification as a separate step and keep the
command log plus a small durable fixture or evidence record for the issue.

## Open a useful issue

Capture the exact command folder, sync output, relevant quality-check result,
and whether Archicad was open during the operation. Include:

- Archicad version and Tapioca release.
- Exact command or API name.
- Reproduction steps and expected result.
- Relevant log excerpt, without secrets.
- Offline checks already performed.

Open a report in the [GitHub issue tracker](https://github.com/keverza/tapioca-core/issues).

## More detail

- [Development machine setup](https://github.com/keverza/tapioca-core/blob/main/docs/guides/dev-setup.md)
- [Testing and validation](https://github.com/keverza/tapioca-core/blob/main/docs/guides/testing.md)
- [API contract and wire shapes](https://github.com/keverza/tapioca-core/blob/main/docs/architecture/api/SPEC.md)
