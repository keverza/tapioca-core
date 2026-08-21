#requires -Version 5.1

<##
.SYNOPSIS
    Provision the ignored AddOn/reference tree from a catalog-listed source tree or upstream archives.

.DESCRIPTION
    CATALOG.yaml is the source of truth for reference paths, profiles, and provenance.
    The provisioner copies only paths listed in that catalog and refuses an unlisted
    top-level source entry. It never deletes destination content, so a source profile
    can be provisioned incrementally without pruning another profile.

    The repository intentionally keeps the large reference tree out of Git. The normal
    source is a complete reference folder copied from another development machine:

        .\tools\build\provision-reference.ps1 -SourceRoot \\OLDPC\share\reference

    A public checkout can instead fetch the build or test profile from the upstream
    URLs recorded in CATALOG.yaml:

        .\tools\build\provision-reference.ps1 -FromUpstream

    PyYAML is used only to read the catalog because Windows PowerShell 5.1 has no native
    YAML reader. It is already a development requirement.

.PARAMETER SourceRoot
    Existing reference folder to copy from. Required unless -ValidateOnly is used.

.PARAMETER FromUpstream
    Download the selected profile from catalog-recorded upstream archives. When Profile
    is omitted, the build profile is selected so a clean public checkout is buildable.

.PARAMETER DestinationRoot
    Target reference folder. Defaults to AddOn/reference in this checkout.

.PARAMETER CatalogPath
    Catalog to read. Defaults to AddOn/reference/CATALOG.yaml in this checkout.

.PARAMETER Profile
    Catalog profile to provision. 'all' copies every entry; otherwise the profile must
    be declared by CATALOG.yaml.

.PARAMETER ValidateOnly
    Validate the catalog and source inventory without copying. If SourceRoot is omitted,
    the destination tree is checked.

.EXAMPLE
    .\tools\build\provision-reference.ps1 -SourceRoot D:\reference -Profile build

.EXAMPLE
    .\tools\build\provision-reference.ps1 -ValidateOnly

.EXAMPLE
    .\tools\build\provision-reference.ps1 -SourceRoot D:\reference -WhatIf
##>
[CmdletBinding(SupportsShouldProcess)]
param (
    [string] $SourceRoot,
    [string] $DestinationRoot,
    [string] $CatalogPath,
    [string] $Profile = "",
    [switch] $ValidateOnly,
    [switch] $FromUpstream
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ([string]::IsNullOrWhiteSpace($DestinationRoot)) {
    $DestinationRoot = Join-Path $repoRoot "AddOn\reference"
}
if ([string]::IsNullOrWhiteSpace($CatalogPath)) {
    $catalogCandidates = @(
        (Join-Path $repoRoot "AddOn\reference\CATALOG.yaml"),
        (Join-Path $repoRoot "tools\build\reference-catalog.yaml")
    )
    $CatalogPath = $catalogCandidates |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($CatalogPath)) {
        $CatalogPath = $catalogCandidates[0]
    }
}

