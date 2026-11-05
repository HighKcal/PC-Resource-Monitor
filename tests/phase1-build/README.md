# Common build and regression profiles

The current submission profile is documented in [tests/README.md](../README.md).
Run from the repository root with Windows x64 MSVC/SDK tools and Python 3:

```powershell
python scripts/test-profiles.py --profile build --output build-check-profiles
python scripts/test-profiles.py --profile all --built-from build-check-profiles --output build-check-regression
```

Both output directories must be new. The current build profile issues 22 build
commands, covering product/basic targets, normal and injected regressions,
prototype modules, GUI feedback, retention, source/history/window/layout and
benchmarks. It does not execute those binaries. The execution profiles and
limits are listed in the linked current guide. Inputs, commands, logs and
artifact hashes are recorded; `--built-from` checks that sources and artifacts
still match. The profile runner does not require historical `build-*` evidence.

`scripts/build-sources.cmd` owns monitor state, Baseline, collector, grouping
and prototype translation-unit lists. `scripts/compile-sources.cmd` maps them
to normal or injected objects. Companion modules are selected from the supplied
source root; a historical prototype override selects companions from its own
folder. `scripts/compile-prototype-fixture.cmd` builds the matching product
modules plus the test-only adapters. Current roots link the split prototype
objects. Only the compatibility fixture TU includes a historical monolith when
that selected root predates the module split.

Existing `.cmd` entry points remain supported. Product flags, vendor warning
isolation, the product GUI administrator manifest and hidden-test asInvoker
manifest remain target-specific. Independent oracle calculations and expected
results remain in the tests. The runner additionally requires actual allocator
failure hits, twelve successful string failure/recovery scenarios with no
outstanding allocations, and retention prepare/step ownership evidence. Each
case gets its own work directory and TEMP/TMP. No product GUI, foreground/UAC
workflow or performance measurement is launched by the passing profiles.

## Optional foundation checks

`build-source-smoke.cmd OUTPUT SOURCE_ROOT MONITOR_LIB` accepts a fresh output,
an explicit source root and its matching library. It compiles normal/injected
TU lists and benchmark targets. `build-injected-smoke.cmd` accepts the same
arguments and builds the calloc-hit wrapper. These helpers work with current
sources; the selected historical root and library must be supplied separately
for a historical comparison. They never modify the selected source root.

```powershell
.\tests\phase1-build\build-source-smoke.cmd build-check-source . build-check-profiles\monitor\monitor.lib
.\tests\phase1-build\build-injected-smoke.cmd build-check-injected . build-check-profiles\monitor\monitor.lib
```

The first phase-1 foundation results remain identified by the local historical
`build-phase1-20261104/stage0-smoke/` evidence. They are not new submission test
results. Historical GUI expected-failure reproduction and retention
`--profile-only` are explicit diagnostics outside the passing profiles; see
[GUI feedback](../phase1-gui/README.md) and
[retention/replay](../phase1-retention/README.md).
