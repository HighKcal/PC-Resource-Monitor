"""Compare successful fixed-input logs; never derives expected values from product code."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def fields(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def parse(path):
    result = {"log": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
              "timing": {}, "memory": {}, "digests": {}, "files": {}, "sql": {}, "representatives": {}}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        data = fields(line)
        if line.startswith("TIMING "):
            key = data.pop("phase")
            result["timing"][key] = {k: float(v) for k, v in data.items()}
        elif line.startswith("MEMORY "):
            key = data.pop("phase")
            result["memory"][key] = {k: int(v) for k, v in data.items()}
        elif line.startswith("DIGEST "):
            key = data.pop("label")
            result["digests"][key] = data
        elif line.startswith("FILE "):
            kind = "evidence" if data["path"].endswith("-evidence.sqlite") else "source"
            result["files"][data["phase"] + "/" + kind] = {k: int(data[k]) for k in ("db", "wal")}
        elif line.startswith("SQL "):
            key = "/".join(data.pop(k) for k in ("db", "phase", "category"))
            result["sql"][key] = {k: float(v) if k.endswith("_ms") else int(v) for k, v in data.items()}
        elif line.startswith("SQL_REPRESENTATIVES "):
            result["representatives"][data["db"] + "/" + data["phase"]] = data["value_end_digest"]
        elif line.startswith("ACTUAL_NEXT_INTERVAL "):
            result["actual_next_interval"] = data
        elif line.startswith("RESULT "):
            result["result"] = data
    if result.get("result", {}).get("failures") != "0":
        raise ValueError("No successful result in " + str(path))
    for required in ("timing", "memory", "digests", "files", "actual_next_interval"):
        if not result.get(required):
            raise ValueError("Missing " + required + " in " + str(path))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("--cases", nargs="+", help="Require this explicit subset in both phases")
    args = parser.parse_args()
    directory = args.directory.resolve()
    phases = {}
    for phase in (args.before, args.after):
        execution = json.loads((directory / (phase + "-execution.json")).read_text(encoding="utf-8"))
        if execution["status"] != "passed":
            raise ValueError("Unsuccessful phase " + phase)
        phases[phase] = {item["case"]: parse(Path(item["log"])) for item in execution["commands"]
                         if not args.cases or item["case"] in args.cases}
        if args.cases and set(phases[phase]) != set(args.cases):
            raise ValueError("Selected case missing from " + phase)
    before, after = (phases[x] for x in (args.before, args.after))
    if set(before) != set(after) or not before:
        raise ValueError("Case sets differ or are empty")
    comparisons = []
    for case in before:
        old, new = before[case], after[case]
        row = {"case": case, "digests_equal": old["digests"] == new["digests"],
               "file_sizes_equal": old["files"] == new["files"], "timing": {},
               "private_after_delta_bytes": new["memory"]["after"]["private_bytes"] - old["memory"]["after"]["private_bytes"],
               "peak_ws_delta_bytes": new["memory"]["after"]["peak_working_set"] - old["memory"]["after"]["peak_working_set"],
               "handles_equal": old["memory"]["after"]["handles"] == new["memory"]["after"]["handles"],
               "actual_next_before": old["actual_next_interval"], "actual_next_after": new["actual_next_interval"],
               "sql_delta": {}, "representatives_equal": old["representatives"] == new["representatives"]}
        for kind in ("ordinary", "close"):
            left, right = old["timing"][kind]["median_ms"], new["timing"][kind]["median_ms"]
            row["timing"][kind] = {"before_ms": left, "after_ms": right, "change_pct": (right / left - 1) * 100}
        for key in sorted(set(old["sql"]) | set(new["sql"])):
            left, right = old["sql"].get(key, {}), new["sql"].get(key, {})
            row["sql_delta"][key] = {k: right.get(k, 0) - left.get(k, 0) for k in sorted(set(left) | set(right))}
        if not row["digests_equal"]:
            raise ValueError("Unexpected functional digest difference: " + case)
        comparisons.append(row)
    output = directory / ("details-" + args.before + "-" + args.after + ".json")
    output.write_text(json.dumps({"phases": phases, "comparisons": comparisons}, indent=2), encoding="utf-8")
    print(str(output))
    print("case | ordinary before/after ms | close before/after ms | private delta bytes | DB/WAL equal")
    for row in comparisons:
        ordinary, close = row["timing"]["ordinary"], row["timing"]["close"]
        print(f'{row["case"]} | {ordinary["before_ms"]:.4f}/{ordinary["after_ms"]:.4f} | '
              f'{close["before_ms"]:.4f}/{close["after_ms"]:.4f} | {row["private_after_delta_bytes"]:+d} | {row["file_sizes_equal"]}')


if __name__ == "__main__":
    main()
