[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$service = Get-Service -Name 'SsdCacheService' -ErrorAction SilentlyContinue
$serviceState = if ($null -eq $service) {
    'missing'
} else {
    $service.Status.ToString().ToLowerInvariant()
}

$driver = Get-Service -Name 'CacheMon' -ErrorAction SilentlyContinue
$driverScmState = if ($null -eq $driver) {
    'missing'
} else {
    $driver.Status.ToString().ToLowerInvariant()
}

$driverLoaded = $false
$filters = & fltmc.exe filters
if ($LASTEXITCODE -eq 0) {
    $driverLoaded = [bool]($filters | Select-String -Pattern '^\s*CacheMon\s+')
}

$driverState = if ($null -eq $driver) {
    'missing'
} elseif ($driverLoaded) {
    'running'
} else {
    $driverScmState
}

$health = [ordered]@{
    service = [ordered]@{
        name = 'SsdCacheService'
        state = $serviceState
        healthy = ($serviceState -eq 'running')
    }
    driver = [ordered]@{
        name = 'CacheMon'
        state = $driverState
        scm_state = $driverScmState
        filter_loaded = $driverLoaded
        healthy = ($driverState -eq 'running')
    }
    overall_healthy = ($serviceState -eq 'running' -and $driverState -eq 'running')
}

$health | ConvertTo-Json -Depth 4

if ($health.overall_healthy) {
    exit 0
}

exit 1
