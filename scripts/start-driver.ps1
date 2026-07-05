[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

& fltmc.exe load CacheMon
if ($LASTEXITCODE -ne 0) {
    throw 'Failed to load CacheMon driver.'
}
