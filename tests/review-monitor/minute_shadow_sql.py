import sqlite3,json
T=10_000_000;D=86400*T;base=134058240000000000;start=base+12*3600*T
c=sqlite3.connect(':memory:')
c.executescript('CREATE TABLE aggregate_window(id INTEGER PRIMARY KEY,program_id,pid,end_utc,version); CREATE INDEX aggregate_program_time ON aggregate_window(program_id,end_utc); CREATE TABLE aggregate_metric(window_id,metric,value,eligible,PRIMARY KEY(window_id,metric));')
rows=[]
for day in range(14):
 for minute in range(1,21):
  for pid in range(32):rows.append((len(rows)+1,1,pid,base-day*D+minute*60*T,1))
c.executemany('INSERT INTO aggregate_window VALUES(?,?,?,?,?)',rows)
c.executemany('INSERT INTO aggregate_metric VALUES(?,0,1,1)',[(r[0],)for r in rows])
old='SELECT m.value,w.end_utc FROM aggregate_window w JOIN aggregate_metric m ON m.window_id=w.id AND m.metric=? AND m.eligible=1 WHERE w.program_id=? AND w.version=1 AND w.end_utc>=? AND w.end_utc<? AND w.end_utc<? AND w.id<=? ORDER BY w.end_utc DESC,w.id DESC LIMIT 64'
selected=[]
for day in range(14):
 lower=base-day*D; selected.extend(c.execute(old,(0,1,lower,lower+D,start,len(rows))))
r={'experiment':'captured pre-fix SQL; current product code is not changed by this script','sqlite_runtime':sqlite3.sqlite_version,'full_eligible_rows':len(rows),'full_distinct_end_minutes':len({r[3]//(60*T)for r in rows}),'selected_rows':len(selected),'selected_distinct_minutes':len({r[1]//(60*T)for r in selected}),'dates':14,'old_dense_bound':'floor(64/32)*14 = 28 < gate60','gate':'learning','expected_after_unique_minute_representatives':280,'start_utc_ticks':start}
assert r['full_distinct_end_minutes']==280 and r['selected_rows']==896 and r['selected_distinct_minutes']==28
print(json.dumps(r,indent=2))
