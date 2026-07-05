[CmdletBinding()]
param(
    [switch] $IncludeWix
)

$ErrorActionPreference = 'Stop'

winget install `
    --id Microsoft.WindowsWDK.10.0.26100 `
    --exact `
    --accept-package-agreements `
    --accept-source-agreements

if ($IncludeWix) {
    $dotnetCommand = Get-Command dotnet -ErrorAction SilentlyContinue
    if (-not $dotnetCommand) {
        winget install `
            --id Microsoft.DotNet.SDK.10 `
            --exact `
            --accept-package-agreements `
            --accept-source-agreements
    }
    else {
        $sdkList = dotnet --list-sdks
        if (-not $sdkList) {
            winget install `
                --id Microsoft.DotNet.SDK.10 `
                --exact `
                --accept-package-agreements `
                --accept-source-agreements
        }
    }

    $wixTool = Join-Path $env:USERPROFILE '.dotnet\tools\wix.exe'
    if (Test-Path -LiteralPath $wixTool) {
        dotnet tool uninstall --global wix
    }

    dotnet tool install --global wix --version 4.0.5

    if (Test-Path -LiteralPath $wixTool) {
        & $wixTool extension add --global WixToolset.UI.wixext/4.0.5
    }
}
