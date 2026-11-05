"""Run the actual product against a new DB; record actual tokens and external cost.

No UI queries, security-policy changes, target termination, or privilege activation.
Run the same command from a UAC-approved shell for an elevated measurement.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import platform
from pathlib import Path
import sqlite3
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('perf', ROOT/'tests/performance/measure-native-gui.py')
perf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(perf)
k = perf.k
a = C.WinDLL('advapi32', use_last_error=True)
a.OpenProcessToken.argtypes = [W.HANDLE, W.DWORD, C.POINTER(W.HANDLE)]
a.GetTokenInformation.argtypes = [W.HANDLE, C.c_int, C.c_void_p, W.DWORD, C.POINTER(W.DWORD)]
a.GetSidSubAuthorityCount.argtypes = [C.c_void_p]
a.GetSidSubAuthorityCount.restype = C.POINTER(C.c_ubyte)
a.GetSidSubAuthority.argtypes = [C.c_void_p, W.DWORD]
a.GetSidSubAuthority.restype = C.POINTER(W.DWORD)

class Luid(C.Structure):
    _fields_ = [('LowPart', W.DWORD), ('HighPart', W.LONG)]
class Privilege(C.Structure):
    _fields_ = [('Luid', Luid), ('Attributes', W.DWORD)]
class SidAndAttributes(C.Structure):
    _fields_ = [('Sid', C.c_void_p), ('Attributes', W.DWORD)]
a.LookupPrivilegeNameW.argtypes = [W.LPCWSTR, C.POINTER(Luid), W.LPWSTR, C.POINTER(W.DWORD)]

def token_info(handle):
    token = W.HANDLE()
    if not a.OpenProcessToken(handle, 0x0008, C.byref(token)):
        raise C.WinError(C.get_last_error())
    def get(kind):
        length = W.DWORD()
        a.GetTokenInformation(token, kind, None, 0, C.byref(length))
        if not length.value: raise C.WinError(C.get_last_error())
        buf = C.create_string_buffer(length.value)
        if not a.GetTokenInformation(token, kind, buf, len(buf), C.byref(length)):
            raise C.WinError(C.get_last_error())
        return buf
    try:
        elevated = C.cast(get(20), C.POINTER(W.DWORD)).contents.value
        elevation_type = C.cast(get(18), C.POINTER(W.DWORD)).contents.value
        integrity_buf = get(25)
        sid = C.cast(integrity_buf, C.POINTER(SidAndAttributes)).contents.Sid
        subcount = a.GetSidSubAuthorityCount(sid).contents.value
        integrity = a.GetSidSubAuthority(sid, subcount-1).contents.value
        priv_buf = get(3)
        count = C.cast(priv_buf, C.POINTER(W.DWORD)).contents.value
        privileges = {}
        for index in range(count):
            item = Privilege.from_buffer(priv_buf, 4+index*C.sizeof(Privilege))
            size = W.DWORD(256); name = C.create_unicode_buffer(256)
            if not a.LookupPrivilegeNameW(None, C.byref(item.Luid), name, C.byref(size)):
                raise C.WinError(C.get_last_error())
            privileges[name.value] = dict(present=True, enabled=bool(item.Attributes & 2), attributes=item.Attributes)
        return dict(elevated=bool(elevated), elevation_type=elevation_type,
                    elevation_type_name={1:'default',2:'full',3:'limited'}.get(elevation_type,'unknown'),
                    integrity_rid=integrity,
                    integrity_name={0:'untrusted',4096:'low',8192:'medium',8448:'medium_plus',12288:'high',16384:'system',20480:'protected'}.get(integrity,'other'),
                    debug=privileges.get('SeDebugPrivilege',dict(present=False,enabled=False)),
                    privileges=privileges)
    finally:
        k.CloseHandle(token)

def db_summary(path):
    db = sqlite3.connect('file:'+path.as_posix()+'?mode=ro', uri=True)
    db.row_factory = sqlite3.Row
    try:
        tables = [r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")]
        return dict(integrity=[r[0] for r in db.execute('PRAGMA integrity_check')],
                    foreign_key_errors=[list(r) for r in db.execute('PRAGMA foreign_key_check')],
                    tables=tables, raw_count=db.execute('SELECT count(*) FROM raw').fetchone()[0])
    finally: db.close()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--token-only',action='store_true')
    ap.add_argument('--exe',type=Path)
    ap.add_argument('--out',type=Path)
    ap.add_argument('--expect',choices=['normal','elevated'])
    ap.add_argument('--label',default='unspecified')
    ap.add_argument('--count',type=int,default=25)
    ap.add_argument('--diagnostics',action='store_true')
    ap.add_argument('--visible',action='store_true')
    args=ap.parse_args()
    if args.count <= 0: ap.error('--count must be positive (zero would run forever)')
    parent_token=token_info(k.GetCurrentProcess())
    if args.token_only:
        print(json.dumps(parent_token,indent=2)); return
    if not args.exe or not args.out or not args.expect: ap.error('--exe, --out, --expect required')
    expected = args.expect == 'elevated'
    if parent_token['elevated'] != expected:
        raise RuntimeError('Refusing mislabeled run: actual parent token '+json.dumps(parent_token))
    if not expected and parent_token['integrity_rid'] >= 12288:
        raise RuntimeError('Normal run requires actual non-high integrity token')
    exe=args.exe.resolve(strict=True); out=args.out.resolve()
    out.mkdir(parents=True,exist_ok=False)
    db=out/'fresh.sqlite'
    command=[str(exe),'--db',str(db),'--count',str(args.count),'--interval-ms','3000','--stats']
    if not args.visible: command.append('--headless')
    if args.diagnostics: command.append('--diagnostics')
    ncpu=k.GetActiveProcessorCount(0xffff)
    metadata=dict(label=args.label,command=command,exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  sampler_helper_sha256=hashlib.sha256(Path(perf.__file__).read_bytes()).hexdigest(),
                  platform=platform.platform(),python_version=platform.python_version(),
                  parent_token=parent_token,privilege_activation_requested=False,
                  started_utc=datetime.now(timezone.utc).isoformat(),logical_cpu_count=ncpu,
                  notes='Fresh DB. No UIA/window queries. Same 3000ms worker wait; actual QPC interval in DB. Cold process, OS cache uncontrolled.')
    (out/'metadata.json').write_text(json.dumps(metadata,indent=2),encoding='utf-8')
    start=time.perf_counter(); _,own_cpu=perf.times(k.GetCurrentProcess())
    with (out/'product.log').open('wb') as log, (out/'samples.jsonl').open('w',encoding='utf-8') as raw:
        child=subprocess.Popen(command,cwd=str(out),stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        handle=k.OpenProcess(0x1000,False,child.pid)
        if not handle: raise C.WinError(C.get_last_error())
        samples=[]
        try:
            creation,_=perf.times(handle)
            metadata.update(pid=child.pid,creation_filetime_100ns=creation,child_token=token_info(handle))
            metadata['token_matches_expected']=(metadata['child_token']['elevated']==expected and
                (expected or metadata['child_token']['integrity_rid'] < 12288))
            (out/'metadata.json').write_text(json.dumps(metadata,indent=2),encoding='utf-8')
            print(json.dumps(dict(event='started',pid=child.pid,label=args.label,token=metadata['child_token']['elevation_type_name'],integrity=metadata['child_token']['integrity_name'],debug=metadata['child_token']['debug'])),flush=True)
            while child.poll() is None:
                try: item=perf.sample(handle,start,creation)
                except OSError:
                    if child.poll() is not None: break
                    raise
                samples.append(item); raw.write(json.dumps(item)+'\n'); raw.flush()
                time.sleep(1)
            exit_code=child.wait()
            _,own_end=perf.times(k.GetCurrentProcess())
            summary=dict(exit_code=exit_code,token_matches_expected=metadata['token_matches_expected'],
                         samples=perf.summarize(samples,ncpu),sampler_cpu_seconds=own_end-own_cpu,
                         finished_utc=datetime.now(timezone.utc).isoformat(),db=db_summary(db) if db.exists() else None)
            (out/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
            print(json.dumps(dict(event='finished',exit_code=exit_code,out=str(out))),flush=True)
            if exit_code or not metadata['token_matches_expected']: raise SystemExit(1)
        finally: k.CloseHandle(handle)

if __name__=='__main__': main()
