# SSD Cache

SSD Cache keeps a local copy of files you actually use from a network share
(NAS). While the share is online, it quietly mirrors those files onto a fast
local volume—typically a USB SSD. When the NAS is unreachable, you can switch
to **Serve** mode and work from the cache as if the share were still there.

The project is a native Windows stack: a kernel minifilter, a background
service, and a lightweight tray application. There is no separate runtime
beyond Windows itself, SQLite, and the WDK driver.

## What it solves

Network shares are convenient but fragile. A dropped VPN, sleeping NAS, or
offline laptop leaves mapped drives empty and applications failing. SSD Cache
does not replace the NAS; it records which files you touch, copies them in the
background with low-priority I/O, and can repoint a drive letter at the cache
when the source is gone.

## What you get

- **Automatic cache population** — recently read or written files are queued
  for copy after a configurable quiet period (default 60 seconds). A new access
  resets the timer so bursty edits are copied once, not on every keystroke.
- **Offline fallback** — in Serve mode the cache volume is mounted at the
  letter users expect for the NAS share, so existing paths keep working.
- **Durable bookkeeping** — a SQLite index tracks last access, cache presence,
  file sizes, SHA-256 digests, pending jobs, and errors across restarts.
- **Bounded disk use** — before copying, the service evicts least-recently-used
  cached files until enough free space remains (default 6 GiB headroom plus the
  incoming file size).
- **Operator control** — the tray app starts and stops the service, switches
  modes, opens config and logs, and shows driver and service health at a glance.

## How it works

Three processes cooperate. The minifilter is the only source of file-activity
events; without it the service cannot learn what to cache.

```text
  NAS share (UNC)                    Local cache volume (e.g. K:)
        |                                      ^
        |  reads / writes / renames / deletes  |  low-priority copy
        v                                      |
  +-------------+    Filter Manager port   +------------------+
  | CacheMon    | ----------------------> | ssd-cache-service |
  | minifilter  |    CACHEMON_EVENT       | (Windows service) |
  +-------------+                          +------------------+
        ^                                          |
        | register / configure                     | SQLite index,
        |                                          | job scheduler,
  +-------------+                                  | eviction
  | ssd-cache-  | <-------- tray commands --------+
  | tray        |
  +-------------+
```

### End-to-end flow

1. **Observe** — the `CacheMon` minifilter watches the configured source UNC.
   It reports read opens, writes, write-close, renames, and deletes to the
   service through a Filter Manager communication port (`\CacheMonPort`).
2. **Filter** — the service drops noise: Explorer directory listings by
   default, Office lock files (`~$*`), SQLite sidecars (`*-wal`, `*-shm`,
   `*-journal`), and any patterns you add in config.
3. **Record** — surviving events update the SQLite cache index. Paths are
   stored relative to the share root so drive letters are presentation only.
4. **Schedule** — copy-worthy events enqueue a delayed job. Writes are copied
   only after the file is closed, then the delay elapses without further access.
5. **Copy** — a background worker copies source → cache with low-priority I/O,
   computing SHA-256 in the same pass so the source is not read twice. By
   default overwrite decisions use path and size only; optional hash comparison
   trades speed for stricter equality checks.
6. **Evict** — when space is tight, least-recently-accessed cached files are
   removed until the volume meets the free-space target.
7. **Present** — the tray app (via `ModeManager` and Windows mount helpers)
   maps drives according to the active mode (see below).

Portable scheduling, indexing, eviction, and configuration logic live in
`src/core`. Windows-specific service hosting, copying, mounting, and driver
adapters live in `src/platform/win`.

## Operating modes

| Mode         | Source share | Cache volume  | Caching               |
|--------------|--------------|---------------|-----------------------|
| **Disabled** | Unchanged    | Unchanged     | Paused; no new copies |
| **Monitor**  | Mapped to `source_presentation_letter` (e.g. `N:`) | Mounted at `cache_letter` (e.g. `K:`) | Active; cache fills in the background |
| **Serve**    | Unmapped     | Mounted at the source letter (e.g. `N:`) | Paused; users work from the cache offline |

Monitor is the normal online posture: use the NAS as usual while the cache
warms up. Serve is the failover posture: the NAS is intentionally offline or
unreachable and applications should read the cached tree from the familiar
drive letter.

## Repository layout

