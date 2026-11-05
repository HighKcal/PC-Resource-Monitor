"""Independent static phase-2 contract checks, not a C parser or runtime proof.

Run only against a frozen candidate. Outputs include complete input hashes so a
passing record never silently applies to later edits. The before root must be
the phase-2 source preservation, not Git HEAD.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

PRODUCT_ROOTS = ("include", "collector", "engine", "storage", "detection", "prototype", "grouping", "gui", "app")
LEX = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|0[xX][\da-fA-F]+|\d+(?:\.\d+)?|\S')
PRIVATE = re.compile(r'^\s*#\s*include\s*[<"]([^>"\n]*(?:monitor_internal|monitor_engine_private|monitor_store_private|storage_internal)\.h)[>"]', re.M)
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"\n]+)[>"]', re.M)
CORE_SQL = re.compile(r'\b(?:FROM|JOIN|INTO|UPDATE|TABLE)\s+(?:IF\s+NOT\s+EXISTS\s+)?["`\[]?(aggregate_window|aggregate_metric|collection_run|program|raw|session|_experiment_pair)\b', re.I)


def tokens(text: str) -> list[str]:
    return [m.group(0) for m in LEX.finditer(text) if not m.group(0).startswith(("//", "/*"))]


def code(text: str) -> str:
    return " ".join(t for t in tokens(text) if not t.startswith(('"', "'")))


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def function(text: str, name: str) -> list[str]:
    ts = tokens(text)
    for i, token in enumerate(ts):
        if token != name or i + 1 >= len(ts) or ts[i + 1] != "(":
            continue
        j = i + 1
        parens = 0
        while j < len(ts):
            if ts[j] == "(":
                parens += 1
            elif ts[j] == ")":
                parens -= 1
                if not parens:
                    break
            j += 1
        if j + 1 == len(ts) or ts[j + 1] != "{":
            continue
        k, braces = j + 1, 0
        while k < len(ts):
            if ts[k] == "{":
                braces += 1
            elif ts[k] == "}":
                braces -= 1
                if not braces:
                    return ts[i:k + 1]
            k += 1
    raise ValueError(f"function definition missing: {name}")


def declarations(text: str) -> dict[str, list[str]]:
    ts = tokens(text)
    found = {}
    for i, token in enumerate(ts):
        if token != "typedef":
            continue
        depth = 0
        for j in range(i + 1, len(ts)):
            depth += (ts[j] == "{") - (ts[j] == "}")
            if ts[j] == ";" and depth == 0:
                name = ts[j - 1]
                if name.startswith("Monitor"):
                    found[name] = ts[i:j + 1]
                break
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root, before = args.root.resolve(), args.before.resolve()
    sources = sorted(p for folder in PRODUCT_ROOTS for p in (root / folder).rglob("*") if p.suffix in (".c", ".h"))
    checks, inputs = [], []

    def check(name: str, failures: list, detail=None):
        checks.append({"name": name, "pass": not failures, "failures": failures, "detail": detail})

    private_outside, store_instance, prototype_core, baseline_coupling = [], [], [], []
    for p in sources:
        rel, text = p.relative_to(root).as_posix(), read(p)
        inputs.append({"path": rel, "sha256": digest(p)})
        c = code(text)
        for match in PRIVATE.finditer(text):
            target = Path(match.group(1).replace("\\", "/")).name
            if target in ("monitor_internal.h", "monitor_engine_private.h") and not rel.startswith("engine/"):
                private_outside.append({"path": rel, "include": match.group(1)})
        if rel.startswith("storage/") and re.search(r"\bMonitorInstance\b", c):
            store_instance.append(rel)
        if rel.startswith("prototype/"):
            problems = [m.group(1) for m in PRIVATE.finditer(text)]
            if re.search(r"\bcore\s*->", c):
                problems.append("dereferences opaque core")
            sql = " ".join(t[1:-1] for t in tokens(text) if t.startswith('"'))
            problems.extend("core SQL relation: " + m.group(1) for m in CORE_SQL.finditer(sql))
            if problems:
                prototype_core.append({"path": rel, "problems": problems})
        if rel in ("detection/baseline.c", "detection/baseline.h"):
            problems = re.findall(r"\b(?:sqlite3\w*|MonitorEngine|MonitorInstance)\b", c)
            problems.extend(m.group(1) for m in PRIVATE.finditer(text))
            if problems:
                baseline_coupling.append({"path": rel, "problems": sorted(set(problems))})

    check("no engine-private consumers outside engine product TUs", private_outside)
    check("store has no MonitorInstance contract", store_instance)
    check("prototype has no engine/store-private or core SQL access", prototype_core)
    check("Baseline policy has no SQLite or engine state access", baseline_coupling)

    expected = ("engine/monitor_window.c", "engine/monitor_window.h", "storage/monitor_history.c", "detection/baseline_history.c", "detection/monitor_policy.h", "prototype/experiment_capture.c", "prototype/experiment_reducer.c", "prototype/experiment_history.c", "prototype/experiment_replay.c", "prototype/experiment_store.c")
    check("requested responsibilities have compiled-source candidates", [p for p in expected if not (root / p).is_file()], "Names are the reviewed contract; presence alone is not acceptance.")

    old_decl = declarations(read(before / "include/monitor.h"))
    new_text = read(root / "include/monitor.h")
    if (root / "include/monitor_types.h").is_file():
        new_text += "\n" + read(root / "include/monitor_types.h")
    new_decl = declarations(new_text)
    check("public Monitor typedef declarations unchanged", [name for name, value in old_decl.items() if new_decl.get(name) != value], sorted(old_decl))
    fixed = ("include/resource_collector.h", "include/experiment.h", "storage/schema.sql", "storage/monitor_schema.h", "prototype/experiment_retention.c", "prototype/experiment_retention.h", "gui/monitor_gui.c")
    check("public/schema/phase1 preserved files unchanged", [name for name in fixed if digest(root / name) != digest(before / name)])

    oracle = "tests/review-performance/test_baseline_oracle.c"
    old_oracle, new_oracle = read(before / oracle), read(root / oracle)
    check("independent Baseline result oracle preserved", [name for name in ("reference", "middle", "compare_double", "check_report") if function(old_oracle, name) != function(new_oracle, name)], "Fixture-access changes are permitted; expected result math/query is not shared with product.")
    inputs.append({"path": oracle, "sha256": digest(root / oracle)})
    check("test-only fixtures own private access", [str(p.relative_to(root)).replace("\\", "/") for p in (root / "tests").rglob("*.c") if p.parent != root / "tests/support" and PRIVATE.search(read(p))], "Legacy archived reproducers may require an explicit, versioned exclusion rather than silent pass.")

    report = {"kind": "static contract checks only", "root": str(root), "before": str(before), "checks": checks, "pass": all(c["pass"] for c in checks), "inputs": inputs}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({"pass": report["pass"], "checks": len(checks), "failed": [c["name"] for c in checks if not c["pass"]], "output": str(args.output)}, ensure_ascii=False))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
