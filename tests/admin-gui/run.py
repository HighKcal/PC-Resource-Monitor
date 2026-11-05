"""Local administrator GUI verification; never adjusts privileges or kills a process.

launch uses ShellExecuteEx(open) from a medium token to test the embedded manifest.
suite must be started by the user in an elevated shell. Fresh outputs are required.
No UI automation, screenshots, text reads or input occur in performance intervals.
"""
from __future__ import annotations
import argparse
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parents[2]

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module

measure = load('admin_measure', ROOT/'tests/access-coverage/measure.py')
perf = measure.perf
k = measure.k
k.WaitForSingleObject.argtypes = [W.HANDLE, W.DWORD]
k.WaitForSingleObject.restype = W.DWORD
k.GetExitCodeProcess.argtypes = [W.HANDLE, C.POINTER(W.DWORD)]
k.GetProcessId.argtypes = [W.HANDLE]; k.GetProcessId.restype = W.DWORD

def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def utc(): return datetime.now(timezone.utc).isoformat()
def save(path, value): Path(path).write_text(json.dumps(value, indent=2), encoding='utf-8')
def admin(token):
    if not (token['elevated'] and token['elevation_type']==2 and token['integrity_rid']==12288):
        raise RuntimeError('Actual elevated/full/high token required')
    if token['debug'].get('enabled'): raise RuntimeError('SeDebugPrivilege must remain disabled')

class ShellInfo(C.Structure):
    _fields_ = [('cbSize',W.DWORD),('fMask',W.ULONG),('hwnd',W.HWND),
                ('lpVerb',W.LPCWSTR),('lpFile',W.LPCWSTR),('lpParameters',W.LPCWSTR),
                ('lpDirectory',W.LPCWSTR),('nShow',C.c_int),('hInstApp',W.HINSTANCE),
                ('lpIDList',C.c_void_p),('lpClass',W.LPCWSTR),('hkeyClass',W.HKEY),
                ('dwHotKey',W.DWORD),('hIcon',W.HANDLE),('hProcess',W.HANDLE)]

def shell(exe, arguments, cwd, verb='open'):
    ole=C.OleDLL('ole32'); ole.CoInitializeEx(None,2)
    api=C.WinDLL('shell32',use_last_error=True)
    api.ShellExecuteExW.argtypes=[C.POINTER(ShellInfo)]; api.ShellExecuteExW.restype=W.BOOL
    info=ShellInfo(); info.cbSize=C.sizeof(info); info.fMask=0x40|0x100|0x400
    info.lpVerb=verb; info.lpFile=str(exe); info.lpParameters=subprocess.list2cmdline(arguments)
    info.lpDirectory=str(cwd); info.nShow=1
    try:
        ok=api.ShellExecuteExW(C.byref(info)); error=0 if ok else C.get_last_error()
        return info.hProcess,error
    finally: ole.CoUninitialize()

def launch(args):
    out=args.out.resolve(); out.mkdir(parents=True,exist_ok=False)
    token=measure.token_info(k.GetCurrentProcess())
    if token['elevated'] or token['integrity_rid']>=12288:
        raise RuntimeError('Normal ShellExecute test requires actual medium/non-elevated parent')
    exe=args.exe.resolve(strict=True); db=out/'fresh.sqlite'
    command=['--db',str(db),'--interval-ms','3000','--stats','--diagnostics']
    if not args.interactive: command+=['--count','8']
    record=dict(started_utc=utc(),parent_token=token,exe=str(exe),exe_sha256=sha(exe),
                verb='open',arguments=command,expected=args.expect,db=str(db))
    save(out/'launch.json',record)
    handle,error=shell(exe,command,out)
    record.update(shell_error=error,returned_process_handle=bool(handle))
    if error:
        record.update(finished_utc=utc(),db_created=db.exists(),
                      passed=args.expect=='cancel' and error==1223 and not db.exists())
        save(out/'launch.json',record)
        return 0 if record['passed'] else 1
    if not handle: raise RuntimeError('Shell did not return a process handle')
    try:
        record.update(pid=k.GetProcessId(handle),creation_filetime_100ns=perf.times(handle)[0],
                      child_token=measure.token_info(handle))
        admin(record['child_token']); save(out/'launch.json',record)
        while k.WaitForSingleObject(handle,1000)==258: pass
        code=W.DWORD()
        if not k.GetExitCodeProcess(handle,C.byref(code)): raise C.WinError(C.get_last_error())
        record.update(exit_code=code.value,finished_utc=utc(),db_created=db.exists(),
                      db=measure.db_summary(db) if db.exists() else None,
                      passed=args.expect=='approve' and code.value==0)
        save(out/'launch.json',record)
        return 0 if record['passed'] else 1
    finally: k.CloseHandle(handle)

