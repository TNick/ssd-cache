[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$binary = Join-Path $PSScriptRoot '..\build\windows-driver-debug\Debug\ssd-cache-service.exe'
& $binary --stop
if ($LASTEXITCODE -ne 0) {
    throw 'Failed to stop SSD Cache service.'
}
