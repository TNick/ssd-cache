# SSD Cache

SSD Cache is a native Windows C++ service, tray app, and minifilter driver for
caching recently accessed NAS files onto a local cache disk. The minifilter is
the required activity source: it observes reads, writes, write-close cleanup,
renames, and deletes, then sends events to the service through a Filter Manager
communication port.

The service stores activity and copy state in SQLite, schedules delayed copy
jobs, copies files with low-priority I/O, and computes SHA-256 while copying so
the source is not read twice for normal cache population. The tray app handles
interactive control, including network-drive presentation and mode switching.

## Layout

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

## Tools

Required Windows build tools:

```text
Visual Studio 2022 Build Tools with Desktop development with C++
CMake
Windows Driver Kit 10.0.26100
.NET SDK and WiX .NET tool only when MSI packaging is enabled
```

Verify the local machine with:

```powershell
.\scripts\verify-tools.ps1
```

Verify the packaging tools too with:

```powershell
.\scripts\verify-tools.ps1 -IncludePackaging
```

Install missing winget-managed tools with:

```powershell
.\scripts\install-tools.ps1
```

Install packaging tools too with:

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

For portable user-mode development only, the fallback preset is:

```powershell
.\scripts\build.ps1 -Preset windows-user-mode-debug
```

That fallback cannot populate the database from file access activity and must
not be treated as a product build.

Build the driver-enabled MSI package with:

```powershell
.\scripts\package.ps1 -Preset windows-driver-debug-msi
```

## Driver Deployment

The minifilter package must be signed before production deployment. In a lab,
enable Windows test signing and sign the package with a test certificate before
installing it. Stage the INF with:

```powershell
.\scripts\install-driver.ps1 -DriverBuildDir build\driver\Debug
```

Production releases need a Microsoft-recognized kernel-mode signing path.

## Configuration

The machine-wide config lives under ProgramData:

```text
%ProgramData%\ssd-cache\config.ini
```

Important keys:

```ini
source_unc=\\nas\share
source_presentation_letter=N
cache_letter=K
sqlite_path=C:\ProgramData\ssd-cache\cache-index.sqlite3
copy_delay_seconds=60
compare_hash_before_overwrite=0
```

The service uses `source_unc` internally. Drive letters are presentation only
because Windows services cannot rely on mappings from an interactive logon
session.
