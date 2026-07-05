[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $DriverBuildDir
)

$ErrorActionPreference = 'Stop'

$infPath = Join-Path $DriverBuildDir 'cachemon.inf'
if (-not (Test-Path -LiteralPath $infPath)) {
    throw "Driver INF not found: $infPath"
}

pnputil.exe /add-driver $infPath /install
