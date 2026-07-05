# Developer Guide

This file is the canonical developer runbook for `ssd-cache`.

Agents and contributors should read this file together with
[README.md](README.md) before making code, build, test, driver, packaging, or
local runtime changes.

## Project layout

```text
src/core             Portable scheduling, path, config, and SQLite logic.
src/platform/win     Windows service, tray, mount, copy, and filter adapters.
src/driver           Required WDK minifilter project and INF.
src/shared           Driver/service message contract.
tests                Portable core tests.
third_party/sqlite   Vendored SQLite amalgamation.
scripts              Tooling, build, and driver install helpers.
docs                 Design and research notes.
```

## Required tools

Required Windows build tools:

```text
Visual Studio 2022 Build Tools with Desktop development with C++
CMake
Windows Driver Kit 10.0.26100
.NET SDK and WiX .NET tool only when MSI packaging is enabled
```

Verify the local machine:

```powershell
.\scripts\verify-tools.ps1
```

Verify packaging tools too:

```powershell
.\scripts\verify-tools.ps1 -IncludePackaging
```

Install missing winget-managed tools:

```powershell
.\scripts\install-tools.ps1
```

Install packaging tools too:

```powershell
.\scripts\install-tools.ps1 -IncludeWix
```

If the WDK bootstrapper cannot download payload MSIs, fix network or firewall
rules and rerun the script. The build requires WDK kernel headers, libraries,
and MSBuild driver targets.

## Build

Build the required driver-enabled Debug preset:

```powershell
.\scripts\build.ps1 -Preset windows-driver-debug
```

This configures, builds, and runs tests for the main local development build
under `build\windows-driver-debug`.

For portable user-mode development only:

```powershell
.\scripts\build.ps1 -Preset windows-user-mode-debug
```

That fallback skips the required WDK minifilter. It cannot populate the access
database from file activity and must not be treated as a product build.

Build the MSI package:

```powershell
.\scripts\package.ps1 -Preset windows-driver-debug-msi
```

## Local startup

These commands assume you already built the `windows-driver-debug` preset and
the minifilter driver is already installed and loaded.

Unified operator entry point:

```powershell
.\build\windows-driver-debug\Debug\ssd-cache-service.exe <command>
```

Supported service commands:

```text
--install
--start
--stop
--status
--driver-start
--driver-stop
--driver-status
--health
--open-config
--open-app-log
--open-service-log
--open-driver-log
--console
```

The built tray executable also supports direct commands:

```powershell
.\build\windows-driver-debug\Debug\ssd-cache-tray.exe <command>
```

Supported tray commands:

```text
--start-service
--stop-service
--disabled-mode
--monitor-mode
--serve-mode
--open-config
--open-app-log
--open-service-log
--open-driver-log
--exit
```

Install the Windows service once:

```powershell
.\build\windows-driver-debug\Debug\ssd-cache-service.exe --install
```

This registers `ssd-cache-service.exe` with the Windows Service Control
Manager.

Start the Windows service:

```powershell
.\build\windows-driver-debug\Debug\ssd-cache-service.exe --start
```

This tells the Service Control Manager to start the installed SSD Cache
service.

Start the Windows service with the repo helper:

```powershell
.\scripts\start-service.ps1
```

Start the tray application in your interactive session:

```powershell
Start-Process .\build\windows-driver-debug\Debug\ssd-cache-tray.exe
```

This launches the tray UI used to control the service and switch modes.

Check the driver state from the command line:

```powershell
.\scripts\get-driver-state.ps1
```

This reports whether the `CacheMon` minifilter driver is missing, stopped, or
running. It checks both the SCM service entry and the loaded filter list and
returns a non-zero exit code when the driver is not fully loaded.

Check the Windows service state from the command line:

```powershell
.\scripts\get-service-state.ps1
```

This reports whether `SsdCacheService` is missing, stopped, or running and
returns a non-zero exit code when it is not running.

Check the combined system health from the command line:

```powershell
.\scripts\get-health.ps1
```

This returns JSON with the service state, driver state, and an
`overall_healthy` flag. It exits with code `0` only when both are running.

Open the component log files from the command line:

```powershell
.\scripts\open-log.ps1 -Component app
.\scripts\open-log.ps1 -Component service
.\scripts\open-log.ps1 -Component driver
```

This opens app and service logs in Notepad. For the driver component it opens
the WDK WinDbg debug-print view because the kernel driver writes diagnostics
with `DbgPrintEx`.

Stop the service:

```powershell
.\build\windows-driver-debug\Debug\ssd-cache-service.exe --stop
```

Stop the service with the repo helper:

```powershell
.\scripts\stop-service.ps1
```

Run the service in the foreground instead of as a Windows service:

```powershell
.\build\windows-driver-debug\Debug\ssd-cache-service.exe --console
```

Use console mode only for local debugging. Do not use it together with the
installed service instance.

Start and stop the driver from the command line:

```powershell
.\scripts\start-driver.ps1
.\scripts\stop-driver.ps1
```

These load and unload the `CacheMon` minifilter through Filter Manager.

The tray icon also reflects driver state:

- Green arrow application icon: driver running.
- Yellow warning application icon: driver installed but not running, or
  transitioning state.
- Red stop application icon: driver missing.

The tray tooltip shows both the driver state and the SSD Cache service state.
The tray menu also includes commands to open the config file, app log, service
log, and driver log.

## Configuration

The machine-wide config lives under ProgramData:

```text
%ProgramData%\ssd-cache\config.ini
```

