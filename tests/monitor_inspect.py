"""Read-only inspection of a monitor-created DB and optional actual-run log.

This checks persisted evidence using Python's SQLite; it does not execute or
replace the C engine tests and is not a detector-accuracy evaluation.
"""
import argparse
import json
import pathlib
import re
import sqlite3
import statistics

parser = argparse.ArgumentParser()
parser.add_argument("db", type=pathlib.Path)
parser.add_argument("--log", type=pathlib.Path)
args = parser.parse_args()
path = args.db.resolve(strict=True)
db = sqlite3.connect(path.as_uri() + "?mode=ro", uri=True)
assert db.execute("pragma application_id").fetchone()[0] == 1297042993
assert db.execute("pragma user_version").fetchone()[0] == 1
report = {"db": str(path), "python_sqlite": sqlite3.sqlite_version,
          "synthetic": db.execute("select synthetic from metadata").fetchone()[0],
          "file_bytes": path.stat().st_size,
          "journal_mode": db.execute("pragma journal_mode").fetchone()[0],
          "integrity": db.execute("pragma integrity_check").fetchone()[0],
          "foreign_key_errors": db.execute("pragma foreign_key_check").fetchall()}
for table in ("session", "collection_run", "program", "raw", "aggregate_window", "aggregate_metric", "feedback"):
    report[table + "_rows"] = db.execute("select count(*) from " + table).fetchone()[0]
report["raw_null_and_zero"] = db.execute(
    "select sum(cpu is null),sum(cpu=0),sum(program_id is null),sum(name is not null) from raw"
).fetchone()
report["window_ranges"] = db.execute(
    "select min(duration),max(duration) from aggregate_window").fetchone()
report["metric_state_counts"] = db.execute(
    "select metric,state,count(*) from aggregate_metric group by metric,state").fetchall()
report["baseline_plan"] = db.execute(
    "explain query plan select m.value,w.end_utc from aggregate_window w join aggregate_metric m "
    "on m.window_id=w.id and m.metric=0 and m.eligible=1 where w.program_id=1 and w.version=1 "
    "and w.end_utc>=0 and w.end_utc<999999999999999999 and w.end_utc<999999999999999999 "
    "and w.id<=999999999 order by w.end_utc desc,w.id desc limit 64").fetchall()
try:
    sizes = dict(db.execute("select name,sum(pgsize) from dbstat group by name"))
    report["page_bytes"] = sizes
    raw_bytes = sum(sizes.get(n, 0) for n in ("raw", "raw_time", "raw_run", "raw_program_time"))
    aggregate_bytes = sum(sizes.get(n, 0) for n in
                          ("aggregate_window", "aggregate_time", "aggregate_run", "aggregate_program_time",
                           "aggregate_metric", "metric_baseline"))
    report["raw_page_bytes_per_row_including_indexes"] = raw_bytes / max(1, report["raw_rows"])
    report["window_plus_four_metrics_page_bytes_per_window"] = aggregate_bytes / max(1, report["aggregate_window_rows"])
except sqlite3.OperationalError as error:
    report["dbstat_unavailable"] = str(error)
if args.log:
    text = args.log.read_text(encoding="utf-8-sig")
    samples = re.findall(r"collect_ms=([\d.]+) store_ms=([\d.]+)", text)
    for index, label in enumerate(("collect_ms", "store_ms")):
        values = sorted(float(sample[index]) for sample in samples)
        if values:
            report[label] = {"n": len(values), "min": values[0], "median": statistics.median(values),
                             "p95_nearest_rank": values[max(0, (len(values) * 95 + 99) // 100 - 1)],
                             "max": values[-1]}
db.close()
print(json.dumps(report, ensure_ascii=False, indent=2))
