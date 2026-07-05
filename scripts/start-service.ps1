[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$binary = Join-Path $PSScriptRoot '..\build\windows-driver-debug\Debug\ssd-cache-service.exe'
& $binary --install
if ($LASTEXITCODE -ne 0) {
    throw 'Failed to install SSD Cache service.'
}

& $binary --start
if ($LASTEXITCODE -ne 0) {
    throw 'Failed to start SSD Cache service.'
}
