"""Observe an already user-approved GUI by exact retained PID+creation+image.

This bounded observation does not launch/elevate/control/terminate a target. No
UIA, screenshot, text or input APIs are used. It cannot prove external UIA absence.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import importlib.util
import json
from pathlib import Path
import sqlite3
import time

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('admin_run',ROOT/'tests/admin-gui/run.py')
r=importlib.util.module_from_spec(spec); spec.loader.exec_module(r)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--launch',type=Path,required=True); ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--warmup',type=int,default=30); ap.add_argument('--seconds',type=int,default=180)
    args=ap.parse_args()
    if args.warmup<0 or args.seconds<3: ap.error('warmup must be nonnegative; seconds must be at least 3')
    args.out.mkdir(parents=True,exist_ok=False)
    record=json.loads(args.launch.read_text(encoding='utf-8'))
    exe=Path(record['exe']).resolve(strict=True); pid=record['pid']; creation=record['creation_filetime_100ns']
    if r.sha(exe)!=record['exe_sha256']: raise RuntimeError('Executable changed')
    handle=r.k.OpenProcess(0x1000|0x00100000,False,pid)
    if not handle: raise C.WinError(C.get_last_error())
    try:
        if r.perf.times(handle)[0]!=creation: raise RuntimeError('PID reused')
        token=r.measure.token_info(handle); r.admin(token)
        r.k.QueryFullProcessImageNameW.argtypes=[W.HANDLE,W.DWORD,W.LPWSTR,C.POINTER(W.DWORD)]
        image=C.create_unicode_buffer(32768); size=W.DWORD(len(image))
        if not r.k.QueryFullProcessImageNameW(handle,0,image,C.byref(size)): raise C.WinError(C.get_last_error())
        if Path(image.value).resolve()!=exe: raise RuntimeError('Image differs')
        if r.k.WaitForSingleObject(handle,0)!=258: raise RuntimeError('Target already exited')
        db=Path(record['arguments'][record['arguments'].index('--db')+1])
        connection=sqlite3.connect('file:'+db.as_posix()+'?mode=ro',uri=True)
        maxrow=connection.execute('SELECT coalesce(max(id),0) FROM raw').fetchone()[0]
        connection.close()
        meta=dict(started_utc=r.utc(),pid=pid,creation_filetime_100ns=creation,exe=str(exe),exe_sha256=r.sha(exe),
                  sampler_sha256=r.sha(__file__),child_token=token,parent_token=r.measure.token_info(r.k.GetCurrentProcess()),
                  db=str(db),start_max_raw_id=maxrow,warmup_seconds=args.warmup,stable_seconds=args.seconds,
                  logical_cpu_count=r.k.GetActiveProcessorCount(0xffff),
                  performance_acceptance=False,external_UIA_absence_verified=False,
                  qualification='Existing final GUI, already warming/learning; no equivalent before run. No UI queries by sampler; other client traffic unverified. This is not a clean before/after performance result.')
        r.save(args.out/'metadata.json',meta)
        samples=[]; start=time.perf_counter(); _,own=r.perf.times(r.k.GetCurrentProcess()); early_exit=False
        with (args.out/'samples.jsonl').open('x',encoding='utf-8') as raw:
            while time.perf_counter()-start<=args.warmup+args.seconds+1:
                if r.k.WaitForSingleObject(handle,0)!=258: early_exit=True; break
                item=r.perf.sample(handle,start,creation); samples.append(item)
                raw.write(json.dumps(item)+'\n'); raw.flush(); time.sleep(1)
        _,own_end=r.perf.times(r.k.GetCurrentProcess())
        stable=[s for s in samples if args.warmup<=s['t']<=args.warmup+args.seconds]
        connection=sqlite3.connect('file:'+db.as_posix()+'?mode=ro',uri=True)
        intervals=[x[0] for x in connection.execute('SELECT observation_interval FROM raw WHERE id>? AND observation_interval IS NOT NULL',(maxrow,))]
        rows=connection.execute('SELECT count(*) FROM raw WHERE id>?',(maxrow,)).fetchone()[0]
        connection.close()
        result=dict(finished_utc=r.utc(),early_exit=early_exit,stable=r.perf.summarize(stable,meta['logical_cpu_count']),
                    sampler_cpu_seconds=own_end-own,new_raw_rows=rows,actual_intervals=r.perf.dist(intervals),
                    final_token=r.measure.token_info(handle),performance_acceptance=False,
                    qualification=meta['qualification'],target_left_running=r.k.WaitForSingleObject(handle,0)==258)
        r.save(args.out/'summary.json',result)
        print(json.dumps(result,indent=2))
        return 0 if not early_exit and stable[-1]['t']-stable[0]['t']>=args.seconds-2 else 1
    finally: r.k.CloseHandle(handle)

if __name__=='__main__': raise SystemExit(main())
