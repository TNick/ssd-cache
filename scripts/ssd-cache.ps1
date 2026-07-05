[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet(
        'install-service',
        'start-service',
        'stop-service',
        'service-state',
        'start-driver',
        'stop-driver',
        'driver-state',
        'health',
        'start-tray',
        'service-console',
        'open-config',
        'open-app-log',
        'open-service-log',
        'open-driver-log',
        'start-system',
        'stop-system'
    )]
    [string] $Command
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot 'build\windows-driver-debug\Debug'
$serviceExe = Join-Path $buildDir 'ssd-cache-service.exe'
$trayExe = Join-Path $buildDir 'ssd-cache-tray.exe'
$programDataDir = Join-Path $env:ProgramData 'ssd-cache'
$configPath = Join-Path $programDataDir 'config.ini'

function Invoke-ServiceExe {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Argument
    )

    & $serviceExe $Argument
    if ($LASTEXITCODE -ne 0) {
        throw "Service command failed: $Argument"
    }
}

function Open-TextFile {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    New-Item -ItemType Directory -Force (Split-Path -Parent $Path) | Out-Null
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType File -Force $Path | Out-Null
    }

    Start-Process notepad.exe $Path
}

switch ($Command) {
    'install-service' {
        Invoke-ServiceExe '--install'
        break
    }
    'start-service' {
        & (Join-Path $PSScriptRoot 'start-service.ps1')
        if ($LASTEXITCODE -ne 0) {
            throw 'Failed to start service.'
        }
        break
    }
    'stop-service' {
        & (Join-Path $PSScriptRoot 'stop-service.ps1')
        if ($LASTEXITCODE -ne 0) {
            throw 'Failed to stop service.'
        }
        break
    }
    'service-state' {
        & (Join-Path $PSScriptRoot 'get-service-state.ps1')
        exit $LASTEXITCODE
    }
    'start-driver' {
        & (Join-Path $PSScriptRoot 'start-driver.ps1')
        if ($LASTEXITCODE -ne 0) {
            throw 'Failed to start driver.'
        }
        break
    }
    'stop-driver' {
        & (Join-Path $PSScriptRoot 'stop-driver.ps1')
        if ($LASTEXITCODE -ne 0) {
            throw 'Failed to stop driver.'
        }
        break
    }
    'driver-state' {
        & (Join-Path $PSScriptRoot 'get-driver-state.ps1')
        exit $LASTEXITCODE
    }
    'health' {
        & (Join-Path $PSScriptRoot 'get-health.ps1')
        exit $LASTEXITCODE
    }
    'start-tray' {
        Start-Process $trayExe
        break
    }
    'service-console' {
        Invoke-ServiceExe '--console'
        break
    }
    'open-config' {
        Open-TextFile $configPath
        break
    }
    'open-app-log' {
        & (Join-Path $PSScriptRoot 'open-log.ps1') -Component app
        if ($LASTEXITCODE -ne 0) {
            throw 'Failed to open app log.'
        }
        break
    }
    'open-service-log' {
        & (Join-Path $PSScriptRoot 'open-log.ps1') -Component service
        if ($LASTEXITCODE -ne 0) {
            throw 'Failed to open service log.'
        }
        break
    }
    'open-driver-log' {
        & (Join-Path $PSScriptRoot 'open-log.ps1') -Component driver
        if ($LASTEXITCODE -ne 0) {
            throw 'Failed to open driver log.'
        }
        break
    }
    'start-system' {
        & (Join-Path $PSScriptRoot 'start-service.ps1')
        if ($LASTEXITCODE -ne 0) {
            throw 'Failed to start service.'
        }

        Start-Process $trayExe
        break
    }
    'stop-system' {
        & (Join-Path $PSScriptRoot 'stop-service.ps1')
        if ($LASTEXITCODE -ne 0) {
            throw 'Failed to stop service.'
        }
        break
    }
}
