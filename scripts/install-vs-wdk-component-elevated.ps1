[CmdletBinding()]
param(
    [string] $InstallPath
)

$ErrorActionPreference = 'Stop'

$programFilesX86 = ${env:ProgramFiles(x86)}
if (-not $programFilesX86) {
    throw 'ProgramFiles(x86) environment variable is not set.'
}

$installerRoot = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer'
$installer = Join-Path $installerRoot 'vs_installer.exe'
$vswhere = Join-Path $installerRoot 'vswhere.exe'

if (-not $InstallPath) {
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "vswhere.exe not found: $vswhere"
    }

    $InstallPath = & $vswhere `
        -latest `
        -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath

    if (-not $InstallPath) {
        throw 'No compatible Visual Studio installation was found.'
    }
}

$arguments = @(
    'modify',
    '--installPath',
    $InstallPath,
    '--add',
    'Component.Microsoft.Windows.DriverKit',
    '--add',
    'Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64.Spectre',
    '--passive',
    '--norestart'
)

if (-not (Test-Path -LiteralPath $installer)) {
    throw "Visual Studio installer not found: $installer"
}

$process = Start-Process `
    -FilePath $installer `
    -ArgumentList $arguments `
    -Verb RunAs `
    -Wait `
    -PassThru

if ($process.ExitCode -ne 0) {
    throw "Visual Studio installer failed with exit code $($process.ExitCode)"
}
