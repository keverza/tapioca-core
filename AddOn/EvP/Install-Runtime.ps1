# Development-tree forwarding shim for the release runtime installer.
# The canonical implementation lives in core\dist so it can ship standalone.
#
# The parameters mirror dist\Install-Runtime.ps1 so existing development commands
# and EvP-Environment.ps1 continue to work unchanged.
#requires -Version 5.1
[CmdletBinding()]
param(
    [string]   $Version    = '3.12.10',
    [string]   $Target     = (Join-Path $env:LOCALAPPDATA 'Tapioca\runtime'),
    [string[]] $Baseline   = @('numpy==2.0.2', 'pillow', 'requests', 'pydantic>=2.7,<3'),
    [switch]   $NoBaseline,
    [switch]   $Force
)

$ErrorActionPreference = 'Stop'
$installer = Join-Path $PSScriptRoot '..\..\dist\Install-Runtime.ps1'
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
    throw "Distribution runtime installer was not found: $installer"
}

& $installer @PSBoundParameters
