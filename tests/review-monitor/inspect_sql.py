import sqlite3, pathlib, time, json
p=pathlib.Path('build-review-monitor/independent.sqlite')
c=sqlite3.connect(f'file:{p.as_posix()}?mode=ro',uri=True)
q='SELECT m.value,w.end_utc FROM aggregate_window w JOIN aggregate_metric m ON m.window_id=w.id AND m.metric=? AND m.eligible=1 WHERE w.program_id=? AND w.version=1 AND w.end_utc>=? AND w.end_utc<? AND w.end_utc<? AND w.id<=? ORDER BY w.end_utc DESC,w.id DESC LIMIT 64'
args=(0,1,134058240000000000,134059104000000000,134058312000000000,9999999)
r={'sqlite_python_version':sqlite3.sqlite_version,'file_bytes':p.stat().st_size,'counts':{t:c.execute('SELECT count(*) FROM '+t).fetchone()[0] for t in ('raw','aggregate_window','aggregate_metric','collection_run')},'query_plan':list(c.execute('EXPLAIN QUERY PLAN '+q,args))}
t=time.perf_counter()
for i in range(1000):list(c.execute(q,args))
r['1000_small_fixture_queries_ms']=(time.perf_counter()-t)*1000
print(json.dumps(r,indent=2))
