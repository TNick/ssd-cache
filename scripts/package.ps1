<#
.SYNOPSIS
    Builds the project and produces a Windows Installer (MSI) package with CPack
    and the WiX toolset.

.DESCRIPTION
    End-to-end packaging entry point. It first performs a full build using the
    given preset (by delegating to build.ps1), then runs CPack over the
    resulting build tree to generate an MSI installer via the WiX (WIX)
    generator.

    Steps performed:
      1. Prepend the per-user .NET tools directory to PATH so the WiX .NET tool
         (installed as a dotnet global tool) is discoverable by CPack.
      2. Run build.ps1 -Preset <Preset> to configure/build/test the project.
      3. Validate the expected build directory and CPack config were produced.
      4. Resolve cpack.exe (preferring a known location, falling back to PATH).
      5. Run CPack with the WIX generator to emit the MSI into build\packages.

.PARAMETER Preset
    CMake preset to build and package. Determines both the build directory
    (build\<Preset>) and the CPack configuration used. Defaults to
    'windows-driver-debug-msi'.

.PARAMETER Configuration
    Build configuration CPack should package ('Debug' or 'Release'), passed to
    CPack via -C. Defaults to 'Debug'.

.OUTPUTS
    An MSI installer written under build\packages.

.NOTES
    Requires CMake/CPack and the WiX toolset (installed as a dotnet global tool,
    plus the WixToolset.UI.wixext extension). Use verify-tools.ps1
    -IncludePackaging to confirm these prerequisites.
#>
[CmdletBinding()]
param(
    # CMake preset to build and package.
    [string] $Preset = 'windows-driver-debug-msi',
    # Build configuration CPack should package (passed via -C).
    [string] $Configuration = 'Debug'
)

# Stop on the first unhandled error.
$ErrorActionPreference = 'Stop'

# Repository root (parent of the scripts\ directory).
$repo = Split-Path -Parent $PSScriptRoot

# Per-user dotnet global tools directory (where the WiX tool is installed).
$dotnetTools = Join-Path $env:USERPROFILE '.dotnet\tools'
# Put it first on PATH so CPack/WiX can find wix.exe when packaging.
if (Test-Path -LiteralPath $dotnetTools) {
    $env:PATH = "$dotnetTools;$env:PATH"
}

# Delegate the configure/build/test cycle to the shared build script.
$buildScript = Join-Path $PSScriptRoot 'build.ps1'
& $buildScript -Preset $Preset
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

# The build tree CMake generated for this preset.
$buildDir = Join-Path $repo "build\$Preset"
if (-not (Test-Path -LiteralPath $buildDir)) {
    throw "Build directory not found: $buildDir"
}

# Preferred fixed location of CPack; fall back to whatever is on PATH.
$cpackExe = 'E:\installed\cmake\bin\cpack.exe'
if (-not (Test-Path -LiteralPath $cpackExe)) {
    $cpackCommand = Get-Command cpack.exe -ErrorAction SilentlyContinue
    if (-not $cpackCommand) {
        throw 'CMake CPack executable was not found.'
    }

    # Use the resolved path from PATH lookup.
    $cpackExe = $cpackCommand.Source
}

# The CPack configuration CMake emits during configure; required to package.
$cpackConfig = Join-Path $buildDir 'CPackConfig.cmake'
if (-not (Test-Path -LiteralPath $cpackConfig)) {
    throw "CPack config was not generated: $cpackConfig"
}

# Output directory for the generated installer package(s).
$packageDir = Join-Path $repo 'build\packages'
# Create it if needed; -Force keeps this idempotent. Out-Null hides the object.
New-Item -ItemType Directory -Path $packageDir -Force | Out-Null

# Run CPack from the repo root so relative paths resolve as expected;
# Push/Pop-Location in a try/finally guarantees we restore the location.
Push-Location $repo
try {
    # Generate the package:
    #   -G WIX          use the WiX (MSI) generator
    #   -C $Configuration  package this build configuration
    #   --config        the CPack config file to drive packaging
    #   -B $packageDir  output directory for the package
    & $cpackExe `
        -G WIX `
        -C $Configuration `
        --config $cpackConfig `
        -B $packageDir
    if ($LASTEXITCODE -ne 0) {
        throw "CPack failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}
