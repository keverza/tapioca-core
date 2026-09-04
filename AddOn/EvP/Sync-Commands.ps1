# Compose the manifest-defined command roots into the flat scripts root Archicad reads.
#
# The repo is the SOURCE OF TRUTH; %LOCALAPPDATA%\Tapioca\Commands is a generated
# working copy. The checked-in command-sync.json is the public source-root allowlist.
# A local, gitignored command-sync.local.json may add private roots when this component
# is mounted under the tapioca-dev superproject. The target-side ownership file records
# what this script deployed so -Prune cannot remove an unrelated folder.
#
#   .\Sync-Commands.ps1            copy changed files
#   .\Sync-Commands.ps1 -WhatIf    show what would change, touch nothing
#   .\Sync-Commands.ps1 -Prune     remove stale folders owned by this sync
#
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch] $Prune
)

$ErrorActionPreference = 'Stop'

$manifestPath = Join-Path $PSScriptRoot 'command-sync.json'
$ownershipFileName = '.tapioca-sync-state.json'

if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "No command sync manifest at $manifestPath"
}

try {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
} catch {
    throw "Could not parse command sync manifest at ${manifestPath}: $($_.Exception.Message)"
}

if ($manifest.version -ne 1) {
    throw "Unsupported command sync manifest version '$($manifest.version)'"
}

$overlayPath = Join-Path $PSScriptRoot 'command-sync.local.json'
$extraSourceRoots = @()
$extraSharedRoots = @()
if (Test-Path -LiteralPath $overlayPath -PathType Leaf) {
    try {
        $overlay = Get-Content -LiteralPath $overlayPath -Raw | ConvertFrom-Json
    } catch {
        throw "Could not parse local command sync overlay at ${overlayPath}: $($_.Exception.Message)"
    }
    if ($null -eq $overlay.PSObject.Properties['version'] -or $overlay.version -ne 1) {
        throw "Unsupported local command sync overlay version '$($overlay.version)'"
    }
    if ($null -eq $overlay.PSObject.Properties['extra_source_roots']) {
        throw "Local command sync overlay must define extra_source_roots"
    }
    $extraSourceRoots = @($overlay.extra_source_roots)
    # Optional, and deliberately so: an overlay written before workflow node
    # folders existed is still a valid overlay, and failing on one would make a
    # workspace unusable until Setup-Workspace.ps1 was re-run.
    if ($null -ne $overlay.PSObject.Properties['extra_shared_roots']) {
        $extraSharedRoots = @($overlay.extra_shared_roots)
    }
}

function Assert-SafeRelativePath {
    param(
        [Parameter(Mandatory = $true)] [string] $Value,
        [Parameter(Mandatory = $true)] [string] $Label
    )

    if ([string]::IsNullOrWhiteSpace($Value) -or
        [System.IO.Path]::IsPathRooted($Value) -or
        $Value -match '(^|[\\/])\.\.([\\/]|$)') {
        throw "$Label must be a non-empty repository-relative path: '$Value'"
    }
}

function Assert-SafeOverlayPath {
    param(
        [Parameter(Mandatory = $true)] [string] $Value,
        [Parameter(Mandatory = $true)] [string] $Label
    )

    # The overlay is the deliberate bridge from core/ to private/. It may contain
    # '..' segments, but it may not turn into an absolute path or an empty entry.
    if ([string]::IsNullOrWhiteSpace($Value) -or [System.IO.Path]::IsPathRooted($Value) -or
        $Value -match '^[A-Za-z]:[\\/]' -or $Value -match '[*?]') {
        throw "$Label must be a non-empty relative path: '$Value'"
    }
}

