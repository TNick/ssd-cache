<#
.SYNOPSIS
    Verifies that all tools and SDK paths required to build (and optionally
    package) the project are present.

.DESCRIPTION
    A preflight check that inspects the local machine for every prerequisite the
    build depends on and reports each as "found" or "missing". It is
    non-destructive: it only probes for commands and paths.

    Core checks (always run):
        * cmake on PATH
        * the Visual Studio developer prompt (VsDevCmd.bat)
        * the MSVC toolset directory
        * WDK kernel header  (fltKernel.h)
        * WDK kernel import library (FltMgr.lib)
        * WDK MSBuild targets directory
        * the VS WDK x64 platform toolset (WindowsKernelModeDriver10.0)

    Additional checks (only with -IncludePackaging):
        * dotnet on PATH
        * at least one installed .NET SDK
        * the WiX .NET tool (wix.exe)
        * the WixToolset.UI.wixext extension in the WiX global extension cache

    The script accumulates a single boolean across all checks and, if any check
    failed, exits with code 1 so it can gate CI or a setup script. On full
    success it exits 0.

.PARAMETER IncludePackaging
    Also verify the packaging prerequisites (dotnet SDK + WiX toolset/extension)
    needed by package.ps1. Off by default.

.OUTPUTS
    Writes a "found:"/"missing:" line per check to the host. Exit code 1 if any
    required item is missing, otherwise 0.
#>
[CmdletBinding()]
param(
    # When set, also check the MSI packaging toolchain (dotnet + WiX).
    [switch] $IncludePackaging
)

# Stop on the first unhandled error.
$ErrorActionPreference = 'Stop'

# Returns $true if a command is resolvable on PATH, reporting found/missing.
function Test-Command {
    param(
        # Name of the command/executable to look for.
        [Parameter(Mandatory = $true)]
        [string] $Name
    )

    # Resolve the command; $null when it isn't on PATH.
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        Write-Host "missing: $Name"
        return $false
    }

    # Report the resolved executable path for transparency.
    Write-Host "found: $Name -> $($command.Source)"
    return $true
}

# Returns $true if a required file/directory path exists, reporting found/missing.
function Test-PathRequired {
    param(
        # Filesystem path that must exist.
        [Parameter(Mandatory = $true)]
        [string] $Path,
        # Human-friendly label shown in the output.
        [Parameter(Mandatory = $true)]
        [string] $Label
    )

    if (Test-Path -LiteralPath $Path) {
        Write-Host "found: $Label -> $Path"
        return $true
    }

    Write-Host "missing: $Label -> $Path"
    return $false
}

# Returns $true if at least one .NET SDK is installed.
function Test-DotNetSdk {
    # Ask the dotnet CLI for the list of installed SDKs.
    $sdkList = dotnet --list-sdks
    if (-not $sdkList) {
        Write-Host 'missing: .NET SDK'
        return $false
    }

    # Report all discovered SDKs on one line.
    Write-Host "found: .NET SDK -> $($sdkList -join ', ')"
    return $true
}

# Returns $true if the WiX UI extension is registered in the global cache.
function Test-WixExtension {
    param(
        # Path to the WiX tool (wix.exe) used to query the extension cache.
        [Parameter(Mandatory = $true)]
        [string] $WixPath
    )

    # List the globally installed WiX extensions.
    $extensions = & $WixPath extension list --global
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'missing: WiX extension cache'
        return $false
    }

    # The UI extension is required by the MSI authoring.
    if ($extensions -notmatch 'WixToolset.UI.wixext') {
        Write-Host 'missing: WixToolset.UI.wixext'
        return $false
    }

    Write-Host 'found: WixToolset.UI.wixext'
    return $true
}

# Overall success flag; ANDed with each check so any failure turns it false.
$ok = $true

# --- Core build prerequisites ------------------------------------------------

# CMake must be available on PATH.
$ok = (Test-Command -Name cmake) -and $ok
# The Visual Studio developer prompt initializer used by the build scripts.
$ok = (Test-PathRequired `
    -Path 'E:\installed\vs\Common7\Tools\VsDevCmd.bat' `
    -Label 'Visual Studio developer prompt') -and $ok
# The MSVC compiler toolset directory.
$ok = (Test-PathRequired `
    -Path 'E:\installed\vs\VC\Tools\MSVC' `
    -Label 'MSVC toolset') -and $ok
# WDK kernel-mode minifilter header, proving the WDK headers are installed.
$ok = (Test-PathRequired `
    -Path 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\km\fltKernel.h' `
    -Label 'WDK fltKernel.h') -and $ok
# WDK x64 Filter Manager import library, proving the WDK libs are installed.
$ok = (Test-PathRequired `
    -Path 'C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\km\x64\FltMgr.lib' `
    -Label 'WDK FltMgr.lib') -and $ok
# WDK build/MSBuild targets directory used by the .vcxproj build path.
$ok = (Test-PathRequired `
    -Path 'C:\Program Files (x86)\Windows Kits\10\build\10.0.26100.0' `
    -Label 'WDK MSBuild targets') -and $ok
# The VS-side x64 kernel-mode driver platform toolset props file.
$ok = (Test-PathRequired `
    -Path 'E:\installed\vs\MSBuild\Microsoft\VC\v170\Platforms\x64\PlatformToolsets\WindowsKernelModeDriver10.0\Toolset.props' `
    -Label 'VS WDK x64 platform toolset') -and $ok

# --- Optional packaging prerequisites ---------------------------------------

if ($IncludePackaging) {
    # Expected location of the WiX .NET global tool.
    $wixTool = Join-Path $env:USERPROFILE '.dotnet\tools\wix.exe'
    # dotnet CLI must be available to host the WiX tool.
    $ok = (Test-Command -Name dotnet) -and $ok
    # At least one .NET SDK must be installed.
    $ok = (Test-DotNetSdk) -and $ok
    # The WiX tool itself must be present.
    $ok = (Test-PathRequired `
        -Path $wixTool `
        -Label 'WiX .NET tool') -and $ok

    # Only query the extension cache if the tool exists (avoids a spurious error).
    if (Test-Path -LiteralPath $wixTool) {
        $ok = (Test-WixExtension -WixPath $wixTool) -and $ok
    }
}

# Fail the process if any required check reported missing.
if (-not $ok) {
    exit 1
}
