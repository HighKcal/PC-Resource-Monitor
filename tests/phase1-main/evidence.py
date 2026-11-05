"""Capture phase-1 inputs and summarize fixed-input pipeline runs (no product mutation)."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(out):
    if out.exists():
        raise SystemExit("Refusing to overwrite snapshot: " + str(out))
    files = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=ROOT
    ).decode("utf-8").split("\0")
    # WORK_LOG is locally ignored; it is still part of the handoff evidence.
    files = sorted(set(files + ["WORK_LOG.md", "third_party/sqlite/sqlite3.c", "third_party/sqlite/sqlite3.h"]))
    manifest = []
    for rel in files:
        source = ROOT / rel
        if not rel or not source.is_file() or source.suffix.lower() in (".db", ".sqlite", ".sqlite3"):
            continue
        target = out / "source" / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        before, after = sha(source), sha(target)
        if before != after:
            raise SystemExit("Input changed while capturing: " + rel)
        manifest.append({"path": rel, "sha256": before, "bytes": source.stat().st_size})
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
    (out / "status.txt").write_bytes(subprocess.check_output(["git", "status", "--short"], cwd=ROOT))
    print(json.dumps({"snapshot": str(out), "count": len(manifest)}))


def fields(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def pipeline(directory, phases, selected_cases=None):
    result = {"phases": {}, "comparisons": [], "selected_cases": selected_cases}
    for phase in phases:
        phase_runs = {}
        for log in sorted(directory.glob(phase + "-*.log")):
            case_name = log.stem[len(phase) + 1:]
            if selected_cases and case_name not in selected_cases:
                continue
            record = {"log": str(log.resolve()), "sha256": sha(log), "timing": {}, "memory": {}, "digests": {}}
            for line in log.read_text(encoding="utf-8-sig").splitlines():
                item = fields(line)
                if line.startswith("TIMING "):
                    key = item.pop("phase")
                    record["timing"][key] = {k: float(v) for k, v in item.items()}
                elif line.startswith("MEMORY "):
                    key = item.pop("phase")
                    record["memory"][key] = {k: int(v) for k, v in item.items()}
                elif line.startswith("DIGEST "):
                    key = item.pop("label")
                    record["digests"][key] = item
                elif line.startswith("ACTUAL_NEXT_INTERVAL "):
                    record["actual_next_interval"] = item
                elif line.startswith("RESULT "):
                    record["result"] = item
            if record.get("result", {}).get("failures") != "0":
                raise SystemExit("Missing successful result: " + str(log))
            phase_runs[case_name] = record
        if not phase_runs:
            raise SystemExit("No logs for phase: " + phase)
        if selected_cases and set(phase_runs) != set(selected_cases):
            raise SystemExit("Selected case missing from phase: " + phase)
        result["phases"][phase] = phase_runs
    for left, right in zip(phases, phases[1:]):
        a, b = result["phases"][left], result["phases"][right]
        if set(a) != set(b):
            raise SystemExit("Case sets differ: " + left + "/" + right)
        for case in a:
            same = a[case]["digests"] == b[case]["digests"]
            row = {"from": left, "to": right, "case": case, "digests_equal": same}
            for phase in ("ordinary", "close"):
                old = a[case]["timing"][phase]["median_ms"]
                new = b[case]["timing"][phase]["median_ms"]
                row[phase] = {"before_ms": old, "after_ms": new, "change_pct": (new / old - 1) * 100}
            result["comparisons"].append(row)
            if not same:
                raise SystemExit("Unexpected digest difference: " + case)
    target = directory / ("comparison-" + "-".join(phases) + ("-selected" if selected_cases else "") + ".json")
    target.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"output": str(target.resolve()), "cases_per_phase": {k: len(v) for k, v in result["phases"].items()}, "comparisons": result["comparisons"]}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    snap = sub.add_parser("capture")
    snap.add_argument("output", type=Path)
    perf = sub.add_parser("pipeline")
    perf.add_argument("directory", type=Path)
    perf.add_argument("phases", nargs="+")
    perf.add_argument("--cases", nargs="+", help="Explicitly require and compare only these named cases")
    args = parser.parse_args()
    if args.command == "capture":
        capture(args.output.resolve())
    else:
        pipeline(args.directory.resolve(), args.phases, args.cases)


if __name__ == "__main__":
    main()
