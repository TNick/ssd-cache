[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

& fltmc.exe unload CacheMon
if ($LASTEXITCODE -ne 0) {
    throw 'Failed to unload CacheMon driver.'
}
