"""Keep probe counters separate from uninstrumented product measurements."""
import argparse
import json
from pathlib import Path
import re

def pairs(line): return dict(re.findall(r'(\w+)=([^\s;]+)',line))
def parse(path):
    intervals=[]; current=None
    for line in path.read_text(errors='replace').splitlines():
        if line.startswith('PERF sample '):
            current=dict(sample=pairs(line),getitem_sent=0,getobject=0,lvm_sent=0,hdm_sent=0)
            intervals.append(current)
        elif current is not None:
            if line.startswith('PERF state '): current['state']=pairs(line)
            elif line.startswith('PERF snapshots '): current['snapshots']=pairs(line)
            elif line.startswith('PERF lvm '):
                v=pairs(line); current['lvm_sent']+=int(v['sent'])
                if v['name']=='GETITEMW': current['getitem_sent']+=int(v['sent'])
            elif line.startswith('PERF hdm '): current['hdm_sent']+=int(pairs(line)['sent'])
            elif line.startswith('PERF getobject '): current['getobject']+=int(pairs(line)['count'])
    return intervals

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--root',type=Path,required=True); ap.add_argument('--runs',nargs='+',required=True)
ap.add_argument('--output',type=Path,required=True)
args=ap.parse_args(); result={}
for label in args.runs:
    intervals=parse(args.root/label/'product.log')
    metadata=json.loads((args.root/label/'metadata.json').read_text(encoding='utf-8-sig'))
    ncpu=metadata['logical_cpu_count']
    # Entire intervals must start after 30s; do not include the transition interval.
    stable=[v for v in intervals if float(v['sample']['uptime_ms'])/1000-float(v['sample']['interval_s'])>=30]
    summary={}
    for name,selected in [('all',intervals),('after30',stable)]:
        seconds=sum(float(v['sample']['interval_s']) for v in selected)
        valid=bool(selected) and all(v['sample']['process_cpu_valid']=='1' and v['sample']['ui_cpu_valid']=='1' for v in selected)
        summary[name]=dict(seconds=seconds,
            cpu_valid=valid,
            cpu_percent=sum(float(v['sample']['process_cpu_s']) for v in selected)/seconds/ncpu*100 if valid and seconds else None,
            ui_cpu_seconds=sum(float(v['sample']['ui_cpu_s']) for v in selected),
            render_calls=sum(int(v['sample']['render_calls']) for v in selected),
            render_wall_ms=sum(float(v['sample']['render_wall_ms']) for v in selected),
            **{field:sum(v[field] for v in selected) for field in ['getitem_sent','getobject','lvm_sent','hdm_sent']},
            states=[{k:v.get('state',{}).get(k) for k in ['visible','iconic','foreground','rows','processes']} for v in selected])
    result[label]=dict(intervals=intervals,summary=summary)
    print(label,json.dumps(summary))
args.output.write_text(json.dumps(result,indent=2),encoding='utf-8')
