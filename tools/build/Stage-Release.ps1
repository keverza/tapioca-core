# Stage the deployable AC29 layout and create the GitHub release zip.
# Run after Build-AddOn29.ps1, or independently against an existing build_29.
#requires -Version 5.1
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$build = Join-Path $repo "AddOn\EvP\build_29"
$dist = Join-Path $repo "dist"
$stage = Join-Path $dist "Tapioca"
$archive = Join-Path $dist "Tapioca-AC29.zip"

foreach ($required in @("Tapioca.apx", "EvPPy.dll")) {
    $path = Join-Path $build $required
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required release artifact was not found: $path"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $build "PyPackage") -PathType Container)) {
    throw "Required release directory was not found: $(Join-Path $build 'PyPackage')"
}

if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null

foreach ($name in @("Tapioca.apx", "EvPPy.dll")) {
    Copy-Item -LiteralPath (Join-Path $build $name) -Destination $stage -Force
}
foreach ($name in @("LICENSE", "NOTICE", "README.md")) {
    Copy-Item -LiteralPath (Join-Path $repo $name) -Destination $stage -Force
}
Copy-Item -LiteralPath (Join-Path $dist "Install-Runtime.ps1") -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $build "PyPackage") -Destination $stage -Recurse -Force

$optionalDirectories = @("GhWorker", "DynamoRunner", "DynamoPackage")
foreach ($name in $optionalDirectories) {
    $source = Join-Path $build $name
    if (Test-Path -LiteralPath $source -PathType Container) {
        Copy-Item -LiteralPath $source -Destination $stage -Recurse -Force
    } else {
        Write-Warning "Optional release directory was not built: $source"
    }
}

# Package metadata is source, not a compiler product. Taking it directly avoids
# carrying an old manifest when this script stages an otherwise-current build.
$dynamoSource = Join-Path $repo "AddOn\EvP\Sources\TapiocaDynamo\pkg.json"
$dynamoManifest = Join-Path $stage "DynamoPackage\Tapioca\pkg.json"
if (Test-Path -LiteralPath (Split-Path -Parent $dynamoManifest) -PathType Container) {
    Copy-Item -LiteralPath $dynamoSource -Destination $dynamoManifest -Force
}

# Debug symbols and Python caches are build products, not release payloads.
Get-ChildItem -LiteralPath $stage -Recurse -File -Filter "*.pdb" |
    Remove-Item -Force
Get-ChildItem -LiteralPath $stage -Recurse -Directory -Filter "__pycache__" |
    Remove-Item -Recurse -Force

if (Test-Path -LiteralPath $archive) {
    Remove-Item -LiteralPath $archive -Force
}
Compress-Archive -LiteralPath $stage -DestinationPath $archive -CompressionLevel Optimal

$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File)
$size = ($files | Measure-Object -Property Length -Sum).Sum
Write-Host ("Release staged: dist\Tapioca ({0} files, {1:N1} MB)" -f $files.Count, ($size / 1MB)) -ForegroundColor Green
Write-Host "Release archive: dist\Tapioca-AC29.zip" -ForegroundColor Green