```text
src/core             Portable core: config, SQLite cache index, job scheduler,
                     path mapping, LRU eviction, mode policy.
src/platform/win     Windows adapters: service host, tray integration, UNC
                     mapping, volume mount points, low-priority file copy,
                     Filter Manager activity source.
src/driver           CacheMon minifilter (WDK) and INF — required for product
                     builds; observes I/O on the source share.
src/shared           Kernel/user-mode message contract (cachemon_messages.h).
src/service          Windows service entry point (ssd-cache-service.exe).
src/app              System-tray application (ssd-cache-tray.exe).
tests                Unit tests for portable core logic.
third_party/sqlite   Vendored SQLite amalgamation.
scripts              Build, driver install, health checks, packaging helpers.
docs                 Design notes and driver test-signing walkthrough.
config               Sample config.ini.
```

Build artifacts include `ssd-cache-service.exe`, `ssd-cache-tray.exe`, and the
signed `cachemon.sys` driver package. A user-mode-only CMake preset exists for
core development but cannot observe real file activity without the driver. When
tests are enabled (the default), CMake also builds `ssd-cache-core-tests.exe`.

## Executables

Typical Debug binaries live under `build\windows-driver-debug\Debug\` after a
local build. Each executable accepts at most one primary command per invocation;
when multiple switches are present, the first one recognized in the order
documented below wins.

CLI output is written to the parent console when the process is launched from a
terminal; otherwise a message box is shown (both programs use the Windows GUI
subsystem and do not allocate their own console).

### `ssd-cache-service`

Windows background service for caching, scheduling, and eviction. With no
command-line switch the executable registers with the Service Control Manager
(SCM) as `SsdCacheService` and runs the service host loop. Operators and scripts
normally invoke it with one of the switches below.

```text
ssd-cache-service [option]
```

#### Default (no option)

Runs as an installed Windows service. The SCM starts this entry point; do not
use it together with `--console` on the same machine.

#### `--install`

Registers `ssd-cache-service.exe` with the SCM using the path of the running
binary.

- **Exit code:** `0` on success, `1` on failure.

#### `--start`

Tells the SCM to start the installed `SsdCacheService` instance.

- **Exit code:** `0` on success, `1` on failure.

#### `--stop`

Tells the SCM to stop the running `SsdCacheService` instance.

- **Exit code:** `0` on success, `1` on failure.

#### `--status`

Prints the current SCM state of `SsdCacheService` (for example `running`,
`stopped`, or `missing`).

- **Exit code:** `0` when the service is running, `1` otherwise.

#### `--driver-start`

Loads the `CacheMon` minifilter through Filter Manager (`fltmc load`).

- **Exit code:** `0` on success, `1` on failure.

#### `--driver-stop`

Unloads the `CacheMon` minifilter.

- **Exit code:** `0` on success, `1` on failure.

#### `--driver-status`

Prints the combined SCM and Filter Manager state of `CacheMon`.

- **Exit code:** `0` when the driver is fully loaded, `1` otherwise.

#### `--health`

Prints a JSON document with `service`, `driver`, and `overall_healthy` fields.
Each component reports `name`, `state`, and `healthy` (true only when
`state` is `running`).

- **Exit code:** `0` only when both service and driver are running.

#### `--free` `<size>`

Evicts least-recently-accessed cached files until at least `<size>` bytes have
been freed (summed by cached file size). Reads `cache_letter` and
`sqlite_path` from the machine config. May free less than requested when the
cache holds fewer bytes than asked.

- **`<size>` format:** a positive number with an optional unit suffix. Accepted
  suffixes: none or `B` (bytes), `K` or `KB`, `M` or `MB`, `G` or `GB`,
  `T` or `TB` (1024-based). Examples: `1048576`, `500MB`, `2GB`, `1.5 GB`.
- **Confirmation:** when the requested size exceeds one third of the cache
  volume, prompts for confirmation unless `--yes` or `-y` is also present.
- **Exit code:** `0` on success (including partial eviction), `1` when the
  user aborts the confirmation prompt, `2` when `<size>` is missing or invalid.

#### `--yes` / `-y`

Skips the large-eviction confirmation prompt for `--free`. Not meaningful
without `--free`; ignored otherwise.

#### `--open-config`

Creates `%ProgramData%\ssd-cache\config.ini` when missing, then opens it in
Notepad.

- **Exit code:** `0`.

#### `--open-app-log`

Creates `%ProgramData%\ssd-cache\app.log` when missing, then opens it in
Notepad.

- **Exit code:** `0`.

#### `--open-service-log`

Creates `%ProgramData%\ssd-cache\service.log` when missing, then opens it in
Notepad.

- **Exit code:** `0`.

#### `--open-driver-log`

Launches WinDbg (from the Windows Kits debugger directory) elevated with kernel
debugging and `!dbgprint` to capture `CacheMon` `DbgPrintEx` output. Shows a
warning dialog when WinDbg is not installed or cannot be started.

- **Exit code:** `0`.

#### `--console`

Runs the service host logic in the foreground of the current process for local
debugging. Do not run while the installed SCM service instance is also active.

- **Exit code:** the service host return value.

#### `--help` / `-h` / `/?`

Prints usage text and exits.

- **Exit code:** `0`.

### `ssd-cache-tray`

System-tray application for interactive control. With no command-line argument
it registers a notification-area icon, refreshes driver and service status, and
runs a message loop. Every tray menu action is also exposed as a one-shot CLI
command that performs the action and exits.

```text
ssd-cache-tray [command]
```

#### Default (no command)

Starts the tray UI. The icon reflects driver health (green when `CacheMon` is
loaded, yellow when installed but not running, red when missing). The tooltip
shows both driver and service state. Right-click opens the control menu.

- **Exit code:** `0` when the message loop ends normally.

#### `--start-service`

Starts the `SsdCacheService` Windows service.

- **Exit code:** `0` on success, `1` on failure.

#### `--stop-service`

Stops the `SsdCacheService` Windows service.

- **Exit code:** `0` on success, `1` on failure.

#### `--disabled-mode`

Switches to Disabled mode: stops scheduling new cache copies and leaves drive
presentation unchanged relative to the disabled transition. Persists
`mode=disabled` in config on success.

- **Exit code:** `0` on success, `1` on failure.

#### `--monitor-mode`

Switches to Monitor mode: maps the source UNC to `source_presentation_letter`
and mounts the cache volume at `cache_letter`. Enables background caching.
Persists `mode=monitor` in config on success.

- **Exit code:** `0` on success, `1` on failure.

#### `--serve-mode`

Switches to Serve mode: un-maps the source share and mounts the cache volume at
`source_presentation_letter` for offline use. Pauses copy scheduling. Persists
`mode=serve` in config on success.

- **Exit code:** `0` on success, `1` on failure.

#### `--open-config`

Ensures the machine config file exists (creating it with defaults when absent),
then opens it in Notepad.

- **Exit code:** `0`.

#### `--open-app-log`

Opens `%ProgramData%\ssd-cache\app.log` in Notepad (creates an empty file when
missing).

- **Exit code:** `0`.

#### `--open-service-log`

Opens `%ProgramData%\ssd-cache\service.log` in Notepad (creates an empty file
when missing).

- **Exit code:** `0`.

#### `--open-driver-log`

Opens the `CacheMon` kernel log through elevated WinDbg and `!dbgprint`, same as
the service executable's `--open-driver-log`.

- **Exit code:** `0`.

#### `--exit`

Locates a running tray instance by window class and posts `WM_CLOSE` so it
exits. Use this to stop a background tray process from a script.

- **Exit code:** `0` when a running instance was found and signalled, `1`
  when no tray window exists.

#### `--help` / `-h` / `/?`

Prints usage text and exits.

- **Exit code:** `0`.

Unknown switches (arguments starting with `-` or `/` that are not listed above)
print an error and exit with code `2`.

### `ssd-cache-core-tests`

Portable unit-test runner for `src/core` logic (config, cache index, scheduler,
path mapping, eviction). Built when `SSD_CACHE_BUILD_TESTS` is on (CMake default).
Invoked by CTest as the `ssd-cache-core-tests` test target.

```text
ssd-cache-core-tests
```

This executable takes no command-line options. It runs every registered test
case sequentially, prints failures to stderr, and summarizes the pass count on
success.

- **Exit code:** `0` when all tests pass, `1` when any test fails.

## Configuration

Machine-wide settings live in an INI-style file:

```text
%ProgramData%\ssd-cache\config.ini
```

The service and tray read this path. Log files sit alongside it (`app.log`,
`service.log`; the kernel driver uses `DbgPrintEx` — see
[DEVELOPERS.md](DEVELOPERS.md) for log access). Lines are `key=value` pairs;
blank lines and lines starting with `#` are ignored. Keys and values are trimmed
of surrounding whitespace.

