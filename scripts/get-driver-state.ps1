<#
.SYNOPSIS
    Reports the current state of the CacheMon minifilter driver.

.DESCRIPTION
    A read-only status probe used by tooling and humans to answer "is the
    driver installed, and is it actually filtering?". It checks two independent
    sources of truth and reconciles them:

      1. The Service Control Manager (SCM) entry for the 'CacheMon' service.
         If there is no service registered, the driver is not installed.
      2. The live minifilter list from `fltmc.exe filters`. A driver can be
         registered as a service yet not attached/loaded as a minifilter, so
         this confirms it is genuinely running in the filter stack.

    The script prints a single human-readable status line and communicates the
    machine-readable result through its exit code (see .OUTPUTS).

.OUTPUTS
    Writes one status line to stdout and sets the process exit code:
        0  - CacheMon is loaded and listed by fltmc (fully running).
        1  - CacheMon exists in the SCM but is not attached as a minifilter
             (e.g. registered/stopped, or running in SCM but not in fltmc).
        2  - CacheMon service is missing entirely (not installed).

.NOTES
    `fltmc.exe filters` typically requires an elevated (administrator) context;
    without elevation the loaded check may fail and the script will fall back to
    the SCM status.
#>
[CmdletBinding()]
param()

# Stop on the first unhandled error.
$ErrorActionPreference = 'Stop'

# Query the driver service state through the SCM first.
# -ErrorAction SilentlyContinue: return $null instead of throwing when absent.
$service = Get-Service -Name 'CacheMon' -ErrorAction SilentlyContinue
if ($null -eq $service) {
    # No SCM entry => the driver was never installed. Exit code 2.
    Write-Output 'CacheMon: missing'
    exit 2
}

# Confirm whether the minifilter is currently loaded.
# Capture the full output of fltmc's filter listing for inspection.
$filters = & fltmc.exe filters
# Tracks whether a CacheMon row was found in that listing.
$isLoaded = $false
# Only trust the output if fltmc actually succeeded.
if ($LASTEXITCODE -eq 0) {
    # Match a line beginning with the CacheMon filter name (optionally indented).
    $isLoaded = $filters | Select-String -Pattern '^\s*CacheMon\s+'
}

if ($isLoaded) {
    # Present in the live filter stack => fully operational. Exit code 0.
    Write-Output 'CacheMon: running'
    exit 0
}

# Not listed by fltmc, but the service exists. If the SCM says it is Running,
# report the discrepancy (service up but filter not attached). Exit code 1.
if ($service.Status -eq [System.ServiceProcess.ServiceControllerStatus]::Running) {
    Write-Output 'CacheMon: running in SCM but not listed by fltmc'
    exit 1
}

# Otherwise report the raw SCM status (e.g. "stopped") in lowercase. Exit code 1.
Write-Output ("CacheMon: {0}" -f $service.Status.ToString().ToLowerInvariant())
exit 1
