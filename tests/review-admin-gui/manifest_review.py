"""Independent SDK mt.exe extraction: inspect binaries as resources, never launch.

Run from an MSVC SDK environment or pass --mt with its absolute path. A GUI must
have RT_MANIFEST #1 embedded. Ordinary binaries may retain an external asInvoker
manifest. Every exact extraction and command log is retained in a new directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def lacks_resource_directory(path):
    data=path.read_bytes()
    if data[:2]!=b'MZ': return False
    pe=struct.unpack_from('<I',data,60)[0]
    if data[pe:pe+4]!=b'PE\0\0': return False
    optional=pe+24; magic=struct.unpack_from('<H',data,optional)[0]
    if magic not in (0x10b,0x20b): return False
    directory=optional+(112 if magic==0x20b else 96)
    return struct.unpack_from('<II',data,directory+16)==(0,0)

def shape(node):
    return (node.tag,tuple(sorted(node.attrib.items())),(node.text or '').strip(),tuple(shape(c) for c in node))

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--mt',type=Path)
    ap.add_argument('--gui',type=Path,nargs='+',required=True)
    ap.add_argument('--ordinary',type=Path,nargs='+',required=True)
    ap.add_argument('--ordinary-baseline',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    mt=args.mt.resolve(strict=True) if args.mt else Path(shutil.which('mt.exe') or '').resolve(strict=True)
    if not mt.is_file(): raise RuntimeError('SDK mt.exe is required')
    out=args.out.resolve(); out.mkdir(parents=True,exist_ok=False)
    reports=[]
    for index,(path,expected) in enumerate([(p,'requireAdministrator') for p in args.gui]+[(p,'asInvoker') for p in args.ordinary]):
        path=path.resolve(strict=True); before=sha(path)
        xml=out/(str(index)+'-'+path.name+'.xml')
        cmd=[str(mt),'-nologo','-inputresource:'+str(path)+';#1','-out:'+str(xml)]
        result=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,check=False)
        (out/(str(index)+'-mt.log')).write_bytes(result.stdout)
        source='RT_MANIFEST_1'
        if result.returncode:
            if expected!='asInvoker': raise RuntimeError('GUI lacks valid embedded #1: '+str(path))
            sidecar=Path(str(path)+'.manifest')
            if not sidecar.is_file():
                baseline=(args.ordinary_baseline/path.name).resolve(strict=True)
                if not lacks_resource_directory(path) or not lacks_resource_directory(baseline) or Path(str(baseline)+'.manifest').exists():
                    raise RuntimeError('Unverified missing ordinary manifest: '+str(path))
                reports.append(dict(path=str(path),sha256=before,source='absent_preserved_from_baseline',
                    expected='no requested elevation; explicit asInvoker manifest is not present',
                    baseline=str(baseline),baseline_sha256=sha(baseline),execution=[],
                    extraction_command=cmd,extraction_exit=result.returncode))
                continue
            xml.write_bytes(sidecar.read_bytes()); source='external_sidecar'
        root=ET.fromstring(xml.read_bytes())
        levels=root.findall('.//{urn:schemas-microsoft-com:asm.v3}requestedExecutionLevel')
        if len(levels)!=1 or levels[0].attrib!={'level':expected,'uiAccess':'false'}:
            raise RuntimeError('Unexpected execution policy: '+str(path))
        controls=[n.attrib for n in root.findall('.//{urn:schemas-microsoft-com:asm.v1}assemblyIdentity') if n.get('name')=='Microsoft.Windows.Common-Controls']
        if expected=='requireAdministrator':
            if len(controls)!=1 or controls[0].get('version')!='6.0.0.0' or controls[0].get('publicKeyToken')!='6595b64144ccf1df':
                raise RuntimeError('Common Controls v6 dependency missing or changed: '+str(path))
            sidecar=Path(str(path)+'.manifest')
            if not sidecar.is_file() or shape(ET.fromstring(sidecar.read_bytes()))!=shape(root):
                raise RuntimeError('GUI sidecar must match the embedded policy: '+str(path))
        if any(n.tag.rsplit('}',1)[-1]=='autoElevate' for n in root.iter()):
            raise RuntimeError('Unexpected autoElevate declaration: '+str(path))
        if sha(path)!=before: raise RuntimeError('Binary changed during resource extraction')
        reports.append(dict(path=str(path),sha256=before,expected=expected,source=source,execution=levels[0].attrib,
            controls=controls,manifest_sha256=sha(xml),extraction_command=cmd,extraction_exit=result.returncode))
    (out/'report.json').write_text(json.dumps(dict(mt=str(mt),mt_sha256=sha(mt),binaries=reports,passed=True),indent=2),encoding='utf-8')
    print(json.dumps(dict(passed=True,binaries=len(reports),out=str(out))))

if __name__=='__main__': main()
