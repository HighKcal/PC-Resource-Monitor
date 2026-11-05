"""Windows-only, external process sampler; no UIA, HWND reads, screenshots or input.

Run a self-terminating monitor GUI (--count) against a new DB. perf_counter is
monotonic QPC on Windows; retained process handle + creation FILETIME prevent PID
reuse. JSONL is raw evidence, JSON contains separately bounded warm/stable phases.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import time

k = C.WinDLL('kernel32', use_last_error=True)
p = C.WinDLL('psapi', use_last_error=True)

class Memory(C.Structure):
    _fields_ = [('cb', W.DWORD), ('PageFaultCount', W.DWORD)] + [
        (name, C.c_size_t) for name in ('PeakWorkingSetSize', 'WorkingSetSize',
        'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
        'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage', 'PrivateUsage')]

k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
k.OpenProcess.restype = W.HANDLE
k.GetCurrentProcess.restype = W.HANDLE
k.CloseHandle.argtypes = [W.HANDLE]
k.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
k.GetProcessTimes.restype = W.BOOL
k.GetProcessHandleCount.argtypes = [W.HANDLE, C.POINTER(W.DWORD)]
k.GetProcessHandleCount.restype = W.BOOL
k.GetActiveProcessorCount.argtypes = [W.WORD]
k.GetActiveProcessorCount.restype = W.DWORD
p.GetProcessMemoryInfo.argtypes = [W.HANDLE, C.POINTER(Memory), W.DWORD]
p.GetProcessMemoryInfo.restype = W.BOOL

def times(handle):
    created, exited, kernel, user = (W.FILETIME() for _ in range(4))
    if not k.GetProcessTimes(handle, C.byref(created), C.byref(exited), C.byref(kernel), C.byref(user)):
        raise C.WinError(C.get_last_error())
    val = lambda ft: (ft.dwHighDateTime << 32) | ft.dwLowDateTime
    return val(created), (val(kernel) + val(user)) / 1e7

def sample(handle, start, creation):
    before = time.perf_counter()
    identity, cpu = times(handle)
    if identity != creation:
        raise RuntimeError('Process creation identity changed')
    mem = Memory(); mem.cb = C.sizeof(mem)
    handles = W.DWORD()
    if not p.GetProcessMemoryInfo(handle, C.byref(mem), mem.cb):
        raise C.WinError(C.get_last_error())
    if not k.GetProcessHandleCount(handle, C.byref(handles)):
        raise C.WinError(C.get_last_error())
    after = time.perf_counter()
    return dict(t=(before+after)/2-start, cpu_seconds=cpu,
                working_set_bytes=mem.WorkingSetSize, private_bytes=mem.PrivateUsage,
                handles=handles.value, sample_wall_ms=(after-before)*1000)

def dist(values):
    a = sorted(values)
    if not a: return None
    return dict(min=a[0], p50=a[math.ceil(.5*len(a))-1],
                p95=a[math.ceil(.95*len(a))-1], p99=a[math.ceil(.99*len(a))-1],
                max=a[-1], mean=sum(a)/len(a))

def summarize(samples, ncpu):
    if len(samples) < 2: return None
    first, last = samples[0], samples[-1]
    elapsed = last['t'] - first['t']
    intervals = [dict(end_t=b['t'], wall_seconds=b['t']-a['t'],
                     cpu_percent=(b['cpu_seconds']-a['cpu_seconds'])*100/(ncpu*(b['t']-a['t'])))
                 for a, b in zip(samples, samples[1:])]
    return dict(start_t=first['t'], end_t=last['t'], wall_seconds=elapsed,
                cpu_seconds=last['cpu_seconds']-first['cpu_seconds'],
                cpu_percent=(last['cpu_seconds']-first['cpu_seconds'])*100/(elapsed*ncpu),
                sample_count=len(samples), interval_cpu_percent=dist([x['cpu_percent'] for x in intervals]),
                sample_interval_seconds=dist([x['wall_seconds'] for x in intervals]),
                highest_intervals=sorted(intervals, key=lambda x:x['cpu_percent'], reverse=True)[:10],
                memory={key:dict(first=first[key], last=last[key], **dist([s[key] for s in samples]))
                        for key in ('working_set_bytes','private_bytes','handles')},
                sampler_call_wall_ms=dist([s['sample_wall_ms'] for s in samples]))

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--warmup', type=float, default=30)
    ap.add_argument('--seconds', type=float, default=180)
    ap.add_argument('--interval', type=float, default=1)
    ap.add_argument('--label', default='unspecified')
    ap.add_argument('--window-boundaries', action='store_true', help='OS window metadata once before warmup and once AFTER stable sampling; no UIA')
    ap.add_argument('--note', default='No UI observation by this sampler; window state requires separate evidence.')
    ap.add_argument('extra', nargs=argparse.REMAINDER)
    args = ap.parse_args()
    exe, out = args.exe.resolve(strict=True), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    extra = args.extra[1:] if args.extra[:1] == ['--'] else args.extra
    if extra.count('--db') != 1:
        raise ValueError('Exactly one explicit new test --db required, including GUI-only runs')
    db = Path(extra[extra.index('--db')+1])
    if not db.is_absolute() or db.exists() or Path(str(db)+'-wal').exists() or Path(str(db)+'-shm').exists():
        raise ValueError('DB must be a new absolute path without existing sidecars')
    db.parent.mkdir(parents=True, exist_ok=True)
    if '--count' not in extra and '--probe-duration-ms' not in extra:
        raise ValueError('Use a self-terminating --count or probe duration; sampler never kills target')
    for flag in ('--count', '--probe-duration-ms'):
        if flag in extra and (extra.count(flag) != 1 or int(extra[extra.index(flag)+1]) <= 0):
            raise ValueError('Shutdown option must occur once with a positive value')
    if min(args.interval, args.seconds) <= 0 or args.warmup < 0: raise ValueError('Invalid duration')
    boundary = None
    if args.window_boundaries:
        if args.warmup < 10: raise ValueError('Boundary metadata requires at least 10s warmup')
        module_path=Path(__file__).resolve().parents[1]/'review-gui-performance'/'window_boundary.py'
        spec=importlib.util.spec_from_file_location('window_boundary', module_path)
        boundary=importlib.util.module_from_spec(spec); spec.loader.exec_module(boundary)
    ncpu = k.GetActiveProcessorCount(0xffff)
    if not ncpu: raise C.WinError(C.get_last_error())
    command = [str(exe), *extra]
    metadata = dict(label=args.label, note=args.note, exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                    command=command, started_utc=datetime.now(timezone.utc).isoformat(),
                    logical_cpu_count=ncpu, warmup_requested=args.warmup, stable_requested=args.seconds,
                    interval_requested=args.interval, clock=time.get_clock_info('perf_counter').__dict__,
                    sampler_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    own = k.GetCurrentProcess(); _, own_cpu = times(own)
    start = time.perf_counter()
    with (out/'target.log').open('wb') as log, (out/'samples.jsonl').open('w', encoding='utf-8') as raw:
        child = subprocess.Popen(command, cwd=str(out), stdout=log, stderr=subprocess.STDOUT,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        handle = k.OpenProcess(0x1000 | 0x0010, False, child.pid)
        if not handle: raise C.WinError(C.get_last_error())
        creation, initial_cpu = times(handle)
        metadata.update(pid=child.pid, creation_filetime_100ns=creation, initial_cpu_seconds=initial_cpu)
        (out/'metadata.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
        print(json.dumps(dict(event='started', pid=child.pid, logical_cpus=ncpu, label=args.label)), flush=True)
        samples=[]; next_at=start; error=None; terminal_sample_error=None; boundaries={}
        try:
            while child.poll() is None:
                delay=next_at-time.perf_counter()
                if delay > 0: time.sleep(delay)
                if child.poll() is not None: break
                try:
                    item=sample(handle,start,creation)
                except OSError as exc:
                    if child.poll() is not None:
                        terminal_sample_error=str(exc)
                        break
                    raise
                samples.append(item)
                raw.write(json.dumps(item)+'\n'); raw.flush()
                if boundary:
                    stage = ('before_warmup' if 5 <= item['t'] < args.warmup else
                             'after_stable' if item['t'] > args.warmup+args.seconds else None)
                    if stage and stage not in boundaries:
                        try:
                            boundaries[stage]=boundary.boundary_snapshot(child.pid,creation,exe)
                        except Exception as exc:
                            boundaries[stage]={'error':str(exc)}
                        (out/'window-boundaries.json').write_text(json.dumps(boundaries,indent=2),encoding='utf-8')
                next_at += args.interval
                if next_at < time.perf_counter(): next_at=time.perf_counter()+args.interval
        except Exception as exc:
            error=str(exc)  # never terminate a running GUI as a measurement side effect
        finally:
            final_creation, final_cpu=times(handle)
            k.CloseHandle(handle)
        duration=time.perf_counter()-start; _, own_end=times(own)
    warm=[s for s in samples if s['t'] <= args.warmup]
    stable=[s for s in samples if args.warmup <= s['t'] <= args.warmup+args.seconds]
    report=dict(**metadata, exit_code=child.poll(), error=error, terminal_sample_error=terminal_sample_error, lifetime_observed_seconds=duration,
                final_cpu_seconds=final_cpu, window_boundaries=boundaries, all_samples=summarize(samples,ncpu),
                startup=summarize(warm,ncpu), stable=summarize(stable,ncpu),
                stable_coverage_sufficient=bool(stable and stable[-1]['t']-stable[0]['t']>=args.seconds-2*args.interval),
                sampler_cpu_seconds=own_end-own_cpu, sampler_cpu_percent=(own_end-own_cpu)*100/(ncpu*duration))
    (out/'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({key:report[key] for key in ('label','exit_code','error','stable_coverage_sufficient','stable','sampler_cpu_percent')}, indent=2), flush=True)
    return 0 if child.poll()==0 and not error and report['stable_coverage_sufficient'] else 1

if __name__ == '__main__':
    raise SystemExit(main())