function Assert-SafeManifestPath {
    param(
        [Parameter(Mandatory = $true)] [string] $Value,
        [Parameter(Mandatory = $true)] [string] $Label
    )

    if ([string]::IsNullOrWhiteSpace($Value) -or [System.IO.Path]::IsPathRooted($Value) -or
        $Value -match '^[A-Za-z]:[\\/]' -or $Value -match '[*?]') {
        throw "$Label must be a non-empty repository-relative path: '$Value'"
    }
    $candidate = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ($Value -replace '/', '\')))
    $repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
    $prefix = $repositoryRoot.TrimEnd('\') + '\'
    if (-not $candidate.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must resolve inside the repository: '$Value'"
    }
}

function Assert-SafeFolderName {
    param(
        [Parameter(Mandatory = $true)] [string] $Value,
        [Parameter(Mandatory = $true)] [string] $Label
    )

    Assert-SafeRelativePath -Value $Value -Label $Label
    if ($Value -match '[\\/]') {
        throw "$Label must be one target folder name, not a nested path: '$Value'"
    }
}

$exclude = $manifest.exclude
$excludedDirectories = @($exclude.directories)
$excludedFilePatterns = @($exclude.file_patterns)
if ($excludedDirectories.Count -eq 0 -or $excludedFilePatterns.Count -eq 0) {
    throw 'The command sync manifest must define non-empty exclude directories and file_patterns'
}

function Test-ExcludedPath {
    param([Parameter(Mandatory = $true)] [string] $RelativePath)

    foreach ($segment in ($RelativePath -split '[\\/]')) {
        foreach ($excluded in $excludedDirectories) {
            if ($segment -ieq [string] $excluded) {
                return $true
            }
        }
    }
    return $false
}

function Test-ExcludedFile {
    param([Parameter(Mandatory = $true)] [string] $RelativePath)

    if (Test-ExcludedPath -RelativePath $RelativePath) {
        return $true
    }

    $fileName = [System.IO.Path]::GetFileName($RelativePath)
    foreach ($pattern in $excludedFilePatterns) {
        if ($fileName -like [string] $pattern) {
            return $true
        }
    }
    return $false
}

$sourceRootEntries = @()
foreach ($sourceRoot in @($manifest.source_roots)) {
    $sourceRootEntries += [pscustomobject]@{ Value = $sourceRoot; IsOverlay = $false }
}
foreach ($sourceRoot in $extraSourceRoots) {
    $sourceRootEntries += [pscustomobject]@{ Value = $sourceRoot; IsOverlay = $true }
}
if ($sourceRootEntries.Count -eq 0) {
    throw 'The command sync manifest must define at least one source root'
}

$rootPlans = @()
$rootKeys = @{}
foreach ($sourceRootEntry in $sourceRootEntries) {
    $sourceRoot = $sourceRootEntry.Value
    if ($sourceRoot -isnot [string]) {
        throw 'Every command sync source root must be a string'
    }

    if ($sourceRootEntry.IsOverlay) {
        Assert-SafeOverlayPath -Value $sourceRoot -Label 'Local command sync source root'
    } else {
        Assert-SafeManifestPath -Value $sourceRoot -Label 'Command sync source root'
    }
    $relativeRoot = $sourceRoot -replace '/', '\'
    $rootKey = $relativeRoot.ToLowerInvariant()
    if ($rootKeys.ContainsKey($rootKey)) {
        throw "Duplicate command sync source root '$sourceRoot'"
    }
    $rootKeys[$rootKey] = $true

    $rootPath = Join-Path $PSScriptRoot $relativeRoot
    if (-not (Test-Path -LiteralPath $rootPath -PathType Container)) {
        throw "Command sync source root does not exist: $rootPath"
    }

    $rootPlans += [pscustomobject]@{
        RelativePath = $relativeRoot
        Path = $rootPath
    }
}

# A shared root copies one tree to one named folder under the target, rather than
# treating it as a container of command folders. That is what deploys _lib, and
# it is what deploys the WORKFLOW NODE LIBRARY: a script node is a folder of
# .py files with no command.py in it, so it would be skipped entirely by the
# command-folder walk above.
$sharedRootEntries = @()
foreach ($sharedRoot in @($manifest.shared_roots)) {
    $sharedRootEntries += [pscustomobject]@{ Value = $sharedRoot; IsOverlay = $false }
}
foreach ($sharedRoot in $extraSharedRoots) {
    $sharedRootEntries += [pscustomobject]@{ Value = $sharedRoot; IsOverlay = $true }
}

$sharedPlans = @()
$sharedTargetOwners = @{}
foreach ($sharedRootEntry in $sharedRootEntries) {
    $sharedRoot = $sharedRootEntry.Value
    if ($null -eq $sharedRoot.source -or $null -eq $sharedRoot.target) {
        throw 'Every shared command sync root needs source and target'
    }

    # The overlay is the deliberate bridge from core/ to private/, so its sources
    # may climb out of the submodule with '..' - exactly as extra_source_roots
    # already may. A manifest source may not.
    if ($sharedRootEntry.IsOverlay) {
        Assert-SafeOverlayPath -Value ([string] $sharedRoot.source) -Label 'Local shared command sync source'
    } else {
        Assert-SafeRelativePath -Value ([string] $sharedRoot.source) -Label 'Shared command sync source'
    }
    Assert-SafeFolderName -Value ([string] $sharedRoot.target) -Label 'Shared command sync target'

    $relativeSource = ([string] $sharedRoot.source) -replace '/', '\'
    # NORMALISED, not merely joined. An overlay source climbs out of the submodule
    # with '..', and Get-FilePlans slices the file's own FullName - which Windows
    # has already collapsed - by this path's length. An uncollapsed root is longer
    # than the paths it contains, and the slice throws.
    $sharedPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $relativeSource))
    if (-not (Test-Path -LiteralPath $sharedPath -PathType Container)) {
        throw "Shared command sync root does not exist: $sharedPath"
    }

    $targetName = [string] $sharedRoot.target
    $targetKey = $targetName.ToLowerInvariant()
    if ($sharedTargetOwners.ContainsKey($targetKey)) {
        # Two roots deploying into one folder is a hard error rather than
        # last-writer-wins: the per-file collision check below would catch a
        # clashing FILE, but two roots that happen not to overlap today would
        # start silently overwriting each other the day somebody adds a file.
        throw "Duplicate shared command sync target '$targetName'"
    }
    $sharedTargetOwners[$targetKey] = $relativeSource

    $sharedPlans += [pscustomobject]@{
        Source = $sharedPath
        SourceLabel = $relativeSource
        Target = $targetName
    }
}

