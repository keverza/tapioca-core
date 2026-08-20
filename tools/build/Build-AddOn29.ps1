# Build the configured AC29 project. Pass a config as arg1 (default RelWithDebInfo).
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$root = Join-Path $repo "AddOn\EvP"

# Governance is the first build gate when this is the private workspace. The public
# core deliberately has no task registry, so the same build script must remain usable
# there without weakening strict validation when the registry is present.
$governanceTool = Join-Path $repo "tools\tapioca.py"
if (-not (Test-Path -LiteralPath $governanceTool -PathType Leaf)) {
    Write-Host "Governance validation skipped: tools/tapioca.py is not present in this checkout." -ForegroundColor Yellow
} elseif ($null -eq (Get-Command python -ErrorAction SilentlyContinue)) {
    throw "Strict governance validation requires Python on PATH."
} else {
    $governance = & python $governanceTool validate --strict 2>&1
    $governanceExit = $LASTEXITCODE
    if ($governanceExit -ne 0) {
        $governance | Write-Host
        # Exit 2 is the checker saying it could not run (a missing PyYAML/jsonschema, an
        # unreadable registry), NOT a registry violation. Reporting it as one sends the
        # reader hunting through tasks.yaml for a defect that is not there -- the same
        # confusion MIG-003 already fixed inside the checker's own output.
        if ($governanceExit -eq 2) {
            throw "Strict governance validation DID NOT RUN (tool error above). Install dev-requirements.txt for the `python` on PATH, then rebuild."
        }
        throw "Strict governance validation failed - fix the reported registry violations before building."
    }
}

. "$root\Find-CMake.ps1"
$cmake = Find-CMake
$configArgs = @($args | Where-Object { $_ -ne "-SkipArchCheck" })
$config = if ($configArgs.Count -ge 1) { $configArgs[0] } else { "RelWithDebInfo" }

# The structure gate, BEFORE the compiler: the rules cpp-architecture-plan.md landed
# (soft cap, palette seam, one factory per domain, all domains registered). Cheap
# (~0.2s) and deliberately blocking — a build that violates the architecture is not a
# build worth having. A legitimate exception is an edit to OVERSIZED in the script,
# which shows up in the diff. Skip for one build with -SkipArchCheck; it will still
# fail the next one, which is the point.
if ($args -notcontains "-SkipArchCheck") {
    if ($null -eq (Get-Command python -ErrorAction SilentlyContinue)) {
        # A missing interpreter is not an architecture violation — say so and build.
        Write-Host "architecture_check: python not found on PATH, skipping the structure gate." -ForegroundColor Yellow
    } else {
        $arch = & python "$repo\tools\quality\check_cpp.py"
        if ($LASTEXITCODE -ne 0) {
            $arch | Write-Host
            throw "Architecture check failed - see the list above. Fix it, or state the exception in tools/quality/check_cpp.py."
        }

        # ⚠️ A RESPONSE FIELD AND ITS SCHEMA ARE ONE EDIT, and forgetting that
        # fails EVERY call to the command with an error naming the field rather
        # than the command. It has cost a full in-Archicad run twice (2026-08-11
        # and 2026-08-13) and no other gate can see it: dryrun fakes the wire, so
        # no schema is consulted, and scan_commands reads only the Python side.
        $schema = & python "$root\tools\schema_check.py"
        if ($LASTEXITCODE -ne 0) {
            $schema | Write-Host
            throw "Schema check failed - see the list above. A response field and its schema are one edit."
        }

        # ⚠️ THE COMPILER NEVER SEES THE VIEWPORT'S SHADERS. They are raw string
        # literals in ArchViz/DiligentShaders.hpp, compiled at DEVICE-INIT time --
        # so a typo in HLSL passes this build, passes Sync-Commands, and appears
        # only as a BLACK VIEWPORT in Archicad with one line in a log. That is a
        # full user round trip for a syntax error, on the code path PLAT-RE51
        # edits constantly.
        #
        # It SKIPS (exit 0) when the Windows SDK's fxc is absent, so a machine
        # that can build a working .apx is never blocked by a missing SDK.
        $hlsl = & python "$repo\tools\quality\check_hlsl.py"
        if ($LASTEXITCODE -ne 0) {
            $hlsl | Write-Host
            throw "HLSL check failed - see the errors above. The shader would compile only when the viewport opens."
        }
        $hlsl | Select-Object -Last 1 | Write-Host
    }
}

# Always reconfigure: picks up newly added source files, and re-detects the VS
# instance so the same checkout builds on either dev machine.
& "$root\Initialize-Build29.ps1"

& $cmake --build "$root\build_29" --config $config
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)." }

# CMake's internal target remains EvP, but the product artifact loaded by
# Archicad is branded Tapioca. Keep the bridge DLL beside the renamed add-on.
$builtAddon = Join-Path $root "build_29\EvP.apx"
$productAddon = Join-Path $root "build_29\Tapioca.apx"
if (-not (Test-Path -LiteralPath $builtAddon -PathType Leaf)) {
    throw "Build completed but the add-on artifact was not found: $builtAddon"
}
if (Test-Path -LiteralPath $productAddon -PathType Leaf) {
    Remove-Item -LiteralPath $productAddon -Force
}
Move-Item -LiteralPath $builtAddon -Destination $productAddon
Write-Host "Built ($config) -> build_29\Tapioca.apx" -ForegroundColor Green
