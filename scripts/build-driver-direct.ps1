<#
.SYNOPSIS
    Builds the CacheMon kernel-mode minifilter driver by invoking the MSVC
    compiler and linker directly, bypassing MSBuild / the .vcxproj project.

.DESCRIPTION
    This is the "escape hatch" driver build. Instead of relying on the Visual
    Studio WindowsKernelModeDriver10.0 platform toolset (used by
    rebuild-driver.ps1), it drives cl.exe and link.exe by hand. That makes it
    resilient when the VS WDK MSBuild integration is missing or broken, and it
    keeps the exact compiler/linker flags visible and under our control.

    Steps performed:
      1. Resolve the repo root, output directory and WDK include/lib paths.
      2. Create the per-configuration output directory under build\driver.
      3. Generate a temporary .cmd batch file that:
           a. Enters an x64 native VS developer environment (VsDevCmd.bat).
           b. Compiles src\driver\src\driver.c into driver.obj with kernel-mode
              flags (/kernel, /GS, /Qspectre, warnings-as-errors, etc.).
           c. Links driver.obj against the kernel libraries (FltMgr, ntoskrnl,
              BufferOverflowK) into cachemon.sys with a matching .pdb.
      4. Run the batch file through cmd.exe and fail hard on any error.
      5. Copy the driver INF next to the built .sys so the pair can be installed.

    The heavy lifting is delegated to a batch file because VsDevCmd.bat sets up
    dozens of environment variables that only persist for the life of a single
    cmd.exe process; running the whole compile/link sequence inside that one
    process is the simplest way to inherit them.

.PARAMETER Configuration
    Build configuration name ('Debug' or 'Release'). Selects the output
    subdirectory and is otherwise informational for this hand-rolled build.
    Defaults to 'Debug'.

.OUTPUTS
    build\driver\<Configuration>\cachemon.sys  - the compiled driver binary.
    build\driver\<Configuration>\cachemon.pdb  - matching debug symbols.
    build\driver\<Configuration>\cachemon.inf  - copy of the install manifest.

.NOTES
    Requires the Windows Driver Kit (WDK) 10.0.26100.0 and a Visual Studio
    installation at the hard-coded E:\installed\vs location. Run from any
    (non-elevated) shell; installation of the built driver is a separate step
    (see install-driver.ps1).
#>
[CmdletBinding()]
param(
    # Build configuration; drives the output folder name (Debug/Release).
    [string] $Configuration = 'Debug'
)

# Abort the whole script on the first unhandled error rather than limping on.
$ErrorActionPreference = 'Stop'

# Full path to the Visual Studio developer command prompt initializer. Running
# this (via `call`) populates PATH/INCLUDE/LIB so cl.exe and link.exe resolve.
$vsDevCmd = 'E:\installed\vs\Common7\Tools\VsDevCmd.bat'

# Repository root: the parent of the scripts\ directory this file lives in.
$repo = Split-Path -Parent $PSScriptRoot

# Per-configuration output directory for all build artifacts (.obj/.sys/.pdb/.inf).
$outDir = Join-Path $repo "build\driver\$Configuration"

# Root of the installed Windows Kits, containing WDK headers and import libs.
$wdkRoot = 'C:\Program Files (x86)\Windows Kits\10'

# Specific WDK version whose headers/libs we compile and link against.
$wdkVersion = '10.0.26100.0'

# Ensure the output directory exists; -Force makes this idempotent (no error if
# it already exists). Out-Null suppresses the DirectoryInfo object it returns.
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# The single C translation unit that makes up the driver.
$driverSource = Join-Path $repo 'src\driver\src\driver.c'

# Project-local shared headers (types shared between driver and user mode).
$sharedInclude = Join-Path $repo 'src\shared'

# Intermediate object file produced by the compile step.
$objectPath = Join-Path $outDir 'driver.obj'

# Final linked kernel driver binary.
$sysPath = Join-Path $outDir 'cachemon.sys'

# Program database (debug symbols) emitted alongside the driver.
$pdbPath = Join-Path $outDir 'cachemon.pdb'

# Source INF (install manifest) checked into the driver source tree...
$infSource = Join-Path $repo 'src\driver\cachemon.inf'
# ...and its destination next to the freshly built .sys.
$infDestination = Join-Path $outDir 'cachemon.inf'

# WDK kernel-mode headers (fltKernel.h, ntddk.h, ...).
$kmInclude = Join-Path $wdkRoot "Include\$wdkVersion\km"

# WDK headers shared between kernel and user mode.
$sharedWdkInclude = Join-Path $wdkRoot "Include\$wdkVersion\shared"

# WDK x64 kernel-mode import libraries (FltMgr.lib, ntoskrnl.lib, ...).
$kmLib = Join-Path $wdkRoot "Lib\$wdkVersion\km\x64"

# Batch script executed inside a single cmd.exe so it inherits the VS dev env.
# Each step guards with `if errorlevel 1 exit /b` so failures propagate out.
#   cl flags:
#     /nologo           suppress the compiler banner
#     /c                compile only, do not link
#     /W4 /WX           highest warning level, treat warnings as errors
#     /wd4324           silence "structure was padded" (expected for alignment)
#     /Zi               generate full debug information
#     /O2               optimize for speed
#     /kernel           kernel-mode compilation (no CRT, restricted features)
#     /GS               buffer security checks (stack cookies)
#     /Qspectre         insert Spectre variant-1 mitigations
#     /D_AMD64_ /DAMD64 /D_WIN64   target the x64 kernel architecture
#     /DUNICODE /D_UNICODE         build against the wide-char Win32/NT APIs
#     /I...             add the shared and WDK include directories
#     /Fo...            output object path
#   link flags:
#     /driver           produce a kernel driver image
#     /subsystem:native native (no Win32 subsystem) driver
#     /machine:x64      target architecture
#     /entry:DriverEntry driver entry point symbol
#     /debug            emit debug info / reference the .pdb
#     /out /pdb         output paths for the .sys and its symbols
#     /libpath          add the WDK kernel library directory
#     *.lib             kernel import libs the driver depends on
$command = @"
call "$vsDevCmd" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
cl /nologo /c /W4 /WX /wd4324 /Zi /O2 /kernel /GS /Qspectre /D_AMD64_ /DAMD64 /D_WIN64 /DUNICODE /D_UNICODE /I"$sharedInclude" /I"$kmInclude" /I"$sharedWdkInclude" /Fo"$objectPath" "$driverSource"
if errorlevel 1 exit /b %errorlevel%
link /nologo /driver /subsystem:native /machine:x64 /entry:DriverEntry /debug /out:"$sysPath" /pdb:"$pdbPath" /libpath:"$kmLib" "$objectPath" FltMgr.lib ntoskrnl.lib BufferOverflowK.lib
if errorlevel 1 exit /b %errorlevel%
"@

# Path to the temporary batch file we materialize the command block into.
$script = Join-Path $env:TEMP 'ssd-cache-build-driver-direct.cmd'

# Write the batch file as ASCII (cmd.exe does not want a BOM or UTF-16).
Set-Content -LiteralPath $script -Value $command -Encoding ASCII

# Run it: /d skip AutoRun, /s handle the outer quoting, /c run then exit.
cmd.exe /d /s /c "`"$script`""

# Surface any failure from the batch process as a terminating PowerShell error.
if ($LASTEXITCODE -ne 0) {
    throw "Direct driver build failed with exit code $LASTEXITCODE"
}

# Place the INF beside the built .sys so the driver package is self-contained.
Copy-Item -LiteralPath $infSource -Destination $infDestination -Force