$folderPlans = @()
$folderOwners = @{}
foreach ($rootPlan in $rootPlans) {
    # A public core may expose one canonical probe without exposing the whole
    # private probe directory. Treat a source root that is itself a command as a
    # one-folder root; normal roots still contain one folder per command.
    $rootEntryPoint = Join-Path $rootPlan.Path 'command.py'
    $sourceFolders = if (Test-Path -LiteralPath $rootEntryPoint -PathType Leaf) {
        @(Get-Item -LiteralPath $rootPlan.Path)
    } else {
        @(Get-ChildItem -LiteralPath $rootPlan.Path -Directory -Force)
    }
    foreach ($sourceFolder in $sourceFolders) {
        if ($sourceFolder.Name -like '_*' -or (Test-ExcludedPath -RelativePath $sourceFolder.Name)) {
            continue
        }

        # Handoffs and task folders are source documentation, not runtime commands.
        $entryPoint = Join-Path $sourceFolder.FullName 'command.py'
        if (-not (Test-Path -LiteralPath $entryPoint -PathType Leaf)) {
            continue
        }

        $folderKey = $sourceFolder.Name.ToLowerInvariant()
        if ($folderOwners.ContainsKey($folderKey)) {
            throw "Source collision: command folder '$($sourceFolder.Name)' is provided by '$($folderOwners[$folderKey])' and '$($rootPlan.RelativePath)'"
        }
        if ($sharedTargetOwners.ContainsKey($folderKey)) {
            throw "Source collision: command folder '$($sourceFolder.Name)' conflicts with shared target '$($sharedTargetOwners[$folderKey])'"
        }

        $folderOwners[$folderKey] = $rootPlan.RelativePath
        $folderPlans += [pscustomobject]@{
            Source = $sourceFolder.FullName
            SourceLabel = Join-Path $rootPlan.RelativePath $sourceFolder.Name
            Target = $sourceFolder.Name
        }
    }
}