The log files live in the same directory:

```text
%ProgramData%\ssd-cache\app.log
%ProgramData%\ssd-cache\service.log
```

The kernel driver emits diagnostics with `DbgPrintEx`. Use
`.\scripts\open-log.ps1 -Component driver` or `--open-driver-log` to open the
WDK WinDbg debug-print view instead of a ProgramData text file.

Sample values:

```ini
source_unc=\\nas\share
source_presentation_letter=N
cache_letter=K
sqlite_path=C:\ProgramData\ssd-cache\cache-index.sqlite3
mode=monitor
copy_delay_seconds=60
compare_hash_before_overwrite=0
ignored_process_patterns=explorer.exe
ignored_path_patterns=~$*;*-wal;*-shm;*-journal
```

The service uses `source_unc` internally. Drive letters are presentation only
because Windows services cannot rely on mappings from an interactive logon
session.

`mode` records the last successfully requested tray mode. The tray uses it to
show the active mode checkmark and disables the already-active mode item so
clicking it is a no-op.

`ignored_process_patterns` is a comma- or semicolon-separated list of
case-insensitive wildcard patterns matched against both process image path and
file name. When the option is absent, the service defaults to `explorer.exe`.
Set `ignored_process_patterns=` explicitly to disable process filtering.

`ignored_path_patterns` is a comma- or semicolon-separated list of
case-insensitive wildcard patterns matched against both relative path and file
name. When the option is absent, the service ignores Office lock files such as
`~$Report.docx` and SQLite sidecars such as `db-wal`, `db-shm`, and
`cache-index.sqlite3-journal`. Set `ignored_path_patterns=` explicitly to
disable path filtering.

## Driver install in test mode

Run the commands in this section from an elevated PowerShell in the repo root.
These are the commands from
[docs/03. install-driver-in-test-mode.md](docs/03.%20install-driver-in-test-mode.md)
with a short explanation for each one.

Build the driver-enabled binaries:

```powershell
.\scripts\build.ps1 -Preset windows-driver-debug
```

This produces the driver package and user-mode binaries used by the later
signing and install steps.

Define the build output folder and tool paths:

```powershell
$pkg = Resolve-Path .\build\driver\Debug
$signtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$inf2cat = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\Inf2Cat.exe'
```

This points PowerShell at the generated driver package directory and the WDK
signing tools.

Create a local code-signing certificate:

```powershell
$cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject 'CN=ssd-cache local test driver signing' `
    -CertStoreLocation Cert:\LocalMachine\My `
    -KeyExportPolicy Exportable `
    -KeyAlgorithm RSA `
    -KeyLength 3072 `
    -HashAlgorithm SHA256 `
    -NotAfter (Get-Date).AddYears(3)
```

This creates a machine-local certificate for test-signing the driver package.

Export the certificate to the driver output folder:

```powershell
Export-Certificate -Cert $cert -FilePath "$pkg\ssd-cache-test.cer"
```

This writes a `.cer` file that can be imported into trusted certificate
stores.

Trust the certificate on the local machine:

```powershell
Import-Certificate -FilePath "$pkg\ssd-cache-test.cer" `
    -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath "$pkg\ssd-cache-test.cer" `
    -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
```

This allows Windows to trust the locally generated test-signing certificate.

Generate the driver catalog:

```powershell
& $inf2cat /driver:$pkg /os:10_GE_X64 /verbose
```

This creates `cachemon.cat` from the INF and package contents for the target
Windows platform.

Sign the catalog and driver binary:

```powershell
& $signtool sign /v /fd SHA256 /sm /s My /sha1 $cert.Thumbprint `
    /tr http://timestamp.digicert.com /td SHA256 "$pkg\cachemon.cat"
& $signtool sign /v /fd SHA256 /sm /s My /sha1 $cert.Thumbprint `
    /tr http://timestamp.digicert.com /td SHA256 "$pkg\cachemon.sys"
```

This applies the test certificate signature to the catalog and `.sys` file.

Verify the local test signatures:

```powershell
& $signtool verify /v /pa /c "$pkg\cachemon.cat" "$pkg\cachemon.inf"
& $signtool verify /v /pa "$pkg\cachemon.sys"
```

This checks that the generated catalog and the embedded driver binary
signatures validate against the locally trusted test certificate. Do not use
`/kp` as the local self-signed test pass/fail check. It validates
kernel-mode/release policy and can report that this test certificate does not
chain to a Microsoft root certificate.

Enable Windows test-signing mode:

```powershell
bcdedit.exe /set TESTSIGNING ON
```

This allows Windows to load test-signed kernel drivers after reboot.

Reboot after enabling `TESTSIGNING`.

If `bcdedit` says the value is protected by Secure Boot policy, suspend
BitLocker first if enabled, reboot into firmware, disable Secure Boot, then
run the command again.

Install the driver package:

```powershell
pnputil.exe /add-driver "$pkg\cachemon.inf" /install
```

This stages the INF into the driver store and installs the package.

Load the minifilter:

```powershell
fltmc.exe load CacheMon
```

This asks Filter Manager to load the `CacheMon` minifilter service.

Verify the minifilter is loaded:

```powershell
fltmc.exe filters | findstr /i CacheMon
```

This prints the loaded filter entry so you can confirm that `CacheMon` is
active.

Disable test-signing mode later:

```powershell
bcdedit.exe /set TESTSIGNING OFF
```

This returns Windows to normal kernel-signing policy after another reboot.
