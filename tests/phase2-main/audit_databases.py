"""Read-only audit of stopped synthetic benchmark DBs, including non-FK references."""
import argparse
import hashlib
import json
from pathlib import Path
import sqlite3


def audit(path):
    db = sqlite3.connect(path.as_uri() + "?mode=ro", uri=True)
    try:
        app = db.execute("PRAGMA application_id").fetchone()[0]
        result = {"path": str(path), "bytes": path.stat().st_size,
                  "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "application_id": app,
                  "integrity": [row[0] for row in db.execute("PRAGMA integrity_check")],
                  "foreign_key_errors": list(db.execute("PRAGMA foreign_key_check"))}
        if app == 1297042993:
            result["synthetic"] = db.execute("SELECT synthetic FROM metadata").fetchone()[0]
            result["open_sessions"] = db.execute("SELECT count(*) FROM session WHERE ended IS NULL").fetchone()[0]
            result["logical_errors"] = {}
        elif app == 1163415601:
            result["synthetic"] = db.execute("SELECT synthetic FROM experiment").fetchone()[0]
            # Episode activation/last decision numbers are intentionally expirable
            # summary identifiers, not live references. Do not flag them as orphans.
            queries = {
                "decision_episode": "SELECT count(*) FROM decision d WHERE d.episode_id IS NOT NULL AND NOT EXISTS(SELECT 1 FROM episode e WHERE e.id=d.episode_id)",
                "reduction_episode": "SELECT count(*) FROM reduction r WHERE r.episode<>0 AND NOT EXISTS(SELECT 1 FROM episode e WHERE e.id=r.episode)",
                "evaluation": "SELECT count(*) FROM evaluation v WHERE (v.episode IS NOT NULL AND NOT EXISTS(SELECT 1 FROM episode e WHERE e.id=v.episode)) OR (v.decision IS NOT NULL AND NOT EXISTS(SELECT 1 FROM decision d WHERE d.id=v.decision))",
            }
            result["logical_errors"] = {name: db.execute(sql).fetchone()[0] for name, sql in queries.items()}
        else:
            raise ValueError("Unknown benchmark DB type: " + str(path))
        result["passed"] = (result["integrity"] == ["ok"] and not result["foreign_key_errors"]
                            and result["synthetic"] == 1 and not result.get("open_sessions", 0)
                            and not any(result["logical_errors"].values()))
        return result
    finally:
        db.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--pattern", required=True, help="Explicit stopped synthetic DB filename glob")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("fresh audit output required")
    paths = sorted(args.directory.resolve().glob(args.pattern))
    if not paths or any(path.suffix != ".sqlite" for path in paths):
        parser.error("pattern must select one or more .sqlite files only")
    records = []
    for path in paths:
        print("AUDIT " + path.name, flush=True)
        records.append(audit(path))
    result = {"python_sqlite_version": sqlite3.sqlite_version, "files": records,
              "passed": all(record["passed"] for record in records)}
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"files": len(records), "passed": result["passed"]}))
    if not result["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
