<#
.SYNOPSIS
    Rebuilds the CacheMon kernel driver from its .vcxproj using MSBuild and the
    Visual Studio WDK platform toolset.

.DESCRIPTION
    The "normal" driver build path. Unlike build-driver-direct.ps1 (which drives
    cl.exe/link.exe by hand), this script builds the driver through its Visual
    Studio project file, relying on the installed
    WindowsKernelModeDriver10.0 platform toolset. It performs a full Rebuild
    (clean + build) so stale artifacts can't leak into the output.

    Steps performed:
      1. Resolve the repo root and the driver .vcxproj path.
      2. Generate a temporary .cmd batch file that, in one cmd.exe process:
           a. Initializes the x64 native VS developer environment.
           b. Runs MSBuild with the Rebuild target for the x64 platform.
      3. Execute the batch file and throw on any non-zero exit code.

    As elsewhere in this folder, the MSBuild call is wrapped in a single batch
    file so it inherits the environment set up by VsDevCmd.bat.

.PARAMETER Configuration
    MSBuild configuration to build ('Debug' or 'Release'). Defaults to 'Debug'.

.NOTES
    Requires the Visual Studio WDK platform toolset to be installed (see
    install-vs-wdk-component.ps1). If that integration is missing or broken,
    use build-driver-direct.ps1 as a fallback.
#>
[CmdletBinding()]
param(
    # MSBuild configuration to build (Debug/Release).
    [string] $Configuration = 'Debug'
)

# Stop on the first unhandled error.
$ErrorActionPreference = 'Stop'

# Visual Studio developer command initializer; `call`ed to set up the toolchain.
$vsDevCmd = 'E:\installed\vs\Common7\Tools\VsDevCmd.bat'

# Repository root (parent of the scripts\ directory).
$repo = Split-Path -Parent $PSScriptRoot

# The driver's Visual Studio project file that MSBuild will build.
$project = Join-Path $repo 'src\driver\cachemon_minifilter.vcxproj'

# Batch script run inside one cmd.exe so the VS env persists across the steps:
#   call VsDevCmd   - load the x64 native developer environment
#   MSBuild ...     - clean+build the driver project:
#       /t:Rebuild             full rebuild (clean then build)
#       /p:Configuration=...   selected configuration
#       /p:Platform=x64        target the x64 platform
$command = @"
call "$vsDevCmd" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
MSBuild "$project" /t:Rebuild /p:Configuration=$Configuration /p:Platform=x64
if errorlevel 1 exit /b %errorlevel%
"@

# Temporary batch file path the command block is written to.
$script = Join-Path $env:TEMP 'ssd-cache-rebuild-driver.cmd'

# Write the batch file as ASCII (no BOM) for cmd.exe.
Set-Content -LiteralPath $script -Value $command -Encoding ASCII

# Execute the batch file: /d no AutoRun, /s outer-quote handling, /c run + exit.
cmd.exe /d /s /c "`"$script`""

# Propagate any failure from the rebuild.
if ($LASTEXITCODE -ne 0) {
    throw "Driver rebuild failed with exit code $LASTEXITCODE"
}
