# Prototype fixture boundary

`experiment_fixture.h` is the synthetic test and benchmark interface. It supplies
borrowed sidecar/core inspection handles, session/fault metadata, explicit
sidecar SQL failure access, and the existing gap/window seed operations. Tests
own their assertions, SQL probes and independent comparator expectations. The
fixed-limit literals in the fixture are seed inputs, not a product oracle.

The current implementation links the independent prototype objects and includes
only `experiment_internal.h` in `experiment_fixture.c`. Public prototype tests
continue to use `experiment.h` without this seam. Borrowed SQLite statements and
text keep their normal SQLite lifetime; callers finalize prepared statements and
finish their own fixture transactions. Returned snapshots retain the public
ownership contract.

Historical source-root comparison is supported by the same fixture implementation:
without `EXPERIMENT_MODULE_SPLIT`, that one compatibility TU includes the selected
root's historical `experiment.c`. Other tests and benchmarks do not include the
monolith. The selected root's retention companion is linked when present.
This preserves the ability to build the same benchmark against pre-extraction
sources. `scripts/build-sources.cmd` selects companions from that root.

The core inspection seam is test-only and delegates to `monitor_fixture`.
Current product source access uses the typed `monitor_source.h` facade;
prototype production files contain no core layout include or source-table SQL.
Retention remains independently callable through its unchanged borrowed
connection/UTC/error-buffer contract.

`tests/prototype/test_source_failures.c` uses the real source view allocator and
free path, with a coordinator read wrapper only to install failures after core
COMMIT. It checks repeated source OOM across reset and successful copy, source
prepare/row-step interruption, sidecar copy failure, array cleanup, fault/reset
and partial diagnostic fields. This focused boundary test does not replace the
independent source association tests or claim all SQLite failure modes.
