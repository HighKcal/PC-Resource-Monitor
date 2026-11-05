# Native GUI cache diagnostic copy

The submitted cache diagnostic prepares a separate copy of the current native
GUI and links matching current libraries. Product sources and installed binaries
are unchanged. Run from the repository root with Windows x64 MSVC/SDK tools and
Python 3.9 or newer; choose new directory names:

```powershell
.\build-monitor.cmd build-check-cache-core
python tests/phase2-core/prepare-cache-probe.py --source-root . --output build-check-cache-probe
.\tests\performance\native-gui\build-cache-probe.cmd build-check-cache-probe build-check-cache-core .
```

Preparation only writes a diagnostic source copy and its input manifest. Its
output must be a fresh directory inside `--source-root`. It checks three unique
instrumentation anchors and copies the current `native_*.h` headers, including
`native_availability.h`, plus `native_cache_probe.h`. If an anchor changes,
preparation fails rather than silently instrumenting another location.

The build arguments are `OUTPUT PRODUCT SOURCE`, all explicit in the example.
`OUTPUT` names that prepared copy, `PRODUCT` contains the matching `monitor.lib`
and `grouping.lib`, and `SOURCE` is their source root. The build also compiles
`tests/support/monitor_fixture.c`; its header and the matching product headers
are required. The fixture exposes borrowed diagnostic access without depending
on the `MonitorEngine` memory layout. Never mix a current copy with another
version's headers or library.

The result is `build-check-cache-probe/monitor_gui_cache_probe.exe`. Building it
does not run a GUI or establish a measurement result. This copied diagnostic has
its own build/manifest settings; it does not establish foreground interaction or
privilege equivalence with the product administrator GUI.

An optional bounded synthetic headless run, separate from any performance
measurement, can use a new database:

```powershell
.\build-check-cache-probe\monitor_gui_cache_probe.exe --headless --synthetic --db build-check-cache-probe\synthetic-cache.sqlite --count 23 --interval-ms 100
```

`CACHE_PROBE` samples execute on the database-owning worker after database open
and each successful ingest, after the collector batch is freed and before
grouping/publication. Each sample records the independent `sqlite3_db_status`
results for `CACHE_USED`, `STMT_USED`, and `SCHEMA_USED` with `resetFlg=0`, process
PrivateUsage/WorkingSetSize, batch/row/window/instance counts and elapsed time.
Failed queries remain unavailable (`-1`) with return/error codes. The diagnostic
prepares no SQL, changes no PRAGMA/cache limit/schema/collection policy, and has
no UI subclass or periodic UI-message counters.

These SQLite categories describe approximate current category bytes, not all
SQLite allocations or total process memory. Their high-water values are not
interpreted as peaks. `STMT_USED` includes lookaside and heap. Do not sum the
categories and equate them with process PrivateUsage. Correlated growth and a
short synthetic run cannot establish a long-term memory bound or identify every
remaining allocation as a GUI leak. The vendored `third_party/sqlite/sqlite3.h`
documents the status categories used here.

Samples and log flushes add observation cost. `CACHE_PROBE observer` includes
status calls, memory sampling and the preceding sample log flush; it excludes
printing its own observer line. One designated owner runs measurements serially,
without another build or load test. Keep exact product measurements separate
from this copied diagnostic. Historical UI-message probe generators and pinned
cache preparations are local archival tools outside this submission; their old
results are not fresh-checkout runtime evidence.