def traffic(log):
    total=dict(lvm_sent=0,hdm_sent=0,getobject=0,text_sent=0,length_sent=0,hook_failures=0,hook_records=0,intervals=0)
    for line in log.read_text(errors='replace').splitlines():
        v=dict(re.findall(r'(\w+)=([^\s;]+)',line))
        if line.startswith('PERF sample '): total['intervals']+=1
        elif line.startswith('PERF lvm '): total['lvm_sent']+=int(v['sent'])
        elif line.startswith('PERF hdm '): total['hdm_sent']+=int(v['sent'])
        elif line.startswith('PERF getobject '): total['getobject']+=int(v['count'])
        elif line.startswith('PERF gettext '):
            total['text_sent']+=int(v['text_sent']); total['length_sent']+=int(v['length_sent'])
        elif line.startswith('PERF hooks '):
            total['hook_records']+=1
            seen=[int(x) for x in v['seen'].split(',')]
            installed=[int(x) for x in v['installed'].split(',')]
            failed=[int(x) for x in v['failed'].split(',')]
            total['hook_failures']+=sum(failed)
            if len(seen)!=5 or len(installed)!=5 or len(failed)!=5 or seen!=installed or not sum(seen) or any(v[x]!='1' for x in ('root','list','header')):
                total['hook_failures']+=1
    total['observed_quiet']=bool(total['intervals'] and total['hook_records']==total['intervals'] and not any(total[x] for x in ('lvm_sent','hdm_sent','getobject','text_sent','length_sent','hook_failures')))
    total['limit']='Covers instrumented window-message paths, not every possible UIA COM call or client.'
    return total

