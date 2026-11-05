"""Prepare a fresh current-source diagnostic copy; never builds or runs a GUI.
The older pinned 2026-11-03 preparation script remains unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.source_root.resolve()
    output = args.output.resolve() if args.output.is_absolute() else (source / args.output).resolve()
    if not output.is_relative_to(source) or output == source or output.exists():
        parser.error("output must be a fresh directory inside source-root")
    gui = source / "gui/monitor_gui.c"
    probe = source / "tests/performance/native-gui/native_cache_probe.h"
    inputs = [gui, probe, *sorted((source / "gui").glob("native_*.h")), Path(__file__).resolve()]
    text = gui.read_text(encoding="utf-8")
    include = '#include "program_groups.h"'
    opened = '    if (!app.synthetic && resource_collector_create(NULL, &collector, &os_error) != RESOURCE_OK) {'
    ingested = '            if (!ok) { resource_display_batch_free(&display); publish(error_snapshot(monitor_error(engine), 1)); app.worker_error = 1; goto finished; }'
    replacements = [(include, include + '\n#include "native_cache_probe.h"'),
                    (opened, '    cache_probe_emit(engine,NULL,0,"open");\n' + opened),
                    (ingested, ingested + '\n            cache_probe_emit(engine,snapshot,app.completed+1,"ingest");')]
    for before, after in replacements:
        if text.count(before) != 1:
            parser.error("current GUI instrumentation anchor is not unique: " + before)
        text = text.replace(before, after, 1)
    manifest = {p.relative_to(source).as_posix(): digest(p) for p in inputs}
    output.mkdir(parents=True)
    (output / "monitor_gui.c").write_text(text, encoding="utf-8")
    for p in inputs[1:-1]:
        (output / p.name).write_bytes(p.read_bytes())
    (output / "source-manifest.json").write_text(json.dumps({"source_root": str(source), "inputs": manifest,
        "instrumentation_sites": 3, "product_gui_modified": False, "executed": False}, indent=2), encoding="utf-8")
    print("Prepared current-source diagnostic copy only: " + str(output))


if __name__ == "__main__":
    main()
