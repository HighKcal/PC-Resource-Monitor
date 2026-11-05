"""Aggregate only local test outputs; never print program names or paths."""
import argparse
import json
import math
from pathlib import Path
import re
import sqlite3

def dist(a):
    a=sorted(a)
    if not a: return None
    return dict(n=len(a), min=a[0], median=a[math.ceil(len(a)*.5)-1],
                p95=a[math.ceil(len(a)*.95)-1], max=a[-1], mean=sum(a)/len(a))

ap=argparse.ArgumentParser()
ap.add_argument('run',type=Path)
args=ap.parse_args()
run=args.run.resolve()
result=json.loads((run/'result.json').read_text(encoding='utf-8-sig'))
log=(run/'target.log').read_text(encoding='utf-8',errors='replace')
command=result['command']; db=Path(command[command.index('--db')+1])
batches=[]; refresh=[]
for line in log.splitlines():
    if line.startswith(('batch=','ui_refresh ')):
        fields={k:float(v) for k,v in re.findall(r'([a-z_]+)=([0-9]+(?:\.[0-9]+)?)',line)}
        (batches if line.startswith('batch=') else refresh).append(fields)
output=dict(label=result['label'], sha256=result['sha256'],
            batch_count=len(batches), refresh_count=len(refresh),
            render_expected='--headless' not in command,
            missing_render_sequences=None if '--headless' in command else sorted(set(int(b['batch']) for b in batches)-set(int(r['sequence']) for r in refresh)),
            rows=dist([b['rows'] for b in batches]),
            display_rows=dist([r['shown']+r['headers'] for r in refresh]),
            render_ms=dist([r['render_ms'] for r in refresh]),
            exit_lines=[s for s in log.splitlines() if s.startswith('exit ')],
            grouping_errors=sum(b.get('grouping_error',0)!=0 for b in batches),
            phase_processing={})
for phase,predicate in [('ordinary',lambda b:b['windows']==0),('completed',lambda b:b['windows']>0)]:
    rows=[b for b in batches if predicate(b)]
    output['phase_processing'][phase]={field:dist([b[field] for b in rows]) for field in ('collect_ms','store_ms','grouping_ms')}
    output['phase_processing'][phase]['combined_ms']=dist([sum(b[k] for k in ('collect_ms','store_ms','grouping_ms')) for b in rows])
if 'PERF start' in log:
    probe=dict(hooks=[], states=[], samples=[], snapshots=[], window_events=[], counters={}, getobject=0, gettext_sent=0, getobject_sent=0, observer_wall_ms=[])
    for line in log.splitlines():
        if not line.startswith('PERF '): continue
        fields=dict(re.findall(r'([a-z_]+)=([^ ]+)',line))
        if line.startswith('PERF hooks '):
            if fields not in probe['hooks']: probe['hooks'].append(fields)
        elif line.startswith('PERF state '): probe['states'].append(fields)
        elif line.startswith('PERF sample '): probe['samples'].append(fields)
        elif line.startswith('PERF snapshots '): probe['snapshots'].append(fields)
        elif line.startswith('PERF window_events '): probe['window_events'].append(fields)
        elif line.startswith(('PERF lvm ','PERF hdm ')):
            key=line.split()[1]+'/'+fields['id']
            cell=probe['counters'].setdefault(key,dict(count=0,sent=0,during_render=0,during_customdraw=0))
            for field in cell: cell[field]+=int(fields[field])
        elif line.startswith('PERF getobject '):
            probe['getobject']+=int(fields['count']); probe['getobject_sent']+=int(fields['sent'])
        elif line.startswith('PERF gettext '): probe['gettext_sent']+=int(fields['text_sent'])+int(fields['length_sent'])
        elif line.startswith('PERF observer_wall_ms='): probe['observer_wall_ms'].append(float(fields['observer_wall_ms']))
    probe['observer_wall_ms']=dist(probe['observer_wall_ms'])
    probe['native_query_sent_context_total']=sum(c['sent'] for c in probe['counters'].values())
    output['probe']=probe
