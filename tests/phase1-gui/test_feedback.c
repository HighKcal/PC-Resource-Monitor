/* GUI 내부 전달 경로의 회귀 시험이다. 표시 창·DB·실제 수집기는 실행하지 않는다.
   PHASE1_BEFORE는 보존한 수정 전 입력에서 원래 두 결함의 재현 조건을 검사한다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include "monitor.h"
#include "resource_display.h"
static int test_open(const char *,int,MonitorEngine **,char *,size_t);
static void test_close(MonitorEngine *);
static void test_reset(MonitorEngine *);
static int test_ingest(MonitorEngine *,const ResourceBatch *,const char *const *,MonitorSnapshot **);
static int test_feedback(MonitorEngine *,int64_t,MonitorFeedback,uint64_t);
static const char *test_error(const MonitorEngine *);
static void *test_calloc(size_t,size_t);
#define monitor_open test_open
#define monitor_close test_close
#define monitor_reset test_reset
#define monitor_ingest test_ingest
#define monitor_feedback test_feedback
#define monitor_error test_error
#define calloc test_calloc
#define wmain unused_gui_wmain
#ifdef PHASE1_BEFORE
#include "../../build-phase1-20261104/before/gui/monitor_gui.c"
#else
#include "../../gui/monitor_gui.c"
#endif
#undef wmain
#undef monitor_open
#undef monitor_close
#undef monitor_reset
#undef monitor_ingest
#undef monitor_feedback
#undef monitor_error
#undef calloc

static unsigned checks,failures,fail_write,fail_open,fail_allocation,stop_after_write;
static volatile unsigned writes;
static HANDLE written,release_write;
#define CHECK(v) do { ++checks; if(!(v)) { ++failures; printf("FAIL line=%u %s\n",(unsigned)__LINE__,#v); } } while(0)
static int test_open(const char *path,int synthetic,MonitorEngine **out,char *error,size_t size)
{
    (void)path; (void)synthetic;
    if(fail_open) { snprintf(error,size,"injected open failure"); return 0; }
    *out=(MonitorEngine *)malloc(1); return *out!=NULL;
}
static void test_close(MonitorEngine *engine) { free(engine); }
static void test_reset(MonitorEngine *engine) { (void)engine; }
static int test_ingest(MonitorEngine *engine,const ResourceBatch *batch,const char *const *paths,MonitorSnapshot **out)
{
    (void)engine; (void)batch; (void)paths; *out=calloc(1,sizeof(**out)); return *out!=NULL;
}
static const char *test_error(const MonitorEngine *engine) { (void)engine; return "injected feedback failure"; }
static void *test_calloc(size_t count,size_t size)
{
    if(fail_allocation) { --fail_allocation; return NULL; }
    return calloc(count,size);
}
static int test_feedback(MonitorEngine *engine,int64_t program,MonitorFeedback kind,uint64_t now)
{
    (void)engine; (void)program; (void)kind; (void)now;
    ++writes;
    if(written) { SetEvent(written); CHECK(WaitForSingleObject(release_write,5000)==WAIT_OBJECT_0); }
    if(stop_after_write && writes==stop_after_write) SetEvent(app.stop);
    return writes!=fail_write;
}
static void init_fixture(void)
{
    memset(&app,0,sizeof(app)); InitializeSRWLock(&app.lock);
    app.stop=CreateEventW(NULL,TRUE,FALSE,NULL); app.wake=CreateEventW(NULL,TRUE,FALSE,NULL);
    app.synthetic=1; app.pause_requested=1; app.session=1; app.interval_ms=1;
    app.shown=calloc(1,sizeof(*app.shown)); app.shown->metrics=calloc(1,sizeof(*app.shown->metrics));
    app.display=app.shown->metrics; app.display->rows=calloc(32,sizeof(*app.display->rows)); app.display->count=32;
    app.shown->session=1;
    for(unsigned i=0;i<32;++i) app.display->rows[i].program_id=100+i;
    writes=fail_write=fail_open=fail_allocation=stop_after_write=0;
    written=release_write=NULL;
}
static void finish_fixture(void)
{
    SetEvent(app.stop); SetEvent(app.wake);
    if(app.worker) { CHECK(WaitForSingleObject(app.worker,5000)==WAIT_OBJECT_0); CloseHandle(app.worker); }
#ifndef PHASE1_BEFORE
    consume_feedback(); CHECK(app.feedback_outstanding==0); CHECK(app.command_count==0);
    feedback_dispose(); CHECK(!app.feedback_overlay); CHECK(app.feedback_overlay_count==0);
#endif
    gui_snapshot_free(app.latest); gui_snapshot_free(app.shown);
    CloseHandle(app.stop); CloseHandle(app.wake); app.worker=NULL;
}
static void wait_results(unsigned count)
{
    unsigned actual=0;
    uint64_t deadline=GetTickCount64()+5000;
    do {
        AcquireSRWLockShared(&app.lock); actual=app.feedback_result_count; ReleaseSRWLockShared(&app.lock);
        if(actual>=count) break;
        Sleep(1);
    } while(GetTickCount64()<deadline);
    CHECK(actual==count);
}
static void start_worker(void)
{ app.worker=CreateThread(NULL,0,worker_main,NULL,0,NULL); CHECK(app.worker!=NULL); }

#ifdef PHASE1_BEFORE
static void original_ack_oracle(void)
{
    init_fixture(); start_worker();
    AcquireSRWLockExclusive(&app.lock);
    for(unsigned i=0;i<16;++i) app.commands[i]=(FeedbackCommand){100+i,MONITOR_FEEDBACK_IGNORE,0};
    app.command_count=16; ReleaseSRWLockExclusive(&app.lock); SetEvent(app.wake); wait_results(16);
    AcquireSRWLockExclusive(&app.lock);
    app.commands[0]=(FeedbackCommand){116,MONITOR_FEEDBACK_EXPECTED,0}; app.command_count=1;
    ReleaseSRWLockExclusive(&app.lock); SetEvent(app.wake);
    while(writes<17) Sleep(1);
    SetEvent(app.stop); CHECK(WaitForSingleObject(app.worker,5000)==WAIT_OBJECT_0);
    /* 저장한 모든 명령은 응답이 필요하다. 수정 전 코드는 17번째 응답을 잃는다. */
    CHECK(app.feedback_result_count==writes);
    printf("BEFORE ack writes=%u results=%u\n",writes,app.feedback_result_count); finish_fixture();
}
static void original_overlay_oracle(void)
{
    uint64_t expires;
    init_fixture();
    for(unsigned i=0;i<17;++i) {
        app.feedback_results[0]=(FeedbackCommand){100+i,MONITOR_FEEDBACK_IGNORE,monitor_utc_now()+UINT64_C(36000000000)};
        app.feedback_result_count=1; consume_feedback();
    }
    CHECK(displayed_feedback(&app.display->rows[0],&expires)==MONITOR_FEEDBACK_IGNORE);
    printf("BEFORE overlay count=%u first=%u\n",app.feedback_overlay_count,(unsigned)displayed_feedback(&app.display->rows[0],&expires));
    finish_fixture();
}
#else
static void complete_queued(FeedbackOutcome outcome,uint64_t revision)
{
    FeedbackCommand command;
    CHECK(app.command_count==1);
    command=app.commands[0]; app.command_count=0;
    feedback_complete(command,outcome,monitor_utc_now(),revision); consume_feedback();
}
static void test_outstanding(void)
{
    init_fixture();
    for(unsigned i=0;i<16;++i) CHECK(feedback_submit(100+i,MONITOR_FEEDBACK_IGNORE)==FEEDBACK_ACCEPTED);
    start_worker(); wait_results(16);
    CHECK(writes==16); CHECK(app.command_count==0); CHECK(app.feedback_outstanding==16);
    CHECK(feedback_submit(116,MONITOR_FEEDBACK_EXPECTED)==FEEDBACK_BUSY);
    CHECK(app.feedback_overlay_count==16);
    for(unsigned i=0;i<16;++i) {
        CHECK(app.feedback_results[i].request_id==i+1); CHECK(app.feedback_results[i].revision==i+1);
        CHECK(app.feedback_results[i].outcome==FEEDBACK_SUCCEEDED);
    }
    consume_feedback(); CHECK(app.feedback_outstanding==0); consume_feedback(); CHECK(app.feedback_outstanding==0);
    CHECK(feedback_submit(116,MONITOR_FEEDBACK_EXPECTED)==FEEDBACK_ACCEPTED); wait_results(1);
    CHECK(writes==17); CHECK(app.feedback_results[0].request_id==17);
    consume_feedback(); CHECK(app.feedback_overlay_count==17);
    for(unsigned i=0;i<17;++i) {
        uint64_t expires;
        CHECK(displayed_feedback(&app.display->rows[i],&expires)==(i==16?MONITOR_FEEDBACK_EXPECTED:MONITOR_FEEDBACK_IGNORE));
    }
    finish_fixture();
}
static void test_allocation_and_rejection(void)
{
    init_fixture(); fail_allocation=1;
    CHECK(feedback_submit(100,MONITOR_FEEDBACK_IGNORE)==FEEDBACK_NO_MEMORY);
    CHECK(!app.feedback_outstanding && !app.command_count && !app.feedback_overlay_count && !writes);
    CHECK(feedback_submit(100,MONITOR_FEEDBACK_IGNORE)==FEEDBACK_ACCEPTED);
    /* 같은 program의 후속 요청은 이미 있는 표시 노드를 재사용하므로 할당이 필요 없다. */
    fail_allocation=1; CHECK(feedback_submit(100,MONITOR_FEEDBACK_CANCEL)==FEEDBACK_ACCEPTED);
    CHECK(fail_allocation==1); fail_allocation=0;
    feedback_finish(); CHECK(app.feedback_result_count==2);
    CHECK(app.feedback_results[0].outcome==FEEDBACK_CANCELLED); CHECK(app.feedback_results[1].outcome==FEEDBACK_CANCELLED);
    consume_feedback(); CHECK(app.feedback_overlay_count==0);
    CHECK(feedback_submit(101,MONITOR_FEEDBACK_IGNORE)==FEEDBACK_CLOSED); CHECK(!app.feedback_overlay_count);
    finish_fixture();
    init_fixture(); CHECK(feedback_submit(100,MONITOR_FEEDBACK_IGNORE)==FEEDBACK_ACCEPTED);
    fail_allocation=1; complete_queued(FEEDBACK_SUCCEEDED,1);
    CHECK(fail_allocation==1); CHECK(app.feedback_overlay_count==1); CHECK(app.feedback_outstanding==0);
    fail_allocation=0; finish_fixture();
}
static void test_terminal_failures(void)
{
    for(unsigned scenario=0;scenario<4;++scenario) {
        init_fixture();
        for(unsigned i=0;i<3;++i) CHECK(feedback_submit(100+i,MONITOR_FEEDBACK_IGNORE)==FEEDBACK_ACCEPTED);
        if(scenario==0) fail_open=1;
        if(scenario==1) fail_write=2;
        if(scenario==2) SetEvent(app.stop);
        if(scenario==3) stop_after_write=1;
        start_worker();
        if(scenario==1 || scenario==0) CHECK(WaitForSingleObject(app.worker,5000)==WAIT_OBJECT_0);
        wait_results(3);
        CHECK(writes==(scenario==1?2u:scenario==3?1u:0u)); CHECK(app.command_count==0);
        for(unsigned i=0;i<3;++i) {
            FeedbackOutcome expected=(scenario==1 || scenario==3) && i==0?FEEDBACK_SUCCEEDED:
                scenario==1 && i==1?FEEDBACK_FAILED:FEEDBACK_CANCELLED;
            CHECK(app.feedback_results[i].request_id==i+1); CHECK(app.feedback_results[i].outcome==expected);
        }
        CHECK(feedback_submit(104,MONITOR_FEEDBACK_EXPECTED)==FEEDBACK_CLOSED);
        finish_fixture();
    }
}
static void test_overlay_lifetime(void)
{
    uint64_t expires,now=monitor_utc_now();
    init_fixture(); app.display->rows[0].feedback=MONITOR_FEEDBACK_IGNORE;
    app.display->rows[0].feedback_expires_utc_100ns=now+UINT64_C(36000000000);
    CHECK(feedback_submit(100,MONITOR_FEEDBACK_EXPECTED)==FEEDBACK_ACCEPTED); complete_queued(FEEDBACK_SUCCEEDED,1);
    CHECK(displayed_feedback(&app.display->rows[0],&expires)==MONITOR_FEEDBACK_EXPECTED);
    ++app.session; feedback_reconcile();
    CHECK(displayed_feedback(&app.display->rows[0],&expires)==MONITOR_FEEDBACK_EXPECTED);
    CHECK(feedback_submit(100,MONITOR_FEEDBACK_CANCEL)==FEEDBACK_ACCEPTED); complete_queued(FEEDBACK_FAILED,0);
    CHECK(displayed_feedback(&app.display->rows[0],&expires)==MONITOR_FEEDBACK_EXPECTED);
    CHECK(feedback_submit(100,MONITOR_FEEDBACK_CANCEL)==FEEDBACK_ACCEPTED); complete_queued(FEEDBACK_SUCCEEDED,2);
    CHECK(displayed_feedback(&app.display->rows[0],&expires)==MONITOR_FEEDBACK_CANCEL); CHECK(expires==0);
    app.shown->feedback_revision=2; app.shown->feedback_current=0; feedback_reconcile();
    CHECK(app.feedback_overlay_count==1); /* 오류 frame은 성공 표시의 제거 근거가 아니다. */
    app.shown->feedback_current=1; app.shown->feedback_revision=1; feedback_reconcile();
    CHECK(app.feedback_overlay_count==1); /* 쓰기 전 정상 frame도 성공 표시를 지우기에는 오래됐다. */
    app.display->rows[0].feedback=MONITOR_FEEDBACK_CANCEL; app.display->rows[0].feedback_expires_utc_100ns=0;
    app.shown->feedback_revision=2; feedback_reconcile(); CHECK(app.feedback_overlay_count==0);
    CHECK(feedback_submit(100,MONITOR_FEEDBACK_EXPECTED)==FEEDBACK_ACCEPTED); complete_queued(FEEDBACK_SUCCEEDED,3);
    feedback_overlay_find(100)->expires=now-1; feedback_reconcile();
    CHECK(displayed_feedback(&app.display->rows[0],&expires)==MONITOR_FEEDBACK_CANCEL); CHECK(app.feedback_overlay_count==1);
    /* 만료 후에도 취소 상태를 남겨 오래된 snapshot의 활성 억제가 되살아나지 않게 한다. */
    app.display->rows[0].feedback=MONITOR_FEEDBACK_IGNORE; app.display->rows[0].feedback_expires_utc_100ns=now+UINT64_C(36000000000);
    CHECK(displayed_feedback(&app.display->rows[0],&expires)==MONITOR_FEEDBACK_CANCEL);
    /* 벽시계 UTC 값으로 저장 순서를 추론하지 않는다. */
    app.display->measured_utc_100ns=UINT64_MAX; feedback_reconcile(); CHECK(app.feedback_overlay_count==1);
    app.display->measured_utc_100ns=1; app.shown->feedback_revision=3;
    app.display->rows[0].feedback=MONITOR_FEEDBACK_CANCEL; app.display->rows[0].feedback_expires_utc_100ns=0;
    feedback_reconcile(); CHECK(app.feedback_overlay_count==0);
    /* 성공을 반영한 frame이 저장 응답보다 먼저 UI에 도착할 수도 있다. */
    CHECK(feedback_submit(100,MONITOR_FEEDBACK_EXPECTED)==FEEDBACK_ACCEPTED);
    app.shown->feedback_revision=4; app.display->rows[0].feedback=MONITOR_FEEDBACK_EXPECTED;
    app.display->rows[0].feedback_expires_utc_100ns=now+UINT64_C(864000000000);
    ++app.session; complete_queued(FEEDBACK_SUCCEEDED,4); CHECK(app.feedback_overlay_count==0);
    CHECK(displayed_feedback(&app.display->rows[0],&expires)==MONITOR_FEEDBACK_EXPECTED);
    /* 표시에서 사라진 대상도 접수 중에는 노드를 예약하고 응답 종결 뒤 정리한다. */
    CHECK(feedback_submit(900,MONITOR_FEEDBACK_IGNORE)==FEEDBACK_ACCEPTED); feedback_reconcile(); CHECK(app.feedback_overlay_count==1);
    complete_queued(FEEDBACK_SUCCEEDED,5); CHECK(app.feedback_overlay_count==0);
    finish_fixture();
}
static void test_stop_while_executing(void)
{
    init_fixture(); written=CreateEventW(NULL,TRUE,FALSE,NULL); release_write=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(feedback_submit(100,MONITOR_FEEDBACK_IGNORE)==FEEDBACK_ACCEPTED); start_worker();
    CHECK(WaitForSingleObject(written,5000)==WAIT_OBJECT_0);
    CHECK(feedback_submit(101,MONITOR_FEEDBACK_EXPECTED)==FEEDBACK_ACCEPTED);
    SetEvent(app.stop); SetEvent(release_write); CHECK(WaitForSingleObject(app.worker,5000)==WAIT_OBJECT_0);
    CHECK(writes==1); CHECK(app.feedback_result_count==2);
    CHECK(app.feedback_results[0].outcome==FEEDBACK_SUCCEEDED); CHECK(app.feedback_results[1].outcome==FEEDBACK_CANCELLED);
    CloseHandle(written); CloseHandle(release_write); written=release_write=NULL; finish_fixture();
}
static void test_postcommit_frame_oom(void)
{
    uint64_t expires;
    init_fixture(); app.pause_requested=0; app.count=1;
    CHECK(feedback_submit(100,MONITOR_FEEDBACK_IGNORE)==FEEDBACK_ACCEPTED);
    fail_allocation=1; start_worker(); CHECK(WaitForSingleObject(app.worker,5000)==WAIT_OBJECT_0);
    CHECK(app.worker_error); CHECK(!app.latest); CHECK(fail_allocation==0);
    CHECK(app.feedback_result_count==1); CHECK(app.feedback_results[0].outcome==FEEDBACK_SUCCEEDED);
    consume_feedback(); CHECK(displayed_feedback(&app.display->rows[0],&expires)==MONITOR_FEEDBACK_IGNORE);
    finish_fixture();
}
static void test_empty_error_frame(void)
{
    uint64_t expires,now=monitor_utc_now();
    MonitorRow prior={0}; GuiSnapshot *normal;
    init_fixture(); prior.program_id=100; prior.feedback=MONITOR_FEEDBACK_IGNORE;
    prior.feedback_expires_utc_100ns=now+UINT64_C(36000000000);
    CHECK(feedback_submit(100,MONITOR_FEEDBACK_EXPECTED)==FEEDBACK_ACCEPTED); complete_queued(FEEDBACK_SUCCEEDED,1);
    /* 원본 행이 0개인 실제 제품 오류 wrapper와 소비 경로로 성공 표시 보존을 검사한다. */
    publish(error_snapshot("injected storage failure",1)); consume_snapshot();
    CHECK(app.display->storage_error); CHECK(app.display->count==0); CHECK(!app.shown->feedback_current);
    CHECK(app.feedback_overlay_count==1); CHECK(feedback_overlay_find(100) && feedback_overlay_find(100)->revision==1);
    CHECK(displayed_feedback(&prior,&expires)==MONITOR_FEEDBACK_EXPECTED);
    /* 쓰기 이전의 정상 frame이 늦게 도착해도 성공 표시를 지울 수 없다. */
    normal=calloc(1,sizeof(*normal)); normal->metrics=calloc(1,sizeof(*normal->metrics));
    normal->metrics->rows=calloc(1,sizeof(*normal->metrics->rows)); normal->metrics->count=1;
    normal->metrics->rows[0].program_id=100; normal->metrics->rows[0].name=_strdup("phase1 fixture.exe");
    normal->metrics->rows[0].raw.process_name_utf8=normal->metrics->rows[0].name;
    normal->session=app.session; normal->feedback_current=1;
    publish_view(normal); consume_snapshot(); CHECK(app.feedback_overlay_count==1);
    CHECK(feedback_submit(100,MONITOR_FEEDBACK_CANCEL)==FEEDBACK_ACCEPTED);
    publish(error_snapshot("injected collector failure",0)); consume_snapshot();
    CHECK(app.display->collector_error && app.display->count==0);
    ++app.session; complete_queued(FEEDBACK_SUCCEEDED,2);
    CHECK(app.feedback_overlay_count==1); CHECK(displayed_feedback(&prior,&expires)==MONITOR_FEEDBACK_CANCEL);
    CHECK(expires==0);
    /* 취소 저장을 반영한 후속 정상 frame부터 해당 취소 표시를 정리할 수 있다. */
    normal=calloc(1,sizeof(*normal)); normal->metrics=calloc(1,sizeof(*normal->metrics));
    normal->metrics->rows=calloc(1,sizeof(*normal->metrics->rows)); normal->metrics->count=1;
    normal->metrics->rows[0].program_id=100; normal->metrics->rows[0].name=_strdup("phase1 fixture.exe");
    normal->metrics->rows[0].raw.process_name_utf8=normal->metrics->rows[0].name;
    normal->session=app.session; normal->feedback_current=1; normal->feedback_revision=2;
    publish_view(normal); consume_snapshot(); CHECK(app.feedback_overlay_count==0);
    CHECK(displayed_feedback(&app.display->rows[0],&expires)==MONITOR_FEEDBACK_CANCEL);
    finish_fixture();
}
#endif
int main(void)
{
#ifdef PHASE1_BEFORE
    original_ack_oracle(); original_overlay_oracle();
#else
    test_outstanding(); test_allocation_and_rejection(); test_terminal_failures(); test_overlay_lifetime(); test_stop_while_executing(); test_postcommit_frame_oom(); test_empty_error_frame();
#endif
    printf("phase1 GUI feedback checks=%u failures=%u\n",checks,failures); return failures?1:0;
}
