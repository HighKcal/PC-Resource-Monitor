/* 합성 완료창과 단조 시각으로 observe→begin→dispatch→result/click을 검증한다.
 * 같은 사건의 중복 요청, 프로그램별 묶음·억제·재발 규칙은 각 사례에서 바꾼다.
 * 기본 main은 Shell·DB 없이 실행하며 --benchmark는 별도 비용 측정이다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../../notifications/notification_policy.h"

static NotificationPolicy policy;
static MonitorRow rows[NOTIFICATION_TRACK_LIMIT+2];
static MonitorSnapshot snapshot;
static unsigned checks,failures;
static const uint64_t epoch=UINT64_C(134000000000000000);
static const uint64_t recurrence_ms=NOTIFICATION_RECURRENCE_MS;

#define CHECK(expr) do { ++checks; if(!(expr)) { ++failures; printf("FAIL policy %u %s\n",(unsigned)__LINE__,#expr); } } while(0)

static uint64_t utc(uint64_t now) { return epoch+now*UINT64_C(10000); }

static void setup(size_t count)
{
    memset(&policy,0,sizeof(policy));
    memset(&snapshot,0,sizeof(snapshot));
    memset(rows,0,count*sizeof(rows[0]));
    notification_policy_enable(&policy,1);
    snapshot.rows=rows;
    snapshot.count=count;
    for(size_t i=0;i<count;++i) {
        rows[i].raw.pid=(uint32_t)i+100;
        rows[i].raw.creation_time_utc_100ns=epoch-UINT64_C(1000000000)+(uint64_t)i;
        rows[i].raw.valid_fields=RESOURCE_VALID_IDENTITY|RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|
            RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE;
        rows[i].path="C:\\private\\fixture.exe";
        rows[i].program_id=(int64_t)i+1000;
    }
}

static void report(size_t i,unsigned metric,MonitorState state,uint64_t at)
{
    rows[i].metric[metric]=(MonitorMetricReport){.state=state,.current=32,.median=2,.threshold=8,
        .streak=state==MONITOR_CANDIDATE?3u:0u,.window_start_utc_100ns=utc(at)-UINT64_C(600000020),
        .window_end_utc_100ns=utc(at)-20,.window_duration_seconds=60};
}

static void observe(uint64_t at,size_t completed)
{
    ++snapshot.sequence;
    snapshot.completed_windows=completed;
    snapshot.measured_utc_100ns=utc(at);
    for(size_t i=0;i<snapshot.count;++i) rows[i].raw.measured_at_utc_100ns=utc(at)-20;
    notification_policy_observe(&policy,&snapshot,1,at,utc(at));
}

static NotificationEvent request(uint64_t at)
{
    NotificationEvent event={0};
    CHECK(notification_policy_begin(&policy,at,0,&event));
    CHECK(notification_policy_dispatch(&policy,event.id,at,0,&event));
    return event;
}

static void fresh_and_episode(void)
{
    NotificationEvent event;
    char path[]="C:\\private\\copied.exe";
    setup(4);
    rows[0].path=path;
    report(0,MONITOR_CPU,MONITOR_CANDIDATE,1000);
    report(0,MONITOR_RAM,MONITOR_CANDIDATE,1000);
    report(1,MONITOR_CPU,MONITOR_CANDIDATE,999);
    report(2,MONITOR_CPU,MONITOR_CANDIDATE,1000);
    rows[2].metric[0].streak=2;
    report(3,MONITOR_CPU,MONITOR_CANDIDATE,1000);
    rows[3].raw.valid_fields&=~RESOURCE_VALID_IDENTITY;
    observe(1000,3);
    CHECK(policy.count==1 && policy.stats.queued==1);
    path[11]='X';
    rows[0].metric[0].current=90;
    observe(2000,0);
    CHECK(policy.count==1);
    CHECK(!notification_policy_begin(&policy,15999,0,&event));
    event=request(16000);
    CHECK(!strcmp(event.name,"copied.exe") && event.members[0].pid==100 && event.program==1000);
    CHECK(event.metrics==3 && event.members[0].metric[0].current==32 &&
        event.members[0].metric[0].window_end==utc(1000)-20);
    CHECK(policy.stats.requests==1 && policy.stats.accepted==0);
    notification_policy_result(&policy,&event,1,16000);
    CHECK(policy.stats.accepted==1);
    CHECK(!notification_policy_click(&policy,event.id+1,16001,&event));
    CHECK(notification_policy_click(&policy,event.id,16002,&event));

    report(0,MONITOR_CPU,MONITOR_UNAVAILABLE,61000);
    report(0,MONITOR_RAM,MONITOR_CANDIDATE,61000);
    report(0,MONITOR_READ,MONITOR_CANDIDATE,61000);
    observe(61000,1);
    CHECK(policy.count==1 && policy.queue[0].metrics==(1u<<MONITOR_READ));
    event=request(76000);
    notification_policy_result(&policy,&event,1,76000);
    notification_policy_retire(&policy,event.id);
    report(0,MONITOR_CPU,MONITOR_CANDIDATE,121000);
    observe(121000,1);
    CHECK(policy.count==0);
    report(0,MONITOR_CPU,MONITOR_NORMAL,181000);
    observe(181000,1);
    report(0,MONITOR_CPU,MONITOR_CANDIDATE,241000);
    observe(241000,1);
    if(recurrence_ms>225000) CHECK(!policy.count && policy.stats.deferred==1);
    else CHECK(policy.count==1 && policy.queue[0].metrics==(1u<<MONITOR_CPU));
    notification_policy_enable(&policy,0);
    CHECK(!policy.count && !policy.active.id && !policy.track_count);
    notification_policy_enable(&policy,1);
    observe(242000,1);
    CHECK(!policy.count);
    report(0,MONITOR_CPU,MONITOR_CANDIDATE,301000);
    observe(301000,1);
    CHECK(policy.count==1);
}

static void suppression_and_claim(void)
{
    NotificationEvent event={0};
    setup(3);
    rows[1].program_id=rows[0].program_id;
    for(size_t i=0;i<3;++i) report(i,MONITOR_RAM,MONITOR_CANDIDATE,1000);
    observe(1000,3);
    CHECK(policy.count==2 && policy.queue[0].member_count==2 && policy.queue[1].member_count==1);
    observe(16000,0);
    CHECK(!notification_policy_begin(&policy,16000,1000,&event));
    CHECK(notification_policy_begin(&policy,16000,0,&event));
    CHECK(event.members[0].pid==100 && event.members[1].pid==101);
    CHECK(!notification_policy_dispatch(&policy,event.id,16001,1000,&event));
    CHECK(policy.count==2 && !policy.stats.requests);
    CHECK(notification_policy_begin(&policy,16002,0,&event));
    notification_policy_feedback(&policy,1000,MONITOR_FEEDBACK_IGNORE,utc(61000),utc(16003));
    CHECK(!notification_policy_dispatch(&policy,event.id,16003,0,&event));
    CHECK(policy.count==1 && policy.queue[0].program==1002 && policy.stats.feedback_purged==1);
    for(size_t i=0;i<2;++i) {
        rows[i].feedback=MONITOR_FEEDBACK_IGNORE;
        rows[i].feedback_expires_utc_100ns=utc(61000);
        report(i,MONITOR_RAM,MONITOR_CANDIDATE,60000);
    }
    observe(60000,2);
    CHECK(policy.count==1);
    notification_policy_feedback(&policy,1000,MONITOR_FEEDBACK_CANCEL,0,utc(60001));
    observe(61000,1);
    CHECK(policy.count==1);
    report(0,MONITOR_RAM,MONITOR_CANDIDATE,62000);
    observe(62000,1);
    CHECK(policy.count==2);
    event=request(62000);
    notification_policy_result(&policy,&event,1,62000);
    CHECK(event.program==1002);
    notification_policy_feedback(&policy,1002,MONITOR_FEEDBACK_EXPECTED,utc(65000),utc(62001));
    CHECK(!policy.active.id && !notification_policy_click(&policy,event.id,62002,&event));
    report(2,MONITOR_RAM,MONITOR_CANDIDATE,122000);
    observe(122000,1);
    CHECK(policy.count==1 && policy.queue[0].program==1000);
}

static void freshness_and_identity(void)
{
    NotificationEvent event={0};
    setup(2);
    rows[1].raw.pid=rows[0].raw.pid;
    report(0,MONITOR_WRITE,MONITOR_CANDIDATE,1000);
    observe(1000,1);
    CHECK(!policy.count);
    snapshot.count=1;
    report(0,MONITOR_WRITE,MONITOR_CANDIDATE,61000);
    observe(61000,1);
    CHECK(policy.count==1);
    CHECK(notification_policy_begin(&policy,76000,0,&event));
    rows[0].raw.valid_fields&=~RESOURCE_VALID_IO_WRITE;
    observe(77000,0);
    CHECK(!notification_policy_dispatch(&policy,event.id,77000,0,&event));
    CHECK(policy.stats.invalidated==1 && !policy.stats.requests);
    rows[0].raw.valid_fields|=RESOURCE_VALID_IO_WRITE;
    report(0,MONITOR_WRITE,MONITOR_CANDIDATE,121000);
    observe(121000,1);
    CHECK(!notification_policy_begin(&policy,136001,0,&event));
    observe(136002,0);
    event=request(136002);
    notification_policy_result(&policy,&event,1,136002);
    notification_policy_retire(&policy,event.id);
    ++rows[0].raw.creation_time_utc_100ns;
    report(0,MONITOR_WRITE,MONITOR_CANDIDATE,181000);
    observe(181000,1);
    CHECK(policy.count==1 && policy.queue[0].members[0].creation!=event.members[0].creation);
    snapshot.collector_error=1;
    observe(182000,0);
    CHECK(!policy.count && !policy.active.id && !policy.track_count);
    snapshot.collector_error=0;
    snapshot.synthetic=1;
    report(0,MONITOR_WRITE,MONITOR_CANDIDATE,241000);
    observe(241000,1);
    CHECK(!policy.count);
}

static void bounded_backoff(void)
{
    NotificationEvent event={0};
    setup(NOTIFICATION_TRACK_LIMIT+2);
    for(size_t i=0;i<snapshot.count;++i) report(i,MONITOR_CPU,MONITOR_CANDIDATE,1000);
    observe(1000,snapshot.count);
    CHECK(policy.count==NOTIFICATION_QUEUE_LIMIT && policy.track_count==NOTIFICATION_TRACK_LIMIT);
    CHECK(policy.stats.overflow==NOTIFICATION_TRACK_LIMIT-NOTIFICATION_QUEUE_LIMIT && policy.stats.tracking_drops==2);
    event=request(16000);
    notification_policy_result(&policy,&event,0,16001);
    CHECK(policy.stats.failed==1 && !policy.stats.accepted && !policy.active.id);
    observe(45999,0);
    CHECK(!notification_policy_begin(&policy,45999,0,&event));
    event=request(46000);
    notification_policy_result(&policy,&event,1,46000);
    notification_policy_retire(&policy,event.id);
    observe(76000,0);
    event=request(76000);
    notification_policy_result(&policy,&event,1,76000);
    notification_policy_retire(&policy,event.id);
    observe(91000,0);
    CHECK(!policy.count && policy.stats.expired==NOTIFICATION_QUEUE_LIMIT-3);
    report(0,MONITOR_CPU,MONITOR_CANDIDATE,121000);
    report(3,MONITOR_CPU,MONITOR_CANDIDATE,121000);
    report(32,MONITOR_CPU,MONITOR_CANDIDATE,121000);
    observe(121000,3);
    CHECK(!policy.count);
    report(0,MONITOR_CPU,MONITOR_CANDIDATE,301000);
    report(32,MONITOR_CPU,MONITOR_CANDIDATE,301000);
    observe(301000,2);
    CHECK(policy.count==1 && policy.queue[0].members[0].pid==132);
    report(0,MONITOR_CPU,MONITOR_CANDIDATE,361000);
    observe(361000,1);
    CHECK(policy.count==2 && policy.queue[1].members[0].pid==100);
    report(3,MONITOR_CPU,MONITOR_CANDIDATE,391000);
    observe(391000,1);
    CHECK(policy.stats.expired==NOTIFICATION_QUEUE_LIMIT-2 && policy.queue[1].members[0].pid==103);
}

static uint64_t process_time(void)
{
    FILETIME created,exited,kernel,user;
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) return 0;
    return (((uint64_t)kernel.dwHighDateTime<<32)|kernel.dwLowDateTime)+
        (((uint64_t)user.dwHighDateTime<<32)|user.dwLowDateTime);
}

static void measure(const char *name,unsigned iterations,int completed,int candidate,int reset)
{
    LARGE_INTEGER start,end,frequency;
    uint64_t cpu_start,cpu_end;
    setup(500);
    for(size_t i=0;i<snapshot.count;++i) {
        rows[i].raw.measured_at_utc_100ns=utc(1000)-20;
        report(i,MONITOR_CPU,candidate?MONITOR_CANDIDATE:MONITOR_NORMAL,1000);
    }
    snapshot.measured_utc_100ns=utc(1000);
    if(!completed && candidate) observe(1000,snapshot.count);
    snapshot.completed_windows=completed?snapshot.count:0;
    QueryPerformanceFrequency(&frequency);
    cpu_start=process_time();
    QueryPerformanceCounter(&start);
    for(unsigned i=0;i<iterations;++i) {
        if(reset) notification_policy_reset(&policy);
        ++snapshot.sequence;
        notification_policy_observe(&policy,&snapshot,1,1000,utc(1000));
    }
    QueryPerformanceCounter(&end);
    cpu_end=process_time();
    printf("bench=%s rows=500 n=%u wall_us_call=%.6f cpu_us_call=%.6f queued=%u tracks=%u\n",
        name,iterations,(double)(end.QuadPart-start.QuadPart)*1000000/(double)frequency.QuadPart/(double)iterations,
        (double)(cpu_end-cpu_start)/10/(double)iterations,policy.count,policy.track_count);
}

int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--benchmark")) {
        printf("policy_bytes=%zu event_bytes=%zu track_bytes=%zu\n",sizeof(policy),sizeof(NotificationEvent),sizeof(NotificationTrack));
        measure("ordinary-empty",3000000,0,0,0);
        measure("completed-no-candidates",25000,1,0,0);
        measure("completed-500-candidates",4000,1,1,1);
        measure("ordinary-sustained-queue",25000,0,1,0);
        return 0;
    }
    fresh_and_episode();
    suppression_and_claim();
    freshness_and_identity();
    bounded_backoff();
    printf("notification policy checks=%u failures=%u policy_bytes=%zu\n",checks,failures,sizeof(policy));
    return failures!=0;
}
