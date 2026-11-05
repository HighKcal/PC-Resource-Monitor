"""Run only the evidence classification function; no Windows/process APIs."""
import ast
from pathlib import Path
import re
import tempfile

ROOT=Path(__file__).resolve().parents[2]
source=ast.parse((ROOT/'tests/admin-gui/run.py').read_text(encoding='utf-8'))
function=next(node for node in source.body if isinstance(node,ast.FunctionDef) and node.name=='traffic')
scope={'re':re}
exec(compile(ast.Module(body=[function],type_ignores=[]),'reviewed_traffic','exec'),scope)
sample='PERF sample reason=periodic interval_s=5\n'
hooks='PERF hooks root=1 list=1 header=1 seen=0,1,1,1,20 installed=0,1,1,1,20 failed=0,0,0,0,0\n'
base=sample+hooks
cases=[
 ('all observed counters zero',base,True),
 ('missing sample','',False),
 ('missing hooks',sample,False),
 ('one unreported hook interval',base+sample,False),
 ('duplicate hooks',base+hooks,False),
 ('failed descendant hook',base.replace('failed=0,0,0,0,0','failed=0,0,0,0,1'),False),
 ('missing descendant installation',base.replace('installed=0,1,1,1,20','installed=0,1,1,1,19'),False),
 ('missing list hook',base.replace('list=1','list=0'),False),
 ('missing header hook',base.replace('header=1','header=0'),False),
 ('bad hook vector',base.replace('failed=0,0,0,0,0','failed=0,0'),False),
 ('list other thread',base+'PERF lvm name=GETITEMW sent=1\n',False),
 ('header other thread',base+'PERF hdm name=GETITEMW sent=1\n',False),
 ('object request',base+'PERF getobject scope=4 count=1\n',False),
 ('text other thread',base+'PERF gettext scope=4 text_sent=1 length_sent=0\n',False),
 ('length other thread',base+'PERF gettext scope=4 text_sent=0 length_sent=1\n',False),
 ('product own text reads',base+'PERF gettext scope=4 text=100 text_sent=0 length=100 length_sent=0\n',True),
]
with tempfile.TemporaryDirectory(prefix='admin-traffic-review-') as folder:
 path=Path(folder)/'product.log'
 for label,text,expected in cases:
  path.write_text(text,encoding='utf-8')
  actual=scope['traffic'](path)['observed_quiet']
  if actual is not expected: raise AssertionError((label,expected,actual))
print('independent_traffic_guard checks='+str(len(cases))+' failures=0')
