[CmdletBinding()]
param(
    [string] $Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'

$vsDevCmd = 'E:\installed\vs\Common7\Tools\VsDevCmd.bat'
$repo = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repo 'src\driver\cachemon_minifilter.vcxproj'

$command = @"
call "$vsDevCmd" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
MSBuild "$project" /t:Rebuild /p:Configuration=$Configuration /p:Platform=x64
if errorlevel 1 exit /b %errorlevel%
"@

$script = Join-Path $env:TEMP 'ssd-cache-rebuild-driver.cmd'
Set-Content -LiteralPath $script -Value $command -Encoding ASCII
cmd.exe /d /s /c "`"$script`""
if ($LASTEXITCODE -ne 0) {
    throw "Driver rebuild failed with exit code $LASTEXITCODE"
}