if ($folderPlans.Count -eq 0) {
    throw 'The command sync manifest produced no command folders'
}

function Get-FilePlans {
    param(
        [Parameter(Mandatory = $true)] [string] $Source,
        [Parameter(Mandatory = $true)] [string] $SourceLabel,
        [Parameter(Mandatory = $true)] [string] $Target
    )

    $plans = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $Source -Recurse -File -Force)) {
        $relative = $file.FullName.Substring($Source.Length + 1)
        if (Test-ExcludedFile -RelativePath $relative) {
            continue
        }

        $plans += [pscustomobject]@{
            Source = $file.FullName
            SourceLabel = Join-Path $SourceLabel $relative
            Destination = Join-Path $Target $relative
        }
    }
    return $plans
}

$filePlans = @()
foreach ($folderPlan in $folderPlans) {
    $filePlans += Get-FilePlans -Source $folderPlan.Source -SourceLabel $folderPlan.SourceLabel -Target $folderPlan.Target
}
foreach ($sharedPlan in $sharedPlans) {
    $filePlans += Get-FilePlans -Source $sharedPlan.Source -SourceLabel $sharedPlan.SourceLabel -Target $sharedPlan.Target
}

$fileOwners = @{}
foreach ($filePlan in $filePlans) {
    $destinationKey = $filePlan.Destination.ToLowerInvariant()
    if ($fileOwners.ContainsKey($destinationKey)) {
        throw "Source collision: '$($filePlan.SourceLabel)' and '$($fileOwners[$destinationKey])' both deploy '$($filePlan.Destination)'"
    }
    $fileOwners[$destinationKey] = $filePlan.SourceLabel
}

$activeFolders = @($folderPlans | ForEach-Object { $_.Target })
$activeFolders += @($sharedPlans | ForEach-Object { $_.Target })
$activeFolders = @($activeFolders | Sort-Object -Unique)
$activeFolderKeys = @{}
foreach ($activeFolder in $activeFolders) {
    $activeFolderKeys[$activeFolder.ToLowerInvariant()] = $true
}

$localAppData = $env:LOCALAPPDATA
if ([string]::IsNullOrWhiteSpace($localAppData)) {
    throw "Windows did not provide LOCALAPPDATA; run Sync-Commands.ps1 in native Windows PowerShell"
}

$target = Join-Path $localAppData 'Tapioca\Commands'
$legacyTargets = @()
$documents = [Environment]::GetFolderPath('MyDocuments')
if (-not [string]::IsNullOrWhiteSpace($documents)) {
    $legacyTargets += Join-Path $documents 'Tapioca Commands'
    $legacyTargets += Join-Path $documents 'EvP Commands'
}
$ownershipPath = Join-Path $target $ownershipFileName

$previousFolders = @()
if ($Prune -and (Test-Path -LiteralPath $ownershipPath -PathType Leaf)) {
    try {
        $ownership = Get-Content -LiteralPath $ownershipPath -Raw | ConvertFrom-Json
    } catch {
        throw "Could not parse target ownership manifest at ${ownershipPath}: $($_.Exception.Message)"
    }
    if ($ownership.version -ne 1) {
        throw "Unsupported target ownership manifest version '$($ownership.version)'"
    }
    foreach ($folder in @($ownership.folders)) {
        Assert-SafeFolderName -Value ([string] $folder) -Label 'Target ownership folder'
        $previousFolders += [string] $folder
    }
} elseif ($Prune) {
    Write-Host "No target ownership manifest at $ownershipPath; no folders will be pruned." -ForegroundColor Yellow
}

