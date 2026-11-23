/* 완료 구간 표시 회귀 전용 어댑터다. bridge/engine 구현을 그대로 호출하되
 * monitor_ingest 직전의 합성 표본만 바꾼다. 제품 DLL에는 이 파일을 연결하지 않는다. */
#include "../../../wpf/native/bridge.h"

static int completed_fixture_ingest(MonitorEngine *,const ResourceBatch *,
    const char *const *,MonitorSnapshot **);
#define monitor_ingest completed_fixture_ingest
#include "../../../wpf/native/bridge.c"
#undef monitor_ingest

static unsigned fixture_sequence;
static int fixture_mode;
static NotificationPolicy fixture_policy;

/* 한 시험 프로세스의 Engine을 순차로 열기 전에 호출한다.
 * 0: 143회 CPU 결측, 1: 182회 후보 뒤 결측, 2: 유효한 0,
 * 3: 144회 PID 재사용(새 생성시각), 4: 첫 완료창 이전 결측,
 * 5: 15초 한도를 넘는 UTC 순방향 16초 간격, 6: UTC 역행,
 * 7: 다음 완료창의 CPU 전부 결측. */
__declspec(dllexport) void completed_fixture_mode(int mode)
{
    fixture_mode=mode; fixture_sequence=0;
    memset(&fixture_policy,0,sizeof(fixture_policy));
    notification_policy_enable(&fixture_policy,1);
}

/* 합성 제품 경로는 실제 알림을 억제한다. 이 대조에서만 이미 커밋된 snapshot의
 * 값 사본에서 synthetic 표지를 지워 동일 C 정책을 검사하며 Shell은 호출하지 않는다. */
__declspec(dllexport) int completed_fixture_policy(Bridge *b,int begin)
{
    MonitorSnapshot input;
    NotificationEvent event={0};
    uint64_t now;
    if(!worker(b) || !b->snapshot) return -1;
    input=*b->snapshot; input.synthetic=0;
    now=input.sequence*UINT64_C(3000);
    notification_policy_observe(&fixture_policy,&input,b->session,now,input.measured_utc_100ns);
    if(begin<0) return (int)fixture_policy.count+(fixture_policy.active.id!=0);
    return begin?notification_policy_begin(&fixture_policy,now,0,&event):0;
}

static int completed_fixture_ingest(MonitorEngine *engine,const ResourceBatch *batch,
    const char *const *paths,MonitorSnapshot **result)
{
    ResourceBatch input=*batch;
    ResourceData row;
    ++fixture_sequence;
    if(batch->count!=1) return monitor_ingest(engine,batch,paths,result);
    row=batch->items[0]; input.items=&row;
    if((fixture_mode==0 && fixture_sequence==143) ||
        (fixture_mode==1 && fixture_sequence==182) ||
        (fixture_mode==4 && fixture_sequence==2)) row.valid_fields&=~RESOURCE_VALID_CPU;
    if(fixture_mode==2) row.cpu_percent=0;
    if(fixture_mode==3 && fixture_sequence>=144) row.creation_time_utc_100ns+=UINT64_C(4300000000);
    if(fixture_mode==5 && fixture_sequence>=143) {
        /* 기본 3초 간격에 13초를 더한다. 이후 회차도 같은 UTC 이동을 유지해
         * 143회만 16초, 144회부터는 다시 3초가 되도록 한다. */
        row.measured_at_utc_100ns+=UINT64_C(130000000);
        input.started_at_utc_100ns+=UINT64_C(130000000);
        input.finished_at_utc_100ns+=UINT64_C(130000000);
        if(fixture_sequence==143) {
            row.observation_interval_seconds=row.io_interval_seconds=16;
            row.valid_fields&=~RESOURCE_VALID_CPU;
        }
    }
    if(fixture_mode==6 && fixture_sequence==143) {
        row.measured_at_utc_100ns-=UINT64_C(60000000);
        input.started_at_utc_100ns-=UINT64_C(60000000);
        input.finished_at_utc_100ns-=UINT64_C(60000000);
        row.valid_fields&=~RESOURCE_VALID_CPU;
    }
    if(fixture_mode==7 && fixture_sequence>=142) row.valid_fields&=~RESOURCE_VALID_CPU;
    return monitor_ingest(engine,&input,paths,result);
}
