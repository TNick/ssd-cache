[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$service = Get-Service -Name 'SsdCacheService' -ErrorAction SilentlyContinue
if ($null -eq $service) {
    Write-Output 'SsdCacheService: missing'
    exit 2
}

Write-Output ("SsdCacheService: {0}" -f $service.Status.ToString().ToLowerInvariant())
if ($service.Status -eq [System.ServiceProcess.ServiceControllerStatus]::Running) {
    exit 0
}

exit 1