def run(out, label, exe, count, extra=(), diagnostic=False):
    folder=out/label; folder.mkdir(exist_ok=False); db=folder/'fresh.sqlite'
    token=measure.token_info(k.GetCurrentProcess()); admin(token)
    ncpu=k.GetActiveProcessorCount(0xffff)
    command=[str(exe),'--db',str(db),'--count',str(count),'--interval-ms','3000','--stats',*extra]
    metadata=dict(label=label,started_utc=utc(),command=command,exe_sha256=sha(exe),
        parent_token=token,logical_cpu_count=ncpu,diagnostic=diagnostic,privilege_activation_requested=False,
        warmup_seconds=30,stable_end_seconds=210,initial_group_state='expanded' if '--probe-expanded' in extra else 'folded',
        notes='New empty DB; default 3000ms wait; OS cache uncontrolled; no UI automation/input; boundary window metadata outside stable interval only.')
    save(folder/'metadata.json',metadata)
    save(out/'status.json',dict(stage=label,utc=utc(),pid=os.getpid()))
    start=time.perf_counter(); _,own=perf.times(k.GetCurrentProcess())
    boundary=load('admin_boundary',ROOT/'tests/review-gui-performance/window_boundary.py')
    with (folder/'product.log').open('xb') as log, (folder/'samples.jsonl').open('x',encoding='utf-8') as raw:
        child=subprocess.Popen(command,cwd=folder,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        handle=k.OpenProcess(0x1000,False,child.pid)
        if not handle: raise C.WinError(C.get_last_error())
        try:
            creation,_=perf.times(handle); actual=measure.token_info(handle); admin(actual)
            metadata.update(pid=child.pid,creation_filetime_100ns=creation,child_token=actual)
            save(folder/'metadata.json',metadata)
            samples=[]; boundaries={}; terminal_error=None
            while child.poll() is None:
                try: item=perf.sample(handle,start,creation)
                except OSError as error:
                    if child.poll() is not None: terminal_error=repr(error); break
                    raise
                samples.append(item); raw.write(json.dumps(item)+'\n'); raw.flush()
                stage='before_warmup' if 5<=item['t']<30 else 'after_stable' if item['t']>210 else None
                if stage and stage not in boundaries and '--headless' not in extra:
                    boundaries[stage]=boundary.boundary_snapshot(child.pid,creation,exe)
                    save(folder/'window-boundaries.json',boundaries)
                time.sleep(1)
            code=child.wait(); _,own_end=perf.times(k.GetCurrentProcess())
            stable=[s for s in samples if 30<=s['t']<=210]
            summary=dict(exit_code=code,whole=perf.summarize(samples,ncpu),stable=perf.summarize(stable,ncpu),
                sampler_cpu_seconds=own_end-own,terminal_sample_error=terminal_error,finished_utc=utc(),
                db=measure.db_summary(db),window_boundaries=boundaries)
            connection=sqlite3.connect('file:'+db.as_posix()+'?mode=ro',uri=True)
            summary['actual_intervals']=perf.dist([r[0] for r in connection.execute('SELECT observation_interval FROM raw WHERE observation_interval IS NOT NULL')])
            summary['open_sessions']=connection.execute('SELECT count(*) FROM session WHERE ended IS NULL').fetchone()[0]
            summary['process_count']=perf.dist([int(v) for v in re.findall(r'^batch=.*?\brows=(\d+)',(folder/'product.log').read_text(errors='replace'),re.M)])
            connection.close()
            if diagnostic: summary['traffic']=traffic(folder/'product.log')
            save(folder/'summary.json',summary)
            if code or summary['db']['integrity']!=['ok'] or summary['db']['foreign_key_errors'] or summary['open_sessions']:
                raise RuntimeError('Run failed: '+label)
            return summary
        finally: k.CloseHandle(handle)

def suite(args):
    out=args.out.resolve(); out.mkdir(parents=True,exist_ok=False)
    token=measure.token_info(k.GetCurrentProcess()); admin(token)
    config=json.loads(args.config.read_text(encoding='utf-8'))
    for item in config['inputs']:
        if sha(item['path'])!=item['sha256']: raise RuntimeError('Pinned input changed: '+item['path'])
    save(out/'suite.json',dict(started_utc=utc(),token=token,config=config,runner_sha256=sha(__file__),delay_seconds=args.delay))
    save(out/'status.json',dict(stage='delay',utc=utc(),pid=os.getpid(),seconds=args.delay))
    time.sleep(args.delay)
    files={key:Path(value) for key,value in config['executables'].items()}
    results={}
    # Diagnose both versions first. Do not accept contaminated runs as product cost.
    for version in ('before','after'):
        label='probe-'+version
        results[label]=run(out,label,files[label],25,diagnostic=True)
    quiet=all(results[x]['traffic']['observed_quiet'] for x in results)
    if not quiet and not args.allow_contaminated:
        save(out/'completion.json',dict(completed=False,reason='External/other-thread UI queries remain; product performance acceptance blocked.',results=results,finished_utc=utc()))
        save(out/'status.json',dict(stage='blocked-external-queries',utc=utc(),pid=os.getpid()))
        return 3
    # Serialized order. No screenshots or UI testing may overlap this suite.
    for label,key,extra,count in [
        ('folded-before','before',[],75),('folded-after','after',[],75),
        ('headless-after','after',['--headless'],75),
        ('expanded-after-control','control-after',['--probe-expanded'],75),
        ('probe-after-end','probe-after',[],25)]:
        results[label]=run(out,label,files[key],count,extra,diagnostic=key.startswith('probe-'))
    quiet=quiet and results['probe-after-end']['traffic']['observed_quiet']
    changed=[item['path'] for item in config['inputs'] if sha(item['path'])!=item['sha256']]
    final_token=measure.token_info(k.GetCurrentProcess()); admin(final_token)
    save(out/'completion.json',dict(completed=not changed,performance_acceptance=False,observed_message_paths_quiet=quiet,
        absence_of_all_external_UIA_calls_proven=False,inputs_changed=changed,token_after=final_token,
        results=results,finished_utc=utc(),
        qualification='Uninstrumented intervals are bracketed by copied diagnostic runs; do not claim continuous absence of every external UIA request. Control/probe executables are not final product binaries.'))
    save(out/'status.json',dict(stage='completed' if not changed else 'input-changed',utc=utc(),pid=os.getpid()))
    return (0 if quiet else 3) if not changed else 1

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='mode',required=True)
    p=sub.add_parser('launch'); p.add_argument('--exe',type=Path,required=True); p.add_argument('--out',type=Path,required=True)
    p.add_argument('--expect',choices=['cancel','approve'],required=True); p.add_argument('--interactive',action='store_true')
    p=sub.add_parser('suite'); p.add_argument('--config',type=Path,required=True); p.add_argument('--out',type=Path,required=True)
    p.add_argument('--delay',type=int,default=30); p.add_argument('--allow-contaminated',action='store_true',help='Diagnostic only; never makes polluted measurements pass')
    args=parser.parse_args()
    if getattr(args,'delay',0)<0: parser.error('delay must be nonnegative')
    try: return launch(args) if args.mode=='launch' else suite(args)
    except BaseException:
        if args.out.exists(): (args.out/'failure.txt').write_text(traceback.format_exc(),encoding='utf-8')
        raise

if __name__=='__main__': raise SystemExit(main())
