"""Independent assertions for the evidence classification, no real DB/API."""
import importlib.util
from pathlib import Path

source=Path(__file__).resolve().parents[2]/'tests/access-coverage/analyze.py'
spec=importlib.util.spec_from_file_location('analysis_under_review',source)
module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
checks=0
def check(value):
    global checks
    checks+=1
    assert value, checks
def row(**updates):
    result=dict(run_id=1,pid=42,creation=None,valid=0,program_id=None,display_source=0,
                open_error=0,times_error=0,memory_error=0,io_error=0,name_error=0,path_error=13,
                cpu_status=2,read_status=2,write_status=2,observation_interval=None,cpu_interval=None,io_interval=None)
    result.update(updates); return result

denied=row(open_error=5)
for field in ('handle_name','display_name','identity','cpu','working_set','read','write'):
    check(module.reason(denied,field)=='not_attempted_open_access_denied')
check(module.reason(denied,'path')=='not_attempted_identity_unavailable')
aggregate=module.aggregate([denied])
check(all(not values for values in aggregate['attempted_api_errors'].values()))
check(not any(aggregate['success'].values()))

first=row(creation=100,valid=11,program_id=7,display_source=1,path_error=0,cpu_status=1,read_status=1,write_status=1)
check(module.reason(first,'cpu')=='first_sample')
check(module.reason(first,'working_set')=='success')
check(module.reason(first,'path')=='success_verified_or_same_instance_cached')
partial=row(creation=100,valid=47,program_id=7,display_source=1,path_error=0,io_error=299,read_status=4,write_status=0)
# Public flags are authoritative for independently valid metrics.
check(module.reason(partial,'write')=='success')
check(module.reason(partial,'read')=='api_error_299')
only_name=row(open_error=5,display_source=2)
aggregate=module.aggregate([only_name,first])
check(aggregate['success']['display_name']==2)
check(aggregate['success']['handle_name']==1)
check(aggregate['success']['list_name_only']==1)
check(aggregate['success']['list_name_with_any_metric']==0)
partial_name=row(valid=8,display_source=2)
check(module.aggregate([partial_name])['success']['list_name_with_any_metric']==1)

changed=row(valid=2,creation=101)
same=row(valid=2,creation=100)
comparison=module.compare([denied,first],[same,changed])
check(comparison['common_verified_instances']==1)
check(comparison['left_identity_unknown']==1)
check(comparison['right_verified_only']==1)
check(module.compare([denied],[same])['common_verified_instances']==0)
check(module.compare([first],[changed])['common_verified_instances']==0)
check(module.distribution([1,3])['median']==2)
print(f'independent_analysis checks={checks} failures=0')