function Get-AbsolutePath {
    param ([Parameter(Mandatory)][string] $Path)

    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function ConvertTo-Array {
    param ($Value)

    if ($null -eq $Value) {
        return @()
    }
    if ($Value -is [System.Array]) {
        return @($Value)
    }
    return @($Value)
}

function Read-Catalog {
    param ([Parameter(Mandatory)][string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Reference catalog was not found: $Path"
    }

    $python = Get-Command -Name "python.exe" -ErrorAction SilentlyContinue
    if ($null -eq $python) {
        throw "Python is required to read $Path. Install the development requirements first."
    }

    $yamlToJson = @'
import json
import sys

try:
    import yaml
except ImportError as error:
    raise SystemExit('PyYAML is required to read CATALOG.yaml: ' + str(error))

with open(sys.argv[1], 'r', encoding='utf-8') as stream:
    value = yaml.safe_load(stream)

if not isinstance(value, dict):
    raise SystemExit('CATALOG.yaml must contain a mapping at its root')

print(json.dumps(value))
'@

    $json = & $python.Source -c $yamlToJson $Path 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw ("Could not parse reference catalog: " + $json.Trim())
    }

    try {
        return ($json | ConvertFrom-Json -ErrorAction Stop)
    } catch {
        throw ("Could not decode catalog JSON: " + $_.Exception.Message)
    }
}

function Assert-CatalogPath {
    param ([Parameter(Mandatory)][string] $Path)

    if ([IO.Path]::IsPathRooted($Path)) {
        throw "Catalog entry path must be relative: $Path"
    }

    $segments = @($Path.Replace("\", "/").Split("/", [System.StringSplitOptions]::RemoveEmptyEntries))
    if ($segments.Count -eq 0 -or $segments -contains "..") {
        throw "Catalog entry path must not be empty or escape the reference root: $Path"
    }
}

function Get-CatalogEntryPath {
    param (
        [Parameter(Mandatory)][string] $Root,
        [Parameter(Mandatory)][string] $RelativePath
    )

    return Join-Path $Root ($RelativePath.Replace("/", "\"))
}

function Assert-Catalog {
    param ([Parameter(Mandatory)] $Catalog)

    if ($Catalog.catalog_version -ne 1) {
        throw "Unsupported reference catalog version: $($Catalog.catalog_version)"
    }
    if ([string]::IsNullOrWhiteSpace([string] $Catalog.reference_root)) {
        throw "CATALOG.yaml must declare reference_root"
    }
    if ($null -eq $Catalog.entries) {
        throw "CATALOG.yaml must declare entries"
    }

    $profileNames = @()
    if ($null -ne $Catalog.profiles) {
        $profileNames = @($Catalog.profiles.PSObject.Properties.Name)
    }
    if ($profileNames.Count -eq 0) {
        throw "CATALOG.yaml must declare at least one provisioning profile"
    }

    $seenPaths = @{}
    foreach ($entry in (ConvertTo-Array $Catalog.entries)) {
        foreach ($field in @("path", "kind", "profiles", "authority", "version", "source", "provenance")) {
            if ($null -eq $entry.PSObject.Properties[$field] -or
                [string]::IsNullOrWhiteSpace([string] $entry.$field)) {
                throw "Catalog entry is missing a non-empty '$field' field"
            }
        }

        $entryPath = ([string] $entry.path).Replace("\", "/")
        Assert-CatalogPath $entryPath
        if ($entryPath -eq "CATALOG.yaml") {
            throw "CATALOG.yaml must not list itself as a provisioned entry"
        }
        if ($seenPaths.ContainsKey($entryPath)) {
            throw "Duplicate catalog entry path: $entryPath"
        }
        $seenPaths[$entryPath] = $true

        if (@("directory", "file") -notcontains [string] $entry.kind) {
            throw "Catalog entry '$entryPath' has invalid kind '$($entry.kind)'"
        }

        $entryProfiles = @(ConvertTo-Array $entry.profiles)
        if ($entryProfiles.Count -eq 0) {
            throw "Catalog entry '$entryPath' must declare at least one profile"
        }
        foreach ($entryProfile in $entryProfiles) {
            if ($profileNames -notcontains [string] $entryProfile) {
                throw "Catalog entry '$entryPath' refers to unknown profile '$entryProfile'"
            }
        }
    }

    return $seenPaths
}

function Assert-Inventory {
    param (
        [Parameter(Mandatory)][string] $Root,
        [Parameter(Mandatory)][hashtable] $CatalogPaths
    )

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Reference source directory was not found: $Root"
    }

    $topLevelPaths = @{}
    foreach ($catalogPath in $CatalogPaths.Keys) {
        $topLevel = $catalogPath.Split("/")[0]
        $topLevelPaths[$topLevel] = $true
    }

    foreach ($child in (Get-ChildItem -LiteralPath $Root -Force)) {
        if ($child.Name -eq "CATALOG.yaml") {
            continue
        }
        if (-not $topLevelPaths.ContainsKey($child.Name)) {
            throw "Uncatalogued reference entry in '$Root': $($child.Name)"
        }
    }
}

function Get-DownloadUrl {
    param ([Parameter(Mandatory)] $Download)

    $provider = if ($null -ne $Download.PSObject.Properties['provider']) {
        [string] $Download.provider
    } else {
        "url"
    }
    if ($provider -eq "github_release") {
        $repository = [string] $Download.repository
        $pattern = [string] $Download.asset_pattern
        if ([string]::IsNullOrWhiteSpace($repository) -or [string]::IsNullOrWhiteSpace($pattern)) {
            throw "A github_release download needs repository and asset_pattern"
        }

        $tag = if ($null -ne $Download.PSObject.Properties['tag']) {
            [string] $Download.tag
        } else {
            ""
        }
        $headers = @{ "User-Agent" = "tapioca-reference-provisioner" }
        if (-not [string]::IsNullOrWhiteSpace($tag)) {
            $releaseUrl = "https://api.github.com/repos/$repository/releases/tags/$tag"
            try {
                $release = Invoke-RestMethod -Uri $releaseUrl -Headers $headers -UseBasicParsing
            } catch {
                throw "Could not query GitHub release '$repository@$tag`: $($_.Exception.Message)"
            }
            if ($release.draft -or $release.prerelease) {
                throw "GitHub release '$repository@$tag' is a draft or prerelease"
            }
            foreach ($asset in @($release.assets)) {
                if ([string] $asset.name -match $pattern) {
                    return [string] $asset.browser_download_url
                }
            }
            throw "No GitHub release asset in $repository@$tag matched '$pattern'"
        }

        $apiUrl = "https://api.github.com/repos/$repository/releases?per_page=100"
        try {
            $releases = @(Invoke-RestMethod -Uri $apiUrl -Headers $headers -UseBasicParsing)
        } catch {
            throw "Could not query GitHub releases for $repository`: $($_.Exception.Message)"
        }
        foreach ($release in ($releases | Where-Object { -not $_.draft -and -not $_.prerelease } |
            Sort-Object -Property published_at -Descending)) {
            foreach ($asset in @($release.assets)) {
                if ([string] $asset.name -match $pattern) {
                    return [string] $asset.browser_download_url
                }
            }
        }
        throw "No GitHub release asset in $repository matched '$pattern'"
    }

    $url = if ($null -ne $Download.PSObject.Properties['url']) { [string] $Download.url } else { "" }
    if ([string]::IsNullOrWhiteSpace($url)) {
        throw "Catalog entry has no upstream URL"
    }
    return $url
}

function Get-ArchiveSourceRoot {
    param (
        [Parameter(Mandatory)][string] $ExtractRoot,
        [Parameter(Mandatory)] $Download
    )

    $subpath = if ($null -ne $Download.PSObject.Properties['archive_subpath']) {
        [string] $Download.archive_subpath
    } else {
        ""
    }
    if (-not [string]::IsNullOrWhiteSpace($subpath)) {
        $source = Join-Path $ExtractRoot ($subpath.Replace("/", "\"))
        if (-not (Test-Path -LiteralPath $source)) {
            throw "Downloaded archive does not contain '$subpath'"
        }
        return $source
    }

    # The Graphisoft kit has Support at the archive root; GitHub source archives
    # normally wrap their contents in one directory. Handle both without encoding
    # archive-specific names in the provisioner.
    if (Test-Path -LiteralPath (Join-Path $ExtractRoot "Support") -PathType Container) {
        return $ExtractRoot
    }
    $children = @(Get-ChildItem -LiteralPath $ExtractRoot -Directory -Force)
    if ($children.Count -eq 1) {
        return $children[0].FullName
    }
    return $ExtractRoot
}

function Get-CacheStem {
    param ([Parameter(Mandatory)][string] $EntryPath)

    return [regex]::Replace($EntryPath, "[^A-Za-z0-9._-]", "_")
}

function Copy-DirectoryContents {
    param (
        [Parameter(Mandatory)][string] $SourceRoot,
        [Parameter(Mandatory)][string] $DestinationRoot
    )

    foreach ($child in (Get-ChildItem -LiteralPath $SourceRoot -Force)) {
        if ($child.Name -eq ".git") {
            continue
        }
        Copy-Item -LiteralPath $child.FullName -Destination $DestinationRoot -Recurse -Force
    }
}

function Read-TextFileOrEmpty {
    param ([Parameter(Mandatory)][string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ""
    }
    $text = [string] (Get-Content -LiteralPath $Path -Raw)
    if ($null -eq $text) {
        return ""
    }
    return $text
}

function Invoke-GitCommand {
    param (
        [Parameter(Mandatory)][string] $GitPath,
        [Parameter(Mandatory)][string[]] $Arguments
    )

    # Git reports ordinary progress ("Cloning into '...'", submodule paths) on stderr,
    # not just failures. Calling it as `& git ... 2> file` makes Windows PowerShell 5.1
    # wrap every one of those lines in a NativeCommandError ErrorRecord, which the
    # script-scope $ErrorActionPreference = "Stop" then turns into a terminating error --
    # so a perfectly healthy clone aborts the provisioner. Start-Process hands both
    # streams straight to files without PowerShell interpreting either one, leaving the
    # exit code as the only success signal, which is what the callers already check.
    $stdoutPath = [IO.Path]::GetTempFileName()
    $stderrPath = [IO.Path]::GetTempFileName()
    try {
        $process = Start-Process -FilePath $GitPath -ArgumentList $Arguments `
            -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
        $stdout = Read-TextFileOrEmpty $stdoutPath
        $stderr = Read-TextFileOrEmpty $stderrPath
        return [pscustomobject]@{
            ExitCode = [int] $process.ExitCode
            Output = @($stdout -split "`r`n|`n" | Where-Object { $_ -ne "" })
            Error = $stderr.Trim()
        }
    } finally {
        Remove-Item -LiteralPath $stdoutPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
    }
}

function Copy-GitEntry {
    param (
        [Parameter(Mandatory)] $Entry,
        [Parameter(Mandatory)][string] $CacheRoot,
        [Parameter(Mandatory)][string] $DestinationRoot
    )

    if ($Entry.kind -ne "directory") {
        throw "Git clone catalog entry '$($Entry.path)' must declare kind 'directory'"
    }
    if ($null -eq $Entry.PSObject.Properties['download']) {
        throw "Catalog entry '$($Entry.path)' has no download metadata for -FromUpstream"
    }

    $download = $Entry.download
    $url = if ($null -ne $download.PSObject.Properties['url']) { [string] $download.url } else { "" }
    $ref = if ($null -ne $download.PSObject.Properties['ref']) { [string] $download.ref } else { "" }
    if ([string]::IsNullOrWhiteSpace($url)) {
        throw "Git clone entry '$($Entry.path)' has no repository URL"
    }
    $git = Get-Command -Name "git.exe" -ErrorAction SilentlyContinue
    if ($null -eq $git) {
        throw "Git is required to provision '$($Entry.path)' recursively from $url"
    }

    $cloneRoot = Join-Path $CacheRoot ("git_" + (Get-CacheStem $Entry.path))
    $cloneExists = Test-Path -LiteralPath (Join-Path $cloneRoot ".git")
    if (-not $cloneExists) {
        if ($WhatIfPreference) {
            Write-Host "What if: would clone $url$(if ($ref) { "@$ref" }) with submodules for $($Entry.path)."
            return $false
        }
        if (Test-Path -LiteralPath $cloneRoot) {
            Remove-Item -LiteralPath $cloneRoot -Recurse -Force
        }
        if (-not (Test-Path -LiteralPath $CacheRoot -PathType Container)) {
            New-Item -ItemType Directory -Path $CacheRoot -Force | Out-Null
        }
        $cloneArguments = @("clone", "--depth", "1")
        if ($null -ne $download.PSObject.Properties['recursive'] -and $download.recursive) {
            $cloneArguments += "--recurse-submodules"
        }
        if (-not [string]::IsNullOrWhiteSpace($ref)) {
            $cloneArguments += @("--branch", $ref)
        }
        $cloneArguments += @($url, $cloneRoot)
        $cloneResult = Invoke-GitCommand -GitPath $git.Source -Arguments $cloneArguments
        if ($cloneResult.ExitCode -ne 0) {
            $details = @($cloneResult.Output + $cloneResult.Error) -join " "
            throw "Could not clone '$url' for '$($Entry.path)': $details"
        }
    } elseif ($null -ne $download.PSObject.Properties['recursive'] -and $download.recursive) {
        $submoduleResult = Invoke-GitCommand -GitPath $git.Source -Arguments @(
            "-C", $cloneRoot, "submodule", "update", "--init", "--recursive"
        )
        if ($submoduleResult.ExitCode -ne 0) {
            $details = @($submoduleResult.Output + $submoduleResult.Error) -join " "
            throw "Could not initialize submodules for '$($Entry.path)': $details"
        }
    }

    $relativePath = ([string] $Entry.path).Replace("/", "\")
    $destinationPath = Get-CatalogEntryPath $DestinationRoot $relativePath
    if (-not $PSCmdlet.ShouldProcess($destinationPath, "install cloned reference '$($Entry.path)'")) {
        return $false
    }
    if (-not (Test-Path -LiteralPath $destinationPath -PathType Container)) {
        New-Item -ItemType Directory -Path $destinationPath -Force | Out-Null
    }
    Copy-DirectoryContents -SourceRoot $cloneRoot -DestinationRoot $destinationPath
    return $true
}

function Copy-UpstreamEntry {
    param (
        [Parameter(Mandatory)] $Entry,
        [Parameter(Mandatory)][string] $CacheRoot,
        [Parameter(Mandatory)][string] $DestinationRoot
    )

    if ($null -eq $Entry.PSObject.Properties['download']) {
        throw "Catalog entry '$($Entry.path)' has no download metadata for -FromUpstream"
    }
    $download = $Entry.download
    $provider = if ($null -ne $download.PSObject.Properties['provider']) {
        [string] $download.provider
    } else {
        "url"
    }
    if ($provider -eq "git_clone") {
        return Copy-GitEntry -Entry $Entry -CacheRoot $CacheRoot -DestinationRoot $DestinationRoot
    }
    if ($WhatIfPreference -and $null -ne $download.PSObject.Properties['provider'] -and
        [string] $download.provider -eq "github_release") {
        Write-Host "What if: would resolve and download the GitHub release asset for $($Entry.path)."
        return $false
    }
    $url = Get-DownloadUrl $download
    $uri = [Uri] $url
    $fileName = [IO.Path]::GetFileName($uri.AbsolutePath)
    if ([string]::IsNullOrWhiteSpace($fileName)) {
        $fileName = "$($Entry.path).zip"
    } elseif ([IO.Path]::GetExtension($fileName) -ne ".zip") {
        $fileName = "$fileName.zip"
    }
    $archivePath = Join-Path $CacheRoot ((Get-CacheStem $Entry.path) + "-" + $fileName)

    if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
        if (-not $PSCmdlet.ShouldProcess($url, "download upstream reference for $($Entry.path)")) {
            return $false
        }
        if (-not (Test-Path -LiteralPath $CacheRoot -PathType Container)) {
            New-Item -ItemType Directory -Path $CacheRoot -Force | Out-Null
        }
        Write-Host "downloading $url" -ForegroundColor Cyan
        Invoke-WebRequest -Uri $url -OutFile $archivePath -UseBasicParsing
    }

    if ($null -ne $download.PSObject.Properties['sha256'] -and
        -not [string]::IsNullOrWhiteSpace([string] $download.sha256)) {
        $actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne ([string] $download.sha256).ToLowerInvariant()) {
            throw "SHA-256 mismatch for '$($Entry.path)': expected $($download.sha256), got $actualHash"
        }
    }

    $extractRoot = Join-Path $CacheRoot "extract_$PID`_$([IO.Path]::GetFileNameWithoutExtension($fileName))"
    if (Test-Path -LiteralPath $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $extractRoot -Force | Out-Null
    try {
        Expand-Archive -LiteralPath $archivePath -DestinationPath $extractRoot -Force
        $sourceRoot = Get-ArchiveSourceRoot -ExtractRoot $extractRoot -Download $download
        $relativePath = ([string] $Entry.path).Replace("/", "\")
        $destinationPath = Get-CatalogEntryPath $DestinationRoot $relativePath
        if (-not $PSCmdlet.ShouldProcess($destinationPath, "install upstream reference '$($Entry.path)'")) {
            return $false
        }

        if ($Entry.kind -eq "directory") {
            if (-not (Test-Path -LiteralPath $destinationPath -PathType Container)) {
                New-Item -ItemType Directory -Path $destinationPath -Force | Out-Null
            }
            Copy-DirectoryContents -SourceRoot $sourceRoot -DestinationRoot $destinationPath
        } else {
            $parent = Split-Path -Parent $destinationPath
            if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
                New-Item -ItemType Directory -Path $parent -Force | Out-Null
            }
            Copy-Item -LiteralPath $sourceRoot -Destination $destinationPath -Force
        }
        return $true
    } finally {
        if (Test-Path -LiteralPath $extractRoot) {
            Remove-Item -LiteralPath $extractRoot -Recurse -Force
        }
    }
}

$catalogPath = Get-AbsolutePath $CatalogPath
$destinationRoot = Get-AbsolutePath $DestinationRoot
$catalog = Read-Catalog $catalogPath
$catalogPaths = Assert-Catalog $catalog

if ($FromUpstream -and -not [string]::IsNullOrWhiteSpace($SourceRoot)) {
    throw "-SourceRoot and -FromUpstream are mutually exclusive"
}
if ([string]::IsNullOrWhiteSpace($Profile)) {
    $Profile = if ($FromUpstream) { "build" } else { "all" }
}
if ($FromUpstream -and $Profile -eq "all") {
    throw "-FromUpstream cannot provision profile 'all' because some research entries have no upstream archive. Use -Profile build or -Profile test, or use -SourceRoot for a complete reference bundle."
}

if ($FromUpstream -and -not $ValidateOnly) {
    $profileNames = @($catalog.profiles.PSObject.Properties.Name)
    if ($profileNames -notcontains $Profile) {
        throw "Unknown profile '$Profile'. Available profiles: $($profileNames -join ', ')"
    }
    $selectedEntries = @(
        foreach ($entry in (ConvertTo-Array $catalog.entries)) {
            $entryProfiles = @(ConvertTo-Array $entry.profiles)
            if ($entryProfiles -contains $Profile) { $entry }
        }
    )
    if ($selectedEntries.Count -eq 0) {
        throw "Profile '$Profile' selected no catalog entries"
    }
    $unsupportedEntries = @(
        $selectedEntries | Where-Object { $null -eq $_.PSObject.Properties['download'] }
    )
    if ($unsupportedEntries.Count -gt 0) {
        throw "Profile '$Profile' has no upstream download metadata for: $($unsupportedEntries.path -join ', '). Use -SourceRoot for these reference entries."
    }

    $cacheRoot = if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        Join-Path ([IO.Path]::GetTempPath()) "tapioca-reference-cache"
    } else {
        Join-Path $env:LOCALAPPDATA "Tapioca\_provision-cache"
    }
    if (-not (Test-Path -LiteralPath $destinationRoot -PathType Container) -and -not $WhatIfPreference) {
        New-Item -ItemType Directory -Path $destinationRoot -Force | Out-Null
    }
    $installed = 0
    $planned = 0
    foreach ($entry in $selectedEntries) {
        if (Copy-UpstreamEntry -Entry $entry -CacheRoot $cacheRoot -DestinationRoot $destinationRoot) {
            $installed++
        } else {
            $planned++
        }
    }
    $action = if ($WhatIfPreference) { "Planned" } else { "Provisioned" }
    Write-Output "$action $($selectedEntries.Count) upstream catalog entries using profile '$Profile' ($installed installed, $planned planned)."
    exit 0
}

if ($ValidateOnly -and [string]::IsNullOrWhiteSpace($SourceRoot)) {
    $SourceRoot = $destinationRoot
}
if (-not $ValidateOnly -and [string]::IsNullOrWhiteSpace($SourceRoot)) {
    throw "-SourceRoot is required when provisioning. Use -ValidateOnly to check the current destination."
}

$sourceRoot = Get-AbsolutePath $SourceRoot
Assert-Inventory $sourceRoot $catalogPaths

$profileNames = @($catalog.profiles.PSObject.Properties.Name)
if ($Profile -ne "all" -and $profileNames -notcontains $Profile) {
    throw "Unknown profile '$Profile'. Available profiles: $($profileNames -join ', ')"
}

if (-not $ValidateOnly -and
    [string]::Equals($sourceRoot.TrimEnd("\"), $destinationRoot.TrimEnd("\"),
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "SourceRoot and DestinationRoot are the same; refusing to copy a reference tree onto itself."
}

$selectedEntries = @(
    foreach ($entry in (ConvertTo-Array $catalog.entries)) {
        $entryProfiles = @(ConvertTo-Array $entry.profiles)
        if ($Profile -eq "all" -or $entryProfiles -contains $Profile) {
            $entry
        }
    }
)

if ($selectedEntries.Count -eq 0) {
    throw "Profile '$Profile' selected no catalog entries"
}

$validated = 0
foreach ($entry in $selectedEntries) {
    $relativePath = ([string] $entry.path).Replace("/", "\")
    $sourcePath = Get-CatalogEntryPath $sourceRoot $relativePath
    if (-not (Test-Path -LiteralPath $sourcePath)) {
        throw "Catalog entry is missing from source '$sourceRoot': $($entry.path)"
    }

    $expectedType = if ($entry.kind -eq "directory") { "Container" } else { "Leaf" }
    if (-not (Test-Path -LiteralPath $sourcePath -PathType $expectedType)) {
        throw "Catalog entry '$($entry.path)' is not the declared kind '$($entry.kind)'"
    }
    $validated++

    if ($ValidateOnly) {
        continue
    }

    $destinationPath = Get-CatalogEntryPath $destinationRoot $relativePath
    if ($entry.kind -eq "directory") {
        if ($PSCmdlet.ShouldProcess($destinationPath, "Copy catalog directory from $sourcePath")) {
            if (-not (Test-Path -LiteralPath $destinationPath -PathType Container)) {
                New-Item -ItemType Directory -Path $destinationPath -Force | Out-Null
            }
            foreach ($child in (Get-ChildItem -LiteralPath $sourcePath -Force)) {
                Copy-Item -LiteralPath $child.FullName -Destination $destinationPath -Recurse -Force
            }
        }
    } else {
        if ($PSCmdlet.ShouldProcess($destinationPath, "Copy catalog file from $sourcePath")) {
            $parent = Split-Path -Parent $destinationPath
            if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
                New-Item -ItemType Directory -Path $parent -Force | Out-Null
            }
            Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
        }
    }
}

$action = if ($ValidateOnly) { "Validated" } elseif ($WhatIfPreference) { "Planned" } else { "Provisioned" }
Write-Output "$action $validated catalog entries from $sourceRoot using profile '$Profile'."
