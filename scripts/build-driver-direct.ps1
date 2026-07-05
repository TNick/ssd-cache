[CmdletBinding()]
param(
    [string] $Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'

$vsDevCmd = 'E:\installed\vs\Common7\Tools\VsDevCmd.bat'
$repo = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $repo "build\driver\$Configuration"
$wdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$wdkVersion = '10.0.26100.0'

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$driverSource = Join-Path $repo 'src\driver\src\driver.c'
$sharedInclude = Join-Path $repo 'src\shared'
$objectPath = Join-Path $outDir 'driver.obj'
$sysPath = Join-Path $outDir 'cachemon.sys'
$pdbPath = Join-Path $outDir 'cachemon.pdb'
$infSource = Join-Path $repo 'src\driver\cachemon.inf'
$infDestination = Join-Path $outDir 'cachemon.inf'

$kmInclude = Join-Path $wdkRoot "Include\$wdkVersion\km"
$sharedWdkInclude = Join-Path $wdkRoot "Include\$wdkVersion\shared"
$kmLib = Join-Path $wdkRoot "Lib\$wdkVersion\km\x64"

$command = @"
call "$vsDevCmd" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
cl /nologo /c /W4 /WX /wd4324 /Zi /O2 /kernel /GS /Qspectre /D_AMD64_ /DAMD64 /D_WIN64 /DUNICODE /D_UNICODE /I"$sharedInclude" /I"$kmInclude" /I"$sharedWdkInclude" /Fo"$objectPath" "$driverSource"
if errorlevel 1 exit /b %errorlevel%
link /nologo /driver /subsystem:native /machine:x64 /entry:DriverEntry /debug /out:"$sysPath" /pdb:"$pdbPath" /libpath:"$kmLib" "$objectPath" FltMgr.lib ntoskrnl.lib BufferOverflowK.lib
if errorlevel 1 exit /b %errorlevel%
"@

$script = Join-Path $env:TEMP 'ssd-cache-build-driver-direct.cmd'
Set-Content -LiteralPath $script -Value $command -Encoding ASCII
cmd.exe /d /s /c "`"$script`""
if ($LASTEXITCODE -ne 0) {
    throw "Direct driver build failed with exit code $LASTEXITCODE"
}

Copy-Item -LiteralPath $infSource -Destination $infDestination -Force
