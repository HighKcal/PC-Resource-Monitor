# Phase 2 integration evidence

These submitted tools accept explicit executables, outputs and comparison labels.
The historical phase-2 comparison used the verified **phase 1 final version with
reference-preserving retention** as its baseline. That local preservation at
`build-phase2-20261104/before/{source,artifacts}`, its binaries and measurement
DBs are not included in a fresh submission checkout.

A new comparison requires separately obtained, legitimate before/after source
versions and matching built executables. Record their identities and use the same
fixed inputs/build conditions. Do not substitute the current candidate, an older
pre-fix retention benchmark or an unrelated Git HEAD and label it the historical
baseline. Running the current executable alone produces current evidence, not a
reproduction of the old before/after result.

`run_pipeline.py --exe EXE --out DIRECTORY --phase LABEL` serially runs the fixed
500-instance matrix: initial/mature history, same/mixed/distinct paths, and
core/prototype (12 cases). Every run uses a new synthetic DB prefix. It records
the executable hash, exact commands, environment CPU count, elapsed time, exit
and raw log. `--cases` explicitly selects a subset; `--profile` is diagnostic and
is kept separate from uninstrumented timing. Warm-after-seeding/verification
does not mean the OS cache was flushed. Only main runs performance, with no other
build or load test running.

`summarize_pipeline.py DIRECTORY BEFORE AFTER [--cases ...]` requires successful
execution manifests and identical independently calculated snapshot/core/sidecar
digests. It reports timing, process memory, handles, DB/WAL sizes, actual next QPC
intervals and optional SQL count/VM/row deltas. File-size or performance changes
are reported rather than silently rejected or treated as product errors. A
single run with three completed-window samples is not statistical proof.

`audit_databases.py DIRECTORY --pattern 'LABEL-*.sqlite' --output FRESH.json`
opens only the explicitly selected stopped synthetic DBs read-only. It checks
integrity, declared foreign keys, closed core sessions and the sidecar's logical
decision/reduction/evaluation references. Expirable episode summary identifiers
are deliberately not treated as live references. This extra inspection uses
Python's recorded SQLite version; product tests use the vendored library.

The phase 1 `evidence.py capture`/pipeline digest comparison and `run-retained.py`
remain usable. For reproduction of that historical retention comparison, supply the separately
preserved **phase 1 fixed** before executable and final phase 2 after executable.
For new work, explicitly identify the actual comparison versions instead. The common fixture adapters select matching
historical headers/product TUs without changing the seed or independent hash
calculation. All generated DBs, executables and logs stay in the new phase 2
output directory. These backend tools do not validate foreground administrator
GUI interaction, quiet GUI performance, user utility or long-running leaks.
