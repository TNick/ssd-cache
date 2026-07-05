[CmdletBinding()]
param(
    [string] $Preset = 'windows-driver-debug'
)

$ErrorActionPreference = 'Stop'

$vsDevCmd = 'E:\installed\vs\Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path -LiteralPath $vsDevCmd)) {
    throw "Visual Studio developer command script not found: $vsDevCmd"
}

$repo = Split-Path -Parent $PSScriptRoot
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

$script = Join-Path $env:TEMP 'ssd-cache-build.cmd'
Set-Content -LiteralPath $script -Value $command -Encoding ASCII
cmd.exe /d /s /c "`"$script`""
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}