if (-not (Test-Path -LiteralPath $target -PathType Container)) {
    foreach ($legacyTarget in $legacyTargets) {
        if (-not (Test-Path -LiteralPath $legacyTarget -PathType Container)) {
            continue
        }
        if ($PSCmdlet.ShouldProcess($legacyTarget, "migrate to $target")) {
            # Split-Path -LiteralPath has no -Parent (it is the only member of
            # LiteralPathSet), so the two cannot bind together. Use the .NET call
            # to keep the literal, wildcard-free semantics the rest of this uses.
            $targetParent = [IO.Path]::GetDirectoryName($target)
            if (-not (Test-Path -LiteralPath $targetParent -PathType Container)) {
                New-Item -ItemType Directory -Path $targetParent -Force | Out-Null
            }
            Move-Item -LiteralPath $legacyTarget -Destination $target
            Write-Host "migrated $legacyTarget -> $target"
        }
        break
    }
}

if (-not (Test-Path -LiteralPath $target -PathType Container)) {
    if ($PSCmdlet.ShouldProcess($target, 'create target directory')) {
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        Write-Host "created $target"
    }
}

$copied = 0
$skipped = 0
$planned = 0
foreach ($filePlan in $filePlans) {
    $destination = Join-Path $target $filePlan.Destination
    $destinationDirectory = Split-Path $destination -Parent
    $existing = Get-Item -LiteralPath $destination -ErrorAction SilentlyContinue
    $same = $existing -and
            -not $existing.PSIsContainer -and
            ($existing.Length -eq (Get-Item -LiteralPath $filePlan.Source).Length) -and
            ($existing.LastWriteTimeUtc -ge (Get-Item -LiteralPath $filePlan.Source).LastWriteTimeUtc)

    if ($same) {
        $skipped++
        continue
    }

    if ($PSCmdlet.ShouldProcess($filePlan.Destination, 'copy')) {
        if (-not (Test-Path -LiteralPath $destinationDirectory -PathType Container)) {
            New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $filePlan.Source -Destination $destination -Force
        $copied++
    } else {
        $planned++
    }
}

$pruned = 0
if ($Prune) {
    $staleFolders = @($previousFolders | Where-Object { -not $activeFolderKeys.ContainsKey($_.ToLowerInvariant()) })
    foreach ($folder in $staleFolders) {
        $stalePath = Join-Path $target $folder
        if (-not (Test-Path -LiteralPath $stalePath)) {
            continue
        }
        if ($PSCmdlet.ShouldProcess($folder, 'delete previously deployed folder')) {
            Remove-Item -LiteralPath $stalePath -Recurse -Force
            $pruned++
        } else {
            $planned++
        }
    }
}

$ownership = [ordered]@{
    version = 1
    folders = $activeFolders
}
$ownershipJson = $ownership | ConvertTo-Json -Depth 3
$temporaryOwnershipPath = "$ownershipPath.$PID.tmp"
if ($PSCmdlet.ShouldProcess($ownershipPath, 'write target ownership manifest')) {
    $ownershipJson | Set-Content -LiteralPath $temporaryOwnershipPath -Encoding UTF8
    Move-Item -LiteralPath $temporaryOwnershipPath -Destination $ownershipPath -Force
}

Write-Host ''
Write-Host ("{0} file(s) copied, {1} already current, {2} change(s) planned, {3} folder(s) pruned -> {4}" -f $copied, $skipped, $planned, $pruned, $target)
if ($copied -gt 0) {
    Write-Host 'Press Rescan in the Tapioca palette to pick them up.' -ForegroundColor Cyan
}
