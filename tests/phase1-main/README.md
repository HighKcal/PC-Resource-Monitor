# Main integration evidence

`evidence.py capture FRESH_DIRECTORY` copies the current tracked and untracked source tree, excluding DBs, plus locally ignored WORK_LOG and SQLite inputs. `evidence.py pipeline PERFORMANCE_DIRECTORY before bugfix after` parses successful logs and requires identical snapshot/source/sidecar digests for matching cases. It preserves raw logs and reports timing changes without treating one run as statistical proof.

`retained_probe.c` copies the previously reviewed synthetic retained-sidecar probe. `PHASE1_NO_PROFILE=1` disables SQLite PROFILE tracing/expanded-SQL capture while retaining QPC and before/after verification. The explicitly selected source root selects the actual product code. For current roots, the shared fixture build compiles and links every selected split prototype module plus `experiment_fixture.c` and `monitor_fixture.c`. The test itself does not include the coordinator. Only the compatibility fixture TU includes a historical monolith when the selected old root predates the split, with that root's companion modules when present. Seed construction and table-value fingerprints are unchanged. The tool does not validate paired source replay, actual UI rendering, or user evaluations.

`build-retained.cmd OUTPUT SOURCE_ROOT MONITOR_LIB` uses the shared TU list and matching fixture adapters. For a current build from the repository root, with new output names:

```powershell
.\build-monitor.cmd build-check-retained-core
.\tests\phase1-main\build-retained.cmd build-check-retained . build-check-retained-core\monitor.lib
```

The actual comparison requires separately supplied, legitimate before/after
executables and a stopped synthetic evidence DB. Historical source/library
preservations and measurement DBs are not included in a fresh submission.
Building the current executable alone does not reproduce an earlier comparison.

`run-retained.py --output FRESH_DIRECTORY --source SYNTHETIC_EVIDENCE --before BEFORE_EXE --after AFTER_EXE` seeds one stopped synthetic source copy with 15,000 additional windows (132,000 decisions total for the chosen input), runs before/after then after/before on fresh copies, and separates uninstrumented stats/retained/expired costs from traced query-plan diagnostics. The seed, executable hashes and commands are recorded. Source DBs are opened read-only and are not modified.

These tools belong to main. Only one holder of the execution slot may run them; no concurrent build, performance run or UI automation is permitted. Warm-after-verification and OS cache not flushed are explicit limitations. Query-only `stats` measures prototype backend refresh, not GUI CPU or rendering performance.