A complete example with every supported key:

```ini
source_unc=\\nas\share
source_presentation_letter=N
cache_letter=K
sqlite_path=C:\ProgramData\ssd-cache\cache-index.sqlite3
mode=monitor
copy_delay_seconds=60
compare_hash_before_overwrite=0
min_free_space_mb=6144
ignored_process_patterns=explorer.exe
ignored_path_patterns=~$*;*-wal;*-shm;*-journal
```

The service resolves paths from `source_unc` internally. Drive letters are
presentation only: the Windows service cannot rely on per-user mappings from
an interactive logon session, so UNC is the authoritative source identity.

### `source_unc`

UNC path of the network share to monitor and cache (for example
`\\nas\share`). The minifilter and service match file activity against this
root; paths in the SQLite index are stored relative to it.

- **Default:** empty (no source configured until set).
- **Required for caching:** yes — without a source UNC the service has nothing
  to observe or copy.

### `source_presentation_letter`

Drive letter where Monitor mode maps the source share for interactive users
(for example `N` → `N:\`).

- **Default:** `N`.
- **Format:** a single letter; the first character is used, upper-cased.
- **Used by:** tray mode switching (Monitor and Serve drive layout).

### `cache_letter`

Drive letter of the local cache volume (for example `K` → `K:\`).

- **Default:** `K`.
- **Format:** a single letter; the first character is used, upper-cased.
- **Used by:** background copy destination, free-space checks, and Monitor
  mode presentation.

### `sqlite_path`

Absolute path to the SQLite cache-index database. The index holds observed
files, last-access times, cache presence, file sizes, SHA-256 digests, pending
copy jobs, and per-file errors.

- **Default:** `%ProgramData%\ssd-cache\cache-index.sqlite3` when the key is
  absent or empty (applied by the service and tray on load).
- **Note:** parent directories are created when the config is saved from the
  tray; the database file itself is created on first service use.

### `mode`

Last successfully requested operating mode. The tray uses this to show the
active mode checkmark and disables the menu item for the mode already in
effect.

- **Default:** absent — the tray infers the mode from the current drive layout,
  then falls back to `disabled` when inference fails. The service treats an
  absent value like Monitor for caching purposes (copy scheduling stays active
  when a source is configured).
- **Accepted values:** `disabled`, `monitor`, `serve` (case-insensitive).
- **Updated by:** tray mode commands (`--disabled-mode`, `--monitor-mode`,
  `--serve-mode`) after a successful transition.

### `copy_delay_seconds`

Quiet period, in whole seconds, between observing a copy-worthy access and
starting the background copy. Each new access to the same file resets the
timer. Writes are scheduled only after the file is closed, then this delay
elapses without further access.

- **Default:** `60`.
- **Format:** signed integer parsed as seconds (invalid values throw on load).

### `compare_hash_before_overwrite`

Whether to compare SHA-256 digests before replacing an existing cached file
that already has the same size. When disabled, path and size alone decide
whether a re-copy is needed (faster). When enabled, a matching hash skips the
copy even when timestamps differ; a mismatch forces a refresh.

- **Default:** `0` (disabled).
- **Accepted values:** `1` or `true` to enable; any other value disables.
- **Trade-off:** enabling avoids redundant copies when content is unchanged but
  adds work when hashes must be computed or compared.

### `min_free_space_mb`

Minimum free space to keep on the cache volume, in megabytes (1024×1024 bytes).
Before caching a file, the service evicts least-recently-accessed cached files
until free space is at least `max(incoming file size, min_free_space_mb)`.

- **Default:** `6144` (6 GiB) when the key is absent.
- **Format:** unsigned integer (MiB). Stored internally as bytes
  (`value × 1024 × 1024`).
- **Disable eviction:** set to `0`.

### `ignored_process_patterns`

Comma- or semicolon-separated list of case-insensitive wildcard patterns
(`*` = any run, `?` = any one character). Access events from processes whose
image path or file name matches any pattern are dropped and never scheduled for
copy. Typical use: skip Explorer directory listings.

- **Default when absent:** `explorer.exe`.
- **Disable filtering:** set the key to an empty value
  (`ignored_process_patterns=`).
- **Examples:** `explorer.exe`, `*\\Search*.exe`, `Acrobat?.exe`.

### `ignored_path_patterns`

Comma- or semicolon-separated list of case-insensitive wildcard patterns.
Relative paths (and their file names alone) matching any pattern are never
cached. Typical use: skip Office lock files and SQLite journal sidecars.

- **Default when absent:** `~$*`, `*-wal`, `*-shm`, `*-journal`.
- **Disable filtering:** set the key to an empty value
  (`ignored_path_patterns=`).
- **Examples:** `*.tmp`, `temp\\*`, `~$*`.

## Developer guide

Setup, build presets, local startup, driver test-mode installation, and MSI
packaging are documented in [DEVELOPERS.md](DEVELOPERS.md).

## License

Released under the MIT License. See [LICENSE](LICENSE) for details.
