[CmdletBinding()]
param(
    [switch] $IncludePackaging
)

$ErrorActionPreference = 'Stop'

function Test-Command {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Name
    )

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        Write-Host "missing: $Name"
        return $false
    }

    Write-Host "found: $Name -> $($command.Source)"
    return $true
}

function Test-PathRequired {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,
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

function Test-DotNetSdk {
    $sdkList = dotnet --list-sdks
    if (-not $sdkList) {
        Write-Host 'missing: .NET SDK'
        return $false
    }

    Write-Host "found: .NET SDK -> $($sdkList -join ', ')"
    return $true
}

function Test-WixExtension {
    param(
        [Parameter(Mandatory = $true)]
        [string] $WixPath
    )

    $extensions = & $WixPath extension list --global
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'missing: WiX extension cache'
        return $false
    }

    if ($extensions -notmatch 'WixToolset.UI.wixext') {
        Write-Host 'missing: WixToolset.UI.wixext'
        return $false
    }

    Write-Host 'found: WixToolset.UI.wixext'
    return $true
}

$ok = $true
$ok = (Test-Command -Name cmake) -and $ok
$ok = (Test-PathRequired `
    -Path 'E:\installed\vs\Common7\Tools\VsDevCmd.bat' `
    -Label 'Visual Studio developer prompt') -and $ok
$ok = (Test-PathRequired `
    -Path 'E:\installed\vs\VC\Tools\MSVC' `
    -Label 'MSVC toolset') -and $ok
$ok = (Test-PathRequired `
    -Path 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\km\fltKernel.h' `
    -Label 'WDK fltKernel.h') -and $ok
$ok = (Test-PathRequired `
    -Path 'C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\km\x64\FltMgr.lib' `
    -Label 'WDK FltMgr.lib') -and $ok
$ok = (Test-PathRequired `
    -Path 'C:\Program Files (x86)\Windows Kits\10\build\10.0.26100.0' `
    -Label 'WDK MSBuild targets') -and $ok
$ok = (Test-PathRequired `
    -Path 'E:\installed\vs\MSBuild\Microsoft\VC\v170\Platforms\x64\PlatformToolsets\WindowsKernelModeDriver10.0\Toolset.props' `
    -Label 'VS WDK x64 platform toolset') -and $ok

if ($IncludePackaging) {
    $wixTool = Join-Path $env:USERPROFILE '.dotnet\tools\wix.exe'
    $ok = (Test-Command -Name dotnet) -and $ok
    $ok = (Test-DotNetSdk) -and $ok
    $ok = (Test-PathRequired `
        -Path $wixTool `
        -Label 'WiX .NET tool') -and $ok

    if (Test-Path -LiteralPath $wixTool) {
        $ok = (Test-WixExtension -WixPath $wixTool) -and $ok
    }
}

if (-not $ok) {
    exit 1
}
