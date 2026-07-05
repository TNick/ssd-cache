[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('app', 'service', 'driver')]
    [string] $Component
)

$ErrorActionPreference = 'Stop'

function Open-DriverDebugOutput {
    $candidates = @(
        'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\windbg.exe',
        'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\windbg.exe',
        'C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\windbg.exe'
    )

    $debugger = $candidates | Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    if (-not $debugger) {
        throw 'WinDbg was not found under the Windows Kits debugger directory.'
    }

    $startArgs = @{
        FilePath = $debugger
        ArgumentList = @(
            '-kl',
            '-c',
            'ed nt!Kd_IHVDRIVER_Mask 0xffffffff; !dbgprint'
        )
        Verb = 'RunAs'
    }
    Start-Process @startArgs
}

if ($Component -eq 'driver') {
    Open-DriverDebugOutput
    return
}

$logDir = Join-Path $env:ProgramData 'ssd-cache'
$logPath = Join-Path $logDir ($Component + '.log')

New-Item -ItemType Directory -Force $logDir | Out-Null
if (-not (Test-Path -LiteralPath $logPath)) {
    New-Item -ItemType File -Force $logPath | Out-Null
}

Start-Process notepad.exe $logPath
