# GUI feedback regressions

Run the two current regressions from the repository root with matching, freshly
built monitor/grouping libraries and the serial execution slot available:

```bat
build-monitor.cmd build-check-monitor
tests\phase1-gui\build.cmd build-check-feedback build-check-monitor\monitor.lib build-check-monitor\grouping.lib
build-check-feedback\test_feedback.exe
build-check-feedback\test_feedback_db.exe build-check-feedback
```

Use new output directory names. The build accepts monitor.lib and grouping.lib
paths as arguments two and three. Historical defaults refer to the previously
verified refactor libraries; a fresh checkout should supply the paths above or
use `scripts/test-profiles.py`, which supplies matching paths. All targets
use C17, `/W4 /WX /O2 /MT`, and the existing Unicode/Win32 compile definitions.

The default builds only `test_feedback.exe` and `test_feedback_db.exe`; it does
not need any preserved `build-*` source. To additionally build the historical
expected-failure executable, supply `historical-before` as argument four:

```bat
tests\phase1-gui\build.cmd build-check-feedback-history MATCHING_MONITOR_LIB MATCHING_GROUPING_LIB historical-before
build-check-feedback-history\test_feedback_before.exe
```

This optional command requires the separately preserved source at
`build-phase1-20261104/before/gui/monitor_gui.c` and its companion GUI headers.
The preservation is local historical evidence and is not included in a fresh
submission checkout. Do not replace it with the current GUI source.

`test_feedback_before.exe` includes that preserved phase-start GUI source. Its two
contract assertions intentionally fail: the seventeenth stored command has no
acknowledgement, and the seventeenth consumed acknowledgement replaces the first
program overlay. Expected exit code: 1, failures: 2. This is a mock DB-write
oracle; it does not claim seventeen SQLite writes occurred.

`test_feedback.exe` includes the current GUI source and mocks engine ingress.
It checks reservation/backpressure, seventeen overlay states, acceptance OOM,
allocation-free completion, initialization and mid-command failures, stop while
executing, session changes, cancellation/expiration tombstones, frame watermark
ordering, actual zero-row storage/collector error frames, absent targets, and
post-commit frame allocation failure. No visible
window, live collector, or database is opened.

`test_feedback_db.exe` links the real engine/SQLite library. It seeds seventeen
synthetic programs in a new PID-named database under the supplied directory,
runs the actual paused GUI worker, verifies sixteen committed writes plus busy
backpressure, drains then writes the seventeenth, and verifies cancellation and
the resume frame watermark against stored feedback. It checks integrity,
foreign keys and closed sessions. It preserves the new database as evidence.

The product GUI's embedded administrator manifest remains the responsibility of
the normal product build; these fixtures are ordinary console executables.
