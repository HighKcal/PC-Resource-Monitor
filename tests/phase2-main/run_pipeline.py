"""Phase-2 serial fixed-input pipeline matrix; fresh phase outputs only."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--phase", required=True)
    parser.add_argument("--cases", nargs="+")
    parser.add_argument("--profile", action="store_true")
    args = parser.parse_args()
    exe, out = args.exe.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    result_path = out / (args.phase + "-execution.json")
    if result_path.exists():
        parser.error("phase already exists")
    records = []
    result = {"phase": args.phase, "exe": str(exe), "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "logical_cpus": os.cpu_count(), "cache": "warm after history seeding/verification; OS cache not flushed", "status": "running", "commands": records}
    try:
        for days in (0, 14):
            for kind in ("same", "mixed", "distinct"):
                for mode in ("core", "experiment"):
                    case = f"{'mature' if days else 'initial'}500-{kind}-{mode}"
                    if args.cases and case not in args.cases:
                        continue
                    prefix = out / (args.phase + "-" + case)
                    log = prefix.with_suffix(".log")
                    if log.exists() or list(out.glob(prefix.name + "*.sqlite")):
                        raise RuntimeError("Refusing to overwrite " + str(prefix))
                    command = [str(exe), "--out", str(prefix), "--instances", "500", "--days", str(days), "--case", kind, "--mode", mode]
                    if args.profile:
                        command.append("--profile")
                    stamp = time.time()
                    print("START " + case, flush=True)
                    with log.open("w", encoding="utf-8") as handle:
                        child = subprocess.run(command, stdout=handle, stderr=subprocess.STDOUT)
                    record = {"case": case, "command": command, "started_utc": datetime.datetime.fromtimestamp(stamp, datetime.timezone.utc).isoformat(), "seconds": time.time() - stamp, "log": str(log), "exit_code": child.returncode}
                    records.append(record)
                    result_path.write_text(json.dumps(result, indent=2), encoding="utf-8")
                    if child.returncode or not log.read_text(encoding="utf-8").rstrip().endswith("failures=0"):
                        raise RuntimeError("Failed " + case)
        if args.cases and {r["case"] for r in records} != set(args.cases):
            raise RuntimeError("Unknown/missing requested case")
        result["status"] = "passed"
    except Exception as exc:
        result["status"] = "failed"
        result["error"] = str(exc)
        raise
    finally:
        result_path.write_text(json.dumps(result, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
