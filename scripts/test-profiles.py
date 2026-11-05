"""Explicit serial build/regression profiles. Never launches the product GUI.
Run python scripts/test-profiles.py --help for profile and fresh-output options.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
PROFILES = ("build", "basic", "policy", "failure", "prototype", "gui", "all")


def fingerprint():
    result = {}
    for folder in ("app", "collector", "detection", "engine", "grouping", "gui", "include", "prototype", "storage", "scripts", "tests"):
        for path in sorted((ROOT / folder).rglob("*")):
            if path.is_file() and path.suffix.lower() in (".c", ".h", ".sql", ".manifest", ".cmd", ".py", ".ps1", ".txt"):
                result[path.relative_to(ROOT).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    for name in ("sqlite3.c", "sqlite3.h"):
        sqlite = ROOT / "third_party/sqlite" / name
        result[sqlite.relative_to(ROOT).as_posix()] = hashlib.sha256(sqlite.read_bytes()).hexdigest()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--output", type=Path, required=True, help="Fresh directory; existing paths are rejected")
    parser.add_argument("--built-from", type=Path, help="Reuse this runner's build profile output, with matching source hashes")
    args = parser.parse_args()
    out = args.output.resolve()
    if out.exists():
        parser.error("--output must not already exist (previous evidence is preserved)")
    initial = fingerprint()
    if args.built_from:
        build = args.built_from.resolve()
        saved = json.loads((build / "result.json").read_text(encoding="utf-8"))
        if saved.get("status") != "passed" or saved.get("profile") != "build":
            parser.error("--built-from must name a successful build profile output")
        if json.loads((build / "inputs.json").read_text(encoding="utf-8")) != initial:
            parser.error("--built-from inputs differ from the current tree")
        for rel, digest in json.loads((build / "artifacts.json").read_text(encoding="utf-8")).items():
            artifact = build / rel
            if not artifact.is_file() or hashlib.sha256(artifact.read_bytes()).hexdigest() != digest:
                parser.error("--built-from artifact hash mismatch: " + rel)
    else:
        build = out
    out.mkdir(parents=True)
    (out / "logs").mkdir()
    (out / "inputs.json").write_text(json.dumps(initial, indent=2), encoding="utf-8")
    records = []
    result = {"profile": args.profile, "output": str(out), "build_root": str(build), "status": "running", "commands": records}
    coverage = {}
    result["coverage"] = coverage
    started = time.time()

    def invoke(name, command, cwd=ROOT, extra_env=None):
        env = os.environ.copy()
        env.pop("REVIEW_BASELINE_OOM", None)
        if extra_env:
            env.update(extra_env)
        log = out / "logs" / (name + ".log")
        print(name, flush=True)
        stamp = time.time()
        with log.open("w", encoding="utf-8") as handle:
            process = subprocess.run(command, cwd=cwd, env=env, stdout=handle, stderr=subprocess.STDOUT)
        records.append({"name": name, "command": command, "cwd": str(cwd), "log": str(log), "environment_overrides": extra_env or {}, "exit_code": process.returncode, "seconds": time.time() - stamp})
        if process.returncode:
            raise RuntimeError(f"{name} failed ({process.returncode}); see {log}")
        return log.read_text(encoding="utf-8", errors="replace")

    def batch(name, relative, *params):
        # cmd.exe requires a second pair of quotes around a quoted command path.
        script = str(ROOT / relative)
        command = f'"{os.environ.get("COMSPEC", "cmd.exe")}" /d /s /c ""{script}" {subprocess.list2cmdline([str(p) for p in params])}"'
        return invoke("build-" + name, command)

    def case(name, executable, *params, extra_env=None):
        work = out / "cases" / name
        work.mkdir(parents=True)
        for folder in ("build-review-monitor", "build-review-prototype", "tmp"):
            (work / folder).mkdir()
        env = {"TEMP": str(work / "tmp"), "TMP": str(work / "tmp")}
        env.update(extra_env or {})
        return invoke(name, [str(build / executable), *map(str, params)], work, env)

    try:
        if not args.built_from:
            batch("collector", "scripts/build-collector.cmd", build / "collector")
            batch("monitor", "scripts/build-monitor.cmd", build / "monitor")
            batch("prototype", "scripts/build-prototype.cmd", build / "prototype", build / "monitor")
            batch("monitor-review", "tests/review-monitor/build-review.cmd", build / "monitor-review", "tests/phase1-build/test_calloc_hits.c")
            batch("prototype-review", "tests/review-prototype/build-review.cmd", build / "prototype-review")
            batch("oracle", "tests/review-performance/build-review.cmd", build / "oracle", build / "monitor", ROOT)
            batch("oracle-oom", "tests/review-performance/build-review.cmd", build / "oracle-oom", build / "monitor", ROOT, "inject")
            batch("refactor", "tests/refactor-review/build-review.cmd", build / "refactor", ROOT, build / "monitor", build / "prototype")
            batch("gui", "tests/review-admin-gui/build.cmd", build / "gui", build / "monitor/monitor.lib", build / "monitor/grouping.lib")
            batch("gui-focused", "tests/phase1-gui/build.cmd", build / "gui-focused", build / "monitor/monitor.lib", build / "monitor/grouping.lib")
            batch("retention", "tests/phase1-retention/build.cmd", build / "retention", ROOT, build / "monitor/monitor.lib")
            batch("retention-module", "tests/phase1-retention/build-module.cmd", build / "retention-module", ROOT)
            batch("benchmarks", "scripts/build-benchmarks.cmd", build / "monitor/monitor.lib", build / "benchmarks", ROOT)
            batch("store-open", "tests/phase2-core/build-open.cmd", build / "store-open", build / "monitor")
            batch("source-contract", "tests/phase2-review/build-source.cmd", build / "source-contract", build / "monitor/monitor.lib")
            batch("history-scope", "tests/phase2-review/build-history.cmd", build / "history-scope", build / "monitor/monitor.lib")
            batch("window", "tests/phase2-review/build-window.cmd", build / "window", ROOT)
            batch("layout", "tests/phase2-review/build-layout.cmd", build / "layout", ROOT)
            batch("duplicate", "tests/phase2-review/build-duplicate.cmd", build / "duplicate", build / "monitor/monitor.lib", ROOT)
            batch("prototype-modules", "tests/prototype/build-module-failures.cmd", build / "prototype-modules", build / "monitor/monitor.lib")
            batch("prototype-source", "tests/prototype/build-source-failures.cmd", build / "prototype-source", build / "monitor/monitor.lib")
            batch("retention-backlog", "tests/phase1-review/build-retention.cmd", build / "retention-backlog", build / "monitor/monitor.lib", ROOT)
        selected = set(PROFILES[1:-1]) if args.profile == "all" else {args.profile}
        if "basic" in selected:
            for folder, executable in (("collector", "test_core"), ("collector", "test_windows"), ("collector", "test_display"), ("monitor", "test_monitor"), ("monitor", "test_grouping"), ("prototype", "test_experiment")):
                case("basic-" + executable, f"{folder}/{executable}.exe")
            case("window-standalone", "window/test_window_standalone.exe")
        if "policy" in selected:
            layout = case("public-layout", "layout/test_public_layout.exe")
            expected_layout = (ROOT / "tests/phase2-core/public-layout-win64.txt").read_text(encoding="utf-8")
            if layout.splitlines() != expected_layout.splitlines():
                raise RuntimeError("public ABI layout differs from frozen pre-phase2 Win64 output")
            text = case("history-scope", "history-scope/test_history_scope.exe", ".")
            if "oom_once=1 fallback_queries=3" not in text or not re.search(r"partial_rows=[1-9][0-9]* interrupted=1", text):
                raise RuntimeError("history scope partial-read/OOM fallback reachability missing")
            case("minute-shadow", "monitor-review/test_independent.exe", "--minute-shadow", "--expect-ready")
            case("minute-selection", "monitor-review/test_independent.exe", "--minute-selection")
            case("baseline-oracle", "oracle/test_baseline_oracle.exe", ".")
        if "failure" in selected:
            text = case("store-open-oom", "store-open/test_store_open.exe", ".")
            if "STORE_OPEN_OOM hits=1 out_null=1 engine_outstanding=0 db_created=0" not in text or "POLICY_SQL version=1 day_cap=64 history_days=14 cache_entries=4096 constants_match=1" not in text:
                raise RuntimeError("store open allocation failure/policy SQL invariant evidence missing")
            text = case("source-contract", "source-contract/test_source_contract.exe", ".")
            for marker in ("SOURCE_EPOCH last_max_ref=valid saturated_core_commits=2 zero_refs=2 reset_keeps_disabled=1 wrap=0", "source_oom_hits=1 outstanding=0", "prepare_denied=1", "partial_rows=1 interrupted=1"):
                if marker not in text:
                    raise RuntimeError("committed source failure/epoch reachability missing: " + marker)
            text = case("engine-storage-calloc", "monitor-review/test_independent.exe")
            if not re.search(r"PHASE1_CALLOC injected_hits=[1-9][0-9]*", text):
                raise RuntimeError("engine/storage calloc failpoint was not reached")
            text = case("string-oom", "refactor/test_failure_boundaries.exe", ".")
            if len(re.findall(r"STRING_FAILURE .*rollback_reset_resume=ok", text)) != 12 or "outstanding_engine_storage_malloc=0" not in text:
                raise RuntimeError("string OOM reachability/ownership evidence missing")
            text = case("baseline-cache-oom", "oracle-oom/test_baseline_oracle.exe", ".", extra_env={"REVIEW_BASELINE_OOM": "1"})
            if not re.search(r"injected_allocations=[1-9][0-9]*", text):
                raise RuntimeError("baseline cache allocator failpoint was not reached")
        if "prototype" in selected:
            case("duplicate-legacy", "duplicate/test_duplicate_identity.exe", ".")
            case("prototype-basic", "prototype/test_experiment.exe")
            case("prototype-independent", "prototype-review/test_independent.exe")
        if selected.intersection({"prototype", "failure"}):
            text = case("prototype-module-failures", "prototype-modules/test_module_failures.exe", ".")
            if "MODULE_ALLOCATOR_HITS coordinator=1 store=2 history=1 replay=1" not in text or len(re.findall(r"MODULE_SQL_HITS table=\w+ insert_denials=1 history_read_denials=1", text)) != 6:
                raise RuntimeError("prototype module allocator/SQL reachability missing")
            text = case("prototype-source-failures", "prototype-source/test_source_failures.exe", ".")
            allocation = re.search(r"PROTOTYPE_SOURCE_HITS allocation=2 prepare=1 step=1 sidecar_copy=1 allocations=(\d+) frees=(\d+)", text)
            if not allocation or allocation.group(1) != allocation.group(2):
                raise RuntimeError("prototype source partial-commit allocation/SQL/free evidence missing")
            case("retention-episode-backlog", "retention-backlog/test_retention_episode_backlog.exe", ".")
            text = case("retention-fixed", "retention/test_retention.exe", ".", "--expect-fixed")
            stages = [int(stage) for stage in re.findall(r"FAILURE_BOUNDARY stage=(\d+) rollback=ok core_commit=retained", text)]
            if sorted(stages) != list(range(6)):
                raise RuntimeError("retention prepare/step failure-boundary scenarios missing or duplicated")
            coverage["retention_integration"] = {"prepare_scenarios": len([stage for stage in stages if stage < 5]), "step_abort_scenarios": stages.count(5), "stages": stages}
            text = case("retention-module", "retention-module/test_retention_module.exe")
            stages = re.findall(r"MODULE_FAILURE stage=(prepare|step) error_written=1 statements_finalized=1 caller_rollback=ok", text)
            if sorted(stages) != ["prepare", "step"] or "MODULE_OWNERSHIP borrowed_connection_statement=preserved caller_transaction=preserved bounded_reference_cleanup=ok" not in text:
                raise RuntimeError("standalone retention ownership/prepare/step coverage missing or duplicated")
            coverage["retention_module"] = {"prepare_scenarios": stages.count("prepare"), "step_abort_scenarios": stages.count("step"), "no_monitor_library": True}
        if "gui" in selected:
            case("gui-hidden", "gui/test_admin_gui.exe")
            case("gui-feedback", "gui-focused/test_feedback.exe")
            case("gui-feedback-db", "gui-focused/test_feedback_db.exe", ".")
        if fingerprint() != initial:
            raise RuntimeError("build/test inputs changed during execution; evidence is version-inconsistent")
        result["status"] = "passed"
    except Exception as exc:
        result["status"] = "failed"
        result["error"] = str(exc)
        print(str(exc), file=sys.stderr)
    finally:
        if not args.built_from:
            artifacts = {p.relative_to(build).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sorted(build.rglob("*")) if p.is_file() and p.suffix.lower() in (".lib", ".obj", ".exe", ".manifest")}
            (out / "artifacts.json").write_text(json.dumps(artifacts, indent=2), encoding="utf-8")
        result["seconds"] = time.time() - started
        (out / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
