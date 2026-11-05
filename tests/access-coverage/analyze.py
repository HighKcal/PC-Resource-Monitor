"""Analyze local product evidence, retaining NA and exact PID+creation joins."""
import argparse
from collections import Counter
import json
import math
from pathlib import Path
import re
import sqlite3
import statistics

FLAGS={'handle_name':1,'identity':2,'cpu':4,'working_set':8,'read':16,'write':32}
STATUS={0:'valid',1:'first_sample',2:'identity_unavailable',3:'identity_changed',4:'query_failed_current_or_previous',5:'clock_invalid',6:'long_gap',7:'counter_rollback'}

def distribution(values):
    v=sorted(x for x in values if x is not None and math.isfinite(x))
    return dict(n=len(v),min=v[0],median=statistics.median(v),p95=v[math.ceil(.95*len(v))-1],max=v[-1],mean=sum(v)/len(v)) if v else None

def reason(row, field):
    if field=='display_name' and row['display_source']: return 'success'
    if field in FLAGS and row['valid']&FLAGS[field]: return 'success'
    if field=='path' and row['program_id'] is not None: return 'success_verified_or_same_instance_cached'
    if field=='path' and not row['valid']&2: return 'not_attempted_identity_unavailable'
    if row['open_error']: return 'not_attempted_open_access_denied' if row['open_error']==5 else 'not_attempted_open_error_'+str(row['open_error'])
    error_col={'handle_name':'name_error','display_name':'name_error','identity':'times_error','cpu':'times_error','working_set':'memory_error','read':'io_error','write':'io_error','path':'path_error'}[field]
    err=row[error_col]
    if err: return ('api_access_denied' if err==5 else 'api_error_'+str(err))+('_exit_later_observed' if row.get('exit_observed') else '')
    if field in ('cpu','read','write'):
        return STATUS.get(row[{'cpu':'cpu_status','read':'read_status','write':'write_status'}[field]],'unknown')
    if field=='identity': return 'zero_creation_or_identity_unconfirmed'
    return 'unavailable_without_api_error'

def aggregate(rows):
    n=len(rows)
    success={name:sum(bool(r['valid']&bit) for r in rows) for name,bit in FLAGS.items()}
    success.update(display_name=sum(bool(r['display_source']) for r in rows),
                   path=sum(r['program_id'] is not None for r in rows),
                   list_name_only=sum(r['display_source']==2 and not(r['valid']&60) for r in rows),
                   list_name_with_any_metric=sum(r['display_source']==2 and bool(r['valid']&60) for r in rows),
                   list_name=sum(r['display_source']==2 for r in rows),
                   all_metrics=sum((r['valid']&60)==60 for r in rows),
                   identity_path_for_comparison=sum(bool(r['valid']&2) and r['program_id'] is not None for r in rows))
    reasons={field:dict(Counter(reason(row,field) for row in rows)) for field in (*FLAGS,'display_name','path')}
    return dict(discovered_rows=n,success=success,rates={key:value/n if n else None for key,value in success.items()},
                reasons=reasons,open_errors=dict(Counter(r['open_error'] for r in rows)),
                attempted_api_errors={f:dict(Counter(r[f] for r in rows if not r['open_error'] and r[f])) for f in ('times_error','memory_error','io_error','name_error')},
                special_rows=[{k:r[k] for k in ('run_id','pid','valid','open_error','display_source','path_error')} for r in rows if r['pid'] in (0,4)],
                intervals={f:distribution([r[f] for r in rows]) for f in ('observation_interval','cpu_interval','io_interval')})

