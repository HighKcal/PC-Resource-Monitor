"""Optional read-only follow-up, outside the timed product run.

GetProcessInformation(ProcessProtectionLevelInfo) on limited-query handles.
No privilege changes. No error-code or executable-name inference of protection.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime, timezone
import json
from pathlib import Path
import sqlite3
import measure

def probe(db_path, output):
    db=sqlite3.connect('file:'+db_path.as_posix()+'?mode=ro',uri=True)
    db.row_factory=sqlite3.Row
    targets=[dict(r) for r in db.execute('SELECT pid,creation,valid FROM raw WHERE run_id=(SELECT max(id) FROM collection_run) ORDER BY id')]
    db.close()
    k=measure.k
    api=getattr(k,'GetProcessInformation',None)
    if api:
        api.argtypes=[W.HANDLE,C.c_int,C.c_void_p,W.DWORD]
        api.restype=W.BOOL
    items=[]
    for target in targets:
        row=dict(pid=target['pid'],expected_creation=target['creation'],identity_match=False,
                 special='idle' if target['pid']==0 else 'system' if target['pid']==4 else None)
        handle=k.OpenProcess(0x1000,False,target['pid'])
        if not handle: row.update(open_error=C.get_last_error(),protection='unverified_open_failed')
        else:
            try:
                try:
                    creation,_=measure.perf.times(handle)
                    row.update(observed_creation=creation,identity_match=bool(target['valid']&2) and creation==target['creation'])
                except OSError as error: row['times_error']=error.winerror
                level=W.DWORD()
                if not api: row['protection']='unsupported_api'
                elif not api(handle,7,C.byref(level),C.sizeof(level)):
                    row.update(protection='unverified_query_failed',protection_error=C.get_last_error())
                else:
                    row.update(protection_level=level.value,
                               protection='none' if level.value==0xfffffffe else 'protected_level_reported' if level.value in range(9) else 'unrecognized_level')
            finally: k.CloseHandle(handle)
        items.append(row)
    result=dict(observed_utc=datetime.now(timezone.utc).isoformat(),token=measure.token_info(k.GetCurrentProcess()),
                note='Later observation. Only identity_match rows connect to product rows. Failed OpenProcess never means protected. SDK 10.0.26100.0 WinBase.h constants.',items=items)
    output.write_text(json.dumps(result,indent=2),encoding='utf-8')
    return result

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--db',type=Path,required=True); ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    if args.out.exists(): raise RuntimeError('Refusing to overwrite evidence')
    result=probe(args.db.resolve(strict=True),args.out.resolve())
    from collections import Counter
    print(json.dumps(dict(total=len(result['items']),current=Counter(r['protection'] for r in result['items']),
                         matched=Counter(r['protection'] for r in result['items'] if r['identity_match']))))
