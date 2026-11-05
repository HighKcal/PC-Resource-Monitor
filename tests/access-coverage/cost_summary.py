"""Summarize only completed ABBA runs; separate warmup and window-close work."""
import argparse
import json
from pathlib import Path
import re
import sqlite3
import analyze
import measure

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--root',required=True,type=Path)
ap.add_argument('--output',required=True,type=Path)
ap.add_argument('--runs',nargs='+')
args=ap.parse_args()
result={}
for label in (args.runs or ['cost-before-1','cost-after-1','cost-after-2','cost-before-2']):
    folder=args.root/label
    metadata=json.loads((folder/'metadata.json').read_text())
    summary=json.loads((folder/'summary.json').read_text())
    assert summary['exit_code']==0 and summary['token_matches_expected']
    assert summary['db']['integrity']==['ok'] and not summary['db']['foreign_key_errors']
    samples=[json.loads(line) for line in (folder/'samples.jsonl').read_text().splitlines()]
    stable=[s for s in samples if s['t']>=12]
    batches=[]
    for line in (folder/'product.log').read_text(errors='replace').splitlines():
        if line.startswith('batch='):
            values=dict(re.findall(r'(\w+)=([^\s]+)',line))
            batches.append({key:float(values[key]) for key in ['batch','rows','windows','collect_ms','store_ms','grouping_ms']})
    db=sqlite3.connect('file:'+(folder/'fresh.sqlite').as_posix()+'?mode=ro',uri=True)
    intervals=[r[0] for r in db.execute('SELECT observation_interval FROM raw WHERE observation_interval IS NOT NULL')]
    raw=db.execute('SELECT count(*) FROM raw').fetchone()[0]
    pending=db.execute('SELECT count(*) FROM session WHERE ended IS NULL').fetchone()[0]
    assert not pending
    db.close()
    result[label]=dict(exe_sha256=metadata['exe_sha256'],token=metadata['child_token'],
                       whole=summary['samples'],stable_after_12s=measure.perf.summarize(stable,metadata['logical_cpu_count']),
                       sampler_cpu_seconds=summary['sampler_cpu_seconds'],rows=raw,
                       row_count=analyze.distribution([b['rows'] for b in batches]),
                       actual_observation_seconds=analyze.distribution(intervals),
                       long_gap_count=sum(v>15 for v in intervals),
                       cost={kind:{field:analyze.distribution([b[field] for b in batches if kind=='all' or bool(b['windows'])==(kind=='window_completed')]) for field in ['collect_ms','store_ms','grouping_ms']} for kind in ['all','ordinary','window_completed']})
args.output.write_text(json.dumps(result,indent=2),encoding='utf-8')
for name,r in result.items():
    s=r['stable_after_12s']
    print(json.dumps(dict(run=name,cpu_percent=s['cpu_percent'],seconds=s['wall_seconds'],
                         working_set=s['memory']['working_set_bytes'],private=s['memory']['private_bytes'],
                         collect=r['cost']['ordinary']['collect_ms'],interval=r['actual_observation_seconds'])))
