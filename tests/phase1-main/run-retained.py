"""Serial, fresh-copy retention cost comparison. Run only with the main execution slot."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--before", required=True, type=Path)
    parser.add_argument("--after", required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    if out.exists():
        parser.error("fresh output required")
    out.mkdir(parents=True)
    source = args.source.resolve()
    executables = {"before": args.before.resolve(), "after": args.after.resolve()}
    inputs = {str(p): digest(p) for p in [source, *executables.values()]}
    record = {"inputs": inputs, "commands": [], "status": "running"}

    def run(name, command, profiled=False):
        env = os.environ.copy()
        if profiled:
            env.pop("PHASE1_NO_PROFILE", None)
        else:
            env["PHASE1_NO_PROFILE"] = "1"
        command = [str(x) for x in command]
        log = out / (name + ".log")
        started = time.time()
        print(name, flush=True)
        with log.open("w", encoding="utf-8") as handle:
            child = subprocess.run(command, cwd=out, env=env, stdout=handle, stderr=subprocess.STDOUT)
        record["commands"].append({"name": name, "command": command, "profiled": profiled, "log": str(log), "exit_code": child.returncode, "seconds": time.time() - started})
        if child.returncode:
            raise RuntimeError(name + " failed")

    try:
        fixture = out / "fixture.sqlite"
        run("seed", [executables["before"], "seed", source, fixture, "15000"])
        record["fixture_sha256"] = digest(fixture)
        # AB then BA uses fresh copies of exactly one stopped synthetic fixture.
        for cycle, order in enumerate((("before", "after"), ("after", "before")), 1):
            for phase in order:
                for mode in ("stats", "trim-retained", "trim-expired"):
                    name = f"round{cycle}-{phase}-{mode}"
                    run(name, [executables[phase], "measure", fixture, out / (name + ".sqlite"), mode])
        for phase in executables:
            for mode in ("trim-retained", "trim-expired"):
                name = f"profile-{phase}-{mode}"
                run(name, [executables[phase], "measure", fixture, out / (name + ".sqlite"), mode], True)
        if digest(fixture) != record["fixture_sha256"] or any(digest(Path(p)) != h for p, h in inputs.items()):
            raise RuntimeError("input changed during run")
        record["status"] = "passed"
    except Exception as exc:
        record["status"] = "failed"
        record["error"] = str(exc)
        raise
    finally:
        (out / "result.json").write_text(json.dumps(record, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
