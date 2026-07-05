<#
.SYNOPSIS
    Installs the CacheMon minifilter driver from a build output directory.

.DESCRIPTION
    Takes a directory containing a built driver (cachemon.inf plus the
    cachemon.sys/.pdb it references) and installs it on the local machine using
    pnputil. The INF drives the install: pnputil adds the driver package to the
    Windows driver store and then installs it.

    Steps performed:
      1. Resolve and validate that cachemon.inf exists in the given directory.
      2. Run `pnputil /add-driver <inf> /install` to stage and install it.

.PARAMETER DriverBuildDir
    Path to the directory that holds the built driver package (the folder
    produced by build-driver-direct.ps1 or rebuild-driver.ps1, e.g.
    build\driver\Debug). Must contain cachemon.inf. Mandatory.

.NOTES
    Must be run elevated (administrator). Installing an unsigned/test-signed
    kernel driver additionally requires the machine to be in test-signing mode
    (see docs/03. install-driver-in-test-mode.md).
#>
[CmdletBinding()]
param(
    # Directory containing the built driver package (must include cachemon.inf).
    [Parameter(Mandatory = $true)]
    [string] $DriverBuildDir
)

# Stop on the first unhandled error.
$ErrorActionPreference = 'Stop'

# Full path to the install manifest that describes the driver package.
$infPath = Join-Path $DriverBuildDir 'cachemon.inf'
# Fail early with a clear message if the package hasn't been built here.
if (-not (Test-Path -LiteralPath $infPath)) {
    throw "Driver INF not found: $infPath"
}

# Add the driver package to the driver store and install it in one call.
#   /add-driver <inf>  stage the package described by the INF
#   /install           immediately install it on this machine
pnputil.exe /add-driver $infPath /install
