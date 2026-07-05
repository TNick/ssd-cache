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

## Developer guide

All developer-facing setup, build, local startup, driver test-mode, packaging,
and configuration instructions live in [DEVELOPERS.md](DEVELOPERS.md).

## License

Released under the MIT License. See [LICENSE](LICENSE) for details.