if 'CACHE_PROBE sample' in log:
    records=[]; costs=[]
    for line in log.splitlines():
        if line.startswith('CACHE_PROBE sample '):
            fields=dict(re.findall(r'([a-z_]+)=([^ ]+)',line))
            records.append({k:(int(v) if re.fullmatch(r'-?[0-9]+',v) else v) for k,v in fields.items()})
        elif line.startswith('CACHE_PROBE observer '):
            costs.append(float(re.search(r'wall_ms=([0-9.]+)',line)[1]))
    stable=[r for r in records if result['warmup_requested']*1000 <= r['uptime_ms'] <= (result['warmup_requested']+result['stable_requested'])*1000]
    fields=('cache_bytes','stmt_bytes','schema_bytes','private_bytes','working_set_bytes','instances')
    all_valid=all(r['cache_rc']==0 and r['stmt_rc']==0 and r['schema_rc']==0 and r['memory_valid']==1 for r in records)
    output['cache_probe']=dict(samples=len(records), all_api_valid=all_valid,
        open=records[0], final=records[-1], stable_first=stable[0] if stable else None,stable_last=stable[-1] if stable else None,
        stable_delta={k:stable[-1][k]-stable[0][k] for k in fields} if stable and all_valid else None,
        sampled_max={k:max(r[k] for r in records) for k in fields} if all_valid else None, observer_wall_ms=dist(costs),
        qualification='Approximate SQLite categories; no exact process-memory reconciliation. Diagnostic, not acceptance CPU.')
if db.exists():
    c=sqlite3.connect(db.as_uri()+'?mode=ro',uri=True)
    scalar=lambda q:c.execute(q).fetchone()[0]
    output['database']=dict(integrity=scalar('PRAGMA integrity_check'), foreign_key_errors=len(c.execute('PRAGMA foreign_key_check').fetchall()),
        unclosed_sessions=scalar('SELECT count(*) FROM session WHERE ended IS NULL'),
        synthetic=scalar('SELECT synthetic FROM metadata'),
        counts={t:scalar('SELECT count(*) FROM '+t) for t in ('collection_run','raw','aggregate_window','aggregate_metric','feedback')},
        run_status=c.execute('SELECT status,count(*) FROM collection_run GROUP BY status').fetchall(),
        logical_cpu_counts=c.execute('SELECT DISTINCT cpu_count FROM collection_run').fetchall(),
        row_count_mismatches=scalar('SELECT count(*) FROM collection_run r WHERE r.row_count != (SELECT count(*) FROM raw x WHERE x.run_id=r.id)'),
        completed_states=c.execute('SELECT state,count(*) FROM aggregate_metric GROUP BY state').fetchall(),
        delta_status=c.execute('SELECT cpu_status,read_status,write_status,count(*) FROM raw GROUP BY cpu_status,read_status,write_status').fetchall(),
        observation_interval_seconds=dist([r[0] for r in c.execute('SELECT observation_interval FROM raw WHERE observation_interval>0')]),
        long_observations=scalar('SELECT count(*) FROM raw WHERE observation_interval>15'))
    times=[r[0] for r in c.execute('SELECT finished FROM collection_run ORDER BY id')]
    output['database']['run_finished_interval_seconds']=dist([(b-a)/1e7 for a,b in zip(times,times[1:])])
    output['database']['completed_runs']=[dict(run_id=r[0], finished_since_process_creation_seconds=(r[1]-result['creation_filetime_100ns'])/1e7,windows=r[2]) for r in c.execute('SELECT r.id,r.finished,count(w.id) FROM collection_run r JOIN aggregate_window w ON w.run_id=r.id GROUP BY r.id ORDER BY r.id')]
    c.close()
(run/'analysis.json').write_text(json.dumps(output,indent=2),encoding='utf-8')
print(json.dumps(output,indent=2))
