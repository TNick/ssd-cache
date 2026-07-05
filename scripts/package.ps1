[CmdletBinding()]
param(
    [string] $Preset = 'windows-driver-debug-msi',
    [string] $Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$dotnetTools = Join-Path $env:USERPROFILE '.dotnet\tools'
if (Test-Path -LiteralPath $dotnetTools) {
    $env:PATH = "$dotnetTools;$env:PATH"
}

$buildScript = Join-Path $PSScriptRoot 'build.ps1'
& $buildScript -Preset $Preset
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

$buildDir = Join-Path $repo "build\$Preset"
if (-not (Test-Path -LiteralPath $buildDir)) {
    throw "Build directory not found: $buildDir"
}

$cpackExe = 'E:\installed\cmake\bin\cpack.exe'
if (-not (Test-Path -LiteralPath $cpackExe)) {
    $cpackCommand = Get-Command cpack.exe -ErrorAction SilentlyContinue
    if (-not $cpackCommand) {
        throw 'CMake CPack executable was not found.'
    }

    $cpackExe = $cpackCommand.Source
}

$cpackConfig = Join-Path $buildDir 'CPackConfig.cmake'
if (-not (Test-Path -LiteralPath $cpackConfig)) {
    throw "CPack config was not generated: $cpackConfig"
}

$packageDir = Join-Path $repo 'build\packages'
New-Item -ItemType Directory -Path $packageDir -Force | Out-Null

Push-Location $repo
try {
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
