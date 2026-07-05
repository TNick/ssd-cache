<#
.SYNOPSIS
    Configures, builds and tests the project through CMake presets inside a
    Visual Studio x64 developer environment.

.DESCRIPTION
    This is the primary end-to-end build entry point. It runs the full
    configure -> build -> test cycle for a given CMake preset:

      1. Verify the Visual Studio developer command script exists.
      2. Generate a temporary .cmd batch file that, in one cmd.exe process:
           a. Initializes the x64 native VS developer environment.
           b. Changes into the repository root.
           c. Runs `cmake --preset` to configure the build tree.
           d. Runs `cmake --build --preset` to compile.
           e. Runs `ctest --preset` to execute the test suite.
      3. Execute the batch file and throw on any non-zero exit code.

    As with the other driver-related scripts here, the compile steps are wrapped
    in a single batch file so they inherit the environment variables that
    VsDevCmd.bat sets (these do not survive across separate processes).

    This script is also reused by package.ps1, which calls it to produce the
    build tree before packaging.

.PARAMETER Preset
    Name of the CMake configure/build/test preset to use (as defined in
    CMakePresets.json). Defaults to 'windows-driver-debug'.

.NOTES
    Requires CMake on PATH and a Visual Studio installation at the hard-coded
    E:\installed\vs location.
#>
[CmdletBinding()]
param(
    # CMake preset name driving configure, build and test.
    [string] $Preset = 'windows-driver-debug'
)

# Stop the script at the first unhandled error.
$ErrorActionPreference = 'Stop'

# Visual Studio developer command initializer; `call`ed to set up the toolchain.
$vsDevCmd = 'E:\installed\vs\Common7\Tools\VsDevCmd.bat'
# Fail early with a clear message if the VS environment script is missing.
if (-not (Test-Path -LiteralPath $vsDevCmd)) {
    throw "Visual Studio developer command script not found: $vsDevCmd"
}

# Repository root (parent of the scripts\ directory).
$repo = Split-Path -Parent $PSScriptRoot

# Batch script run inside one cmd.exe so the VS env persists across the steps.
# Each step is guarded so a failure aborts the batch and reports its exit code:
#   call VsDevCmd     - load the x64 native developer environment
#   cd /d "$repo"     - move into the repo root (across drives with /d)
#   cmake --preset    - configure the CMake build tree
#   cmake --build     - compile the configured tree
#   ctest --preset    - run the test suite
$command = @"
call "$vsDevCmd" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
cd /d "$repo"
if errorlevel 1 exit /b %errorlevel%
cmake --preset $Preset
if errorlevel 1 exit /b %errorlevel%
cmake --build --preset $Preset
if errorlevel 1 exit /b %errorlevel%
ctest --preset $Preset
if errorlevel 1 exit /b %errorlevel%
"@

# Temporary batch file path the command block is written to.
$script = Join-Path $env:TEMP 'ssd-cache-build.cmd'

# Write the batch file as ASCII (no BOM) for cmd.exe.
Set-Content -LiteralPath $script -Value $command -Encoding ASCII

# Execute the batch file: /d no AutoRun, /s outer-quote handling, /c run + exit.
cmd.exe /d /s /c "`"$script`""

# Propagate any failure from the build/test run.
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}
