# Build the configured AC29 project. Pass a config as arg1 (default RelWithDebInfo).
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$root = Join-Path $repo "AddOn\EvP"

# Governance is the first build gate when this is the private workspace. The public
# core deliberately has no task registry, so the same build script must remain usable
# there without weakening strict validation when the registry is present.
$governanceTool = @(
    (Join-Path $repo "tools\tapioca.py")
    (Join-Path (Split-Path -Parent $repo) "private\tools\tapioca.py")
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if ($null -eq $governanceTool) {
    Write-Host "Governance validation skipped: no private Tapioca CLI was found for this checkout." -ForegroundColor Yellow
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

# ---------------------------------------------------------------------------
# Copy the built artifacts into dist\ so they are in one findable place.
# ---------------------------------------------------------------------------
# A COPY, AND ONLY A COPY. Every target above still builds where CMake and
# dotnet put it, and nothing here moves, redirects or reconfigures any of that:
# GhWorkerHost still finds GhWorker\Tapioca.GhWorker.exe beside the .apx in
# build_29\, and TapiocaPackage.cs still installs the Grasshopper package from
# GhWorker\GrasshopperLibraries\. This exists because build_29\ is a CMake
# tree -- the .apx sits beside a 270 MB .pdb, three Diligent subprojects,
# .vcxproj files and two configurations of intermediates -- and picking the
# shipped binaries out of it by hand is a chore every single time.
#
# ⚠️ dist\ IS A PICKUP POINT, NOT A DEPLOYABLE LAYOUT. The files land flat, so
# nothing here is arranged the way Archicad or Grasshopper would need to load
# it. Take a copy from here; do not point a runtime at it.
#
# dist\ is tracked (it carries the generated API docs), so these binaries are
# ignored by name in .gitignore.
$dist = Join-Path $repo "dist"
New-Item -ItemType Directory -Force -Path $dist | Out-Null

# Named roots rather than a recursive sweep of build_29\: a recursive *.dll
# glob would rake in every Diligent, DiligentFX and DiligentTools binary and
# both configurations' test output, and the folder would stop being findable --
# which is the one thing it is for.
$artifactRoots = @(
    @{ Path = (Join-Path $root "build_29");          Recurse = $false }  # Tapioca.apx, EvPPy.dll
    @{ Path = (Join-Path $root "build_29\GhWorker"); Recurse = $true  }  # worker dll + Tapioca.Grasshopper.gha
)

$copied = @()
foreach ($entry in $artifactRoots) {
    if (-not (Test-Path -LiteralPath $entry.Path -PathType Container)) { continue }
    # Listed first and filtered second, rather than through -Include. -Include's
    # interaction with -Path, -LiteralPath and -Recurse is inconsistent enough
    # that it silently returned nothing from the worker's subfolder here, and a
    # copy step that quietly skips the .gha is exactly the failure this whole
    # block exists to avoid.
    $found = Get-ChildItem -LiteralPath $entry.Path -File -Recurse:$entry.Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in ".apx", ".dll", ".gha" }
    foreach ($file in $found) {
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $dist $file.Name) -Force
        $copied += $file.Name
    }
}

# Said by name, because an absence here is silent otherwise. The Grasshopper
# half is OPTIONAL BY DESIGN -- a machine without the .NET SDK still builds a
# complete, loadable Tapioca.apx and Grasshopper becomes a menu item that
# explains what is missing -- so a missing .gha is a warning, never a failure.
if ($copied -notcontains "Tapioca.Grasshopper.gha") {
    Write-Host "dist: Tapioca.Grasshopper.gha was not built (.NET SDK absent?); the Grasshopper Editor would open without Tapioca's components." -ForegroundColor Yellow
}
if ($copied -notcontains "EvPPy.dll") {
    Write-Host "dist: EvPPy.dll was not built; Python commands will not run." -ForegroundColor Yellow
}

Write-Host ("Copied to dist\: " + (($copied | Sort-Object -Unique) -join ", ")) -ForegroundColor Green
