<#
.SYNOPSIS
    Adds the Windows Driver Kit (WDK) and Spectre-mitigated VC toolset
    components to an existing Visual Studio installation, silently.

.DESCRIPTION
    Ensures the machine has the two components the driver build needs:
        * Component.Microsoft.Windows.DriverKit  - the WDK build integration.
        * Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64.Spectre
          - the Spectre-mitigated x86/x64 C++ runtime libraries (required
            because the driver is compiled with /Qspectre).

    This is the non-elevating variant: it invokes vs_installer.exe directly in
    `--quiet` mode. It assumes the current shell is already running elevated
    (administrator). If you are not elevated, use the companion
    install-vs-wdk-component-elevated.ps1, which self-elevates via UAC.

    Steps performed:
      1. Locate the Visual Studio installer directory under Program Files (x86).
      2. If no InstallPath was supplied, use vswhere to find the latest VS that
         already has the base x86/x64 VC tools installed.
      3. Run vs_installer.exe to add the two components silently, then check the
         resulting exit code.

.PARAMETER InstallPath
    Root path of the target Visual Studio installation to modify. When omitted,
    it is auto-detected with vswhere (latest install carrying the VC x86/x64
    tools).

.NOTES
    Requires an elevated shell. For a version that prompts for elevation and
    shows a progress UI, use install-vs-wdk-component-elevated.ps1.
#>
[CmdletBinding()]
param(
    # Target VS installation root; auto-detected via vswhere when not given.
    [string] $InstallPath
)

# Stop on the first unhandled error.
$ErrorActionPreference = 'Stop'

# The 32-bit Program Files root where the VS installer always lives.
$programFilesX86 = ${env:ProgramFiles(x86)}
if (-not $programFilesX86) {
    throw 'ProgramFiles(x86) environment variable is not set.'
}

# Directory containing both vs_installer.exe and vswhere.exe.
$installerRoot = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer'
# The Visual Studio installer executable used to modify installations.
$installer = Join-Path $installerRoot 'vs_installer.exe'
# vswhere: locates installed VS instances and their properties.
$vswhere = Join-Path $installerRoot 'vswhere.exe'

# Validate the installer is present up front.
if (-not (Test-Path -LiteralPath $installer)) {
    throw "Visual Studio installer not found: $installer"
}

# If the caller didn't specify which VS to modify, discover it.
if (-not $InstallPath) {
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "vswhere.exe not found: $vswhere"
    }

    # Find the newest install that already has the base x86/x64 VC tools:
    #   -latest        pick the most recent version
    #   -products *    consider all editions (Community/Pro/Enterprise/BuildTools)
    #   -requires ...  must already have the VC x86/x64 tools workload component
    #   -property installationPath  return just the install root path
    $InstallPath = & $vswhere `
        -latest `
        -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath

    if (-not $InstallPath) {
        throw 'No compatible Visual Studio installation was found.'
    }
}

# Modify the installation in place, silently:
#   modify            change an existing installation
#   --installPath     which installation to change
#   --add <id>        each component to add (WDK + Spectre-mitigated VC libs)
#   --quiet           no UI and no interaction
#   --norestart       don't reboot automatically if a restart is needed
& $installer modify `
    --installPath $InstallPath `
    --add Component.Microsoft.Windows.DriverKit `
    --add Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64.Spectre `
    --quiet `
    --norestart

# Non-zero exit code means the component install failed.
if ($LASTEXITCODE -ne 0) {
    throw "Visual Studio installer failed with exit code $LASTEXITCODE"
}
