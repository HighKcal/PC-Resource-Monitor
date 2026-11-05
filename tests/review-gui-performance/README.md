# Independent GUI performance observation helpers

The submitted tools are `window_boundary.py`, `capture-uia.ps1` and
`summarize-uia.ps1`. Run their commands from the repository root on Windows.
They do not modify product sources or interact with windows through UI
Automation, MSAA, input injection, screenshots or window-text queries. One
designated owner holds the execution slot. Source review and tool preparation
do not establish runtime success. No legacy regression-build entry is required.

`window_boundary.py` exports
`boundary_snapshot(pid, creation_filetime_100ns, executable)`. It verifies the
retained process identity and executable, enumerates top-level windows once,
and returns target-only visible/iconic/foreground, window/client rectangle and DPI
metadata. Call it before warmup and after the acceptance interval, while the target
is alive. It does not establish continuously unobscured visibility or foreground.
Its CLI accepts `--pid`, `--creation`, `--exe`, `--out`; output must be new.
Supply the actual already launched target's PID, creation FILETIME and matching
executable path. For example, with those values held in PowerShell variables:

```powershell
python tests/review-gui-performance/window_boundary.py --pid $targetPid --creation $targetCreation --exe $targetExe --out build-check-window.json
```

The optional external sampler in `tests/performance/measure-native-gui.py`
imports this file when `--window-boundaries` is requested. That sampler takes an
explicit executable, new output and new absolute test DB path, and requires a
self-terminating target. Launch/UAC approval and foreground state remain
separate from these passive helpers.

For a separate passive diagnostic, with a previously launched target still alive:

```powershell
.\tests\review-gui-performance\capture-uia.ps1 -TargetPid $targetPid -Out build-check-etw -Seconds 5
```

This starts one uniquely named ETW session through `logman create trace ... -ets`
and stops only that session in `finally`. The fixed-size file is capped at 64 MB.
No pre-existing session, application, accessibility or security setting is changed.
Access denied is a capture failure; do not change permissions or security policy
to work around it in this task. Inspect a stop failure's named owned session.
The ETL is local raw diagnostic evidence and is not an exported process/UI list.

After all performance measurements and target execution finish:

```powershell
.\tests\review-gui-performance\summarize-uia.ps1 -Out build-check-etw
```

The offline parser streams events, produces aggregate counts and at most 5,000
sanitized request rows, and runs `tracerpt ETL -o NUL -summary FILE -y`. Inspect
the original summary's lost-event and lost-buffer counts explicitly; they are
not assumed to be zero. A file at the cap, unknown event version, parse failure,
tracerpt failure, or unverified loss invalidates an absence claim. A nonzero
target-related event can still establish observed activity despite incomplete
coverage; it cannot establish the complete rate.

For local schema version 0, event 544 is the client-side request writer and its
peer is the server; 545 is the server-side receiver and its peer is the client.
Sanitized rows retain event ID, timestamp, emitting header PID/TID, peer PID and
request ID. Pair opposite PID directions plus request ID, and check peer creation
identity in the two process snapshots. This is connection traffic, not a count of
desktop scans. An unpaired header PID does not identify a universal requester.

Window metadata sampling and the ETW diagnostic add observation cost. Keep exact product
CPU measurements separate, and do not subtract their CPU from product CPU as if
that isolated an automation cost.