def read_run(folder):
    metadata=json.loads((folder/'metadata.json').read_text(encoding='utf-8-sig'))
    summary=json.loads((folder/'summary.json').read_text(encoding='utf-8-sig'))
    assert summary['exit_code']==0 and summary['token_matches_expected']
    assert summary['db']['integrity']==['ok'] and not summary['db']['foreign_key_errors']
    coverage={}; batches=[]
    for line in (folder/'product.log').read_text(encoding='utf-8',errors='replace').splitlines():
        if line.startswith('coverage '):
            v=dict(re.findall(r'(\w+)=([^\s]+)',line))
            coverage[(int(v['batch']),int(v['row']))]=v
        if line.startswith('batch='):
            v=dict(re.findall(r'(\w+)=([^\s]+)',line))
            batches.append({k:float(v[k]) for k in ('batch','rows','windows','collect_ms','store_ms','grouping_ms')})
    db=sqlite3.connect('file:'+ (folder/'fresh.sqlite').as_posix()+'?mode=ro',uri=True)
    db.row_factory=sqlite3.Row
    rows=[dict(row) for row in db.execute('SELECT * FROM raw ORDER BY run_id,id')]
    runs=[dict(row) for row in db.execute('SELECT * FROM collection_run ORDER BY id')]
    run_index={run['id']:i+1 for i,run in enumerate(runs)}
    index=Counter()
    for row in rows:
        key=(run_index[row['run_id']],index[row['run_id']]); index[row['run_id']]+=1
        row['display_source']=1 if row['valid']&1 else 0
        if coverage:
            v=coverage[key]
            assert int(v['pid'])==row['pid'] and int(v['measured'])==row['utc'] and int(v['valid'])==row['valid']
            assert int(v['creation'])==(row['creation'] or 0)
            assert int(v['metadata'])==1
            assert int(v['path_valid'])==(row['program_id'] is not None)
            assert (int(v['attempted'])&15)==(0 if row['open_error'] else 15)
            row['display_source']=int(v['display_source']); row['exit_observed']=int(v['exit_observed'])
            row['comparison']=[int(x) for x in v['comparison'].split(',')]
    if coverage: assert len(coverage)==len(rows)
    assert all(index[run['id']]==run['row_count'] for run in runs)
    last=[r for r in rows if r['run_id']==runs[-1]['id']]
    report=dict(metadata=metadata,external_summary=summary,run_count=len(runs),coverage_metadata_rows=len(coverage),
                first=aggregate([r for r in rows if r['run_id']==runs[0]['id']]),
                post_first=aggregate([r for r in rows if r['run_id']!=runs[0]['id']]),last=aggregate(last),
                metrics_by_completed_state=[dict(r) for r in db.execute('SELECT metric,state,reason,count(*) AS count FROM aggregate_metric GROUP BY metric,state,reason')],
                sessions=[dict(r) for r in db.execute('SELECT * FROM session')],
                product_cost={kind:{field:distribution([b[field] for b in batches if (kind=='all' or bool(b['windows'])==(kind=='window_completed'))]) for field in ('collect_ms','store_ms','grouping_ms')} for kind in ('all','window_completed','ordinary')})
    if coverage:
        report['last_comparison_states']=[dict(Counter(r['comparison'][m] for r in last)) for m in range(4)]
    db.close()
    return report,last

def compare(left,right):
    def identity(row): return (row['pid'],row['creation']) if row['valid']&2 and row['creation'] else None
    a={identity(r):r for r in left if identity(r)}; b={identity(r):r for r in right if identity(r)}
    common=a.keys()&b.keys()
    def ok(row,field):
        if field=='display_name': return bool(row['display_source'])
        if field=='path': return row['program_id'] is not None
        return bool(row['valid']&FLAGS[field])
    transitions={field:dict(Counter(('success' if ok(a[key],field) else 'failure')+'->'+('success' if ok(b[key],field) else 'failure') for key in common)) for field in (*FLAGS,'display_name','path')}
    return dict(common_verified_instances=len(common),left_verified_only=len(a.keys()-b.keys()),right_verified_only=len(b.keys()-a.keys()),
                left_identity_unknown=sum(not identity(r) for r in left),right_identity_unknown=sum(not identity(r) for r in right),
                field_transitions=transitions,note='No PID-only joins. Identity-unknown access-denied rows cannot be classified as the same instance recovered.')

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root',type=Path,required=True)
    ap.add_argument('--runs',nargs='+',required=True)
    ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args(); reports={}; last={}
    for name in args.runs: reports[name],last[name]=read_run(args.root/name)
    comparisons={a+'__'+b:compare(last[a],last[b]) for i,a in enumerate(args.runs) for b in args.runs[i+1:]}
    args.output.write_text(json.dumps(dict(runs=reports,comparisons=comparisons),indent=2),encoding='utf-8')
    for name,r in reports.items():
        print(name,json.dumps(dict(last=r['last']['success'],n=r['last']['discovered_rows'],postfirst=r['post_first']['success'])))
    print('Saved',args.output)

if __name__=='__main__': main()
