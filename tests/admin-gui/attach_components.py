"""Add passive UI-thread CPU accounting to attach.py; no target UI messages.

The HWND must have been selected before the observation. Its owning PID is
checked against the launch identity; thread creation is held stable by a handle.
Non-UI CPU includes all other threads, not a claimed exclusive engine profile.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import importlib.util
import json
from pathlib import Path
import sys

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('attach_base',HERE/'attach.py')
base=importlib.util.module_from_spec(spec); spec.loader.exec_module(base)
r=base.r

def main():
    parser=argparse.ArgumentParser(add_help=False)
    parser.add_argument('--hwnd',type=int,required=True)
    args,remaining=parser.parse_known_args()
    launch=Path(remaining[remaining.index('--launch')+1]); out=Path(remaining[remaining.index('--out')+1])
    record=json.loads(launch.read_text(encoding='utf-8'))
    u=C.WinDLL('user32',use_last_error=True)
    u.GetWindowThreadProcessId.argtypes=[W.HWND,C.POINTER(W.DWORD)]
    u.GetWindowThreadProcessId.restype=W.DWORD
    owner=W.DWORD(); tid=u.GetWindowThreadProcessId(args.hwnd,C.byref(owner))
    if not tid or owner.value!=record['pid']: raise RuntimeError('Selected HWND owner differs from retained process identity')
    r.k.OpenThread.argtypes=[W.DWORD,W.BOOL,W.DWORD]; r.k.OpenThread.restype=W.HANDLE
    r.k.GetThreadTimes.argtypes=[W.HANDLE]+[C.POINTER(W.FILETIME)]*4
    r.k.GetThreadTimes.restype=W.BOOL
    handle=r.k.OpenThread(0x0800,False,tid)
    if not handle: raise C.WinError(C.get_last_error())
    r.k.GetProcessIdOfThread.argtypes=[W.HANDLE]
    r.k.GetProcessIdOfThread.restype=W.DWORD
    def thread_times():
        fields=[W.FILETIME() for _ in range(4)]
        if not r.k.GetThreadTimes(handle,*[C.byref(v) for v in fields]): raise C.WinError(C.get_last_error())
        values=[(v.dwHighDateTime<<32)|v.dwLowDateTime for v in fields]
        return values[0],(values[2]+values[3])/1e7
    try:
        if r.k.GetProcessIdOfThread(handle)!=record['pid']:
            raise RuntimeError('Held UI thread owner changed during open')
        thread_creation,_=thread_times()
        original=r.perf.sample
        def sample(process,start,creation):
            item=original(process,start,creation); identity,cpu=thread_times()
            if identity!=thread_creation: raise RuntimeError('UI thread identity changed')
            item['ui_cpu_seconds']=cpu
            return item
        r.perf.sample=sample; sys.argv=[sys.argv[0],*remaining]
        code=base.main()
        final_owner=W.DWORD()
        final_tid=u.GetWindowThreadProcessId(args.hwnd,C.byref(final_owner))
        if final_tid!=tid or final_owner.value!=record['pid'] or r.k.GetProcessIdOfThread(handle)!=record['pid']:
            raise RuntimeError('Window/UI thread identity changed across observation')
        metadata=json.loads((out/'metadata.json').read_text()); summary=json.loads((out/'summary.json').read_text())
        samples=[json.loads(line) for line in (out/'samples.jsonl').read_text().splitlines()]
        stable=[s for s in samples if metadata['warmup_seconds']<=s['t']<=metadata['warmup_seconds']+metadata['stable_seconds']]
        ui=r.perf.summarize([dict(s,cpu_seconds=s['ui_cpu_seconds']) for s in stable],metadata['logical_cpu_count'])
        r.save(out/'components.json',dict(hwnd=args.hwnd,pid=owner.value,ui_tid=tid,
            ui_thread_creation_filetime_100ns=thread_creation,ui=ui,
            non_ui_cpu_percent=summary['stable']['cpu_percent']-ui['cpu_percent'],
            total_cpu_percent=summary['stable']['cpu_percent'],component_sampler_sha256=r.sha(__file__),
            base_sampler_sha256=r.sha(HERE/'attach.py'),
            qualification='Total minus UI includes every other thread. It does not isolate individual engine/API operations or prove external UIA absence.'))
        return code
    finally: r.k.CloseHandle(handle)

if __name__=='__main__': raise SystemExit(main())
