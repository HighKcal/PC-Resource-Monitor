/* WPF worker가 수집·저장/학습·관계·소속·알림 정책을 호출하는 어댑터다. bridge_collect의
 * 한 회차 수집 → 커밋 → 알림 관측 → 표시 묶음 교체를 먼저 읽는다. Bridge가 native
 * 자원을 소유하고 Engine.cs가 반환 포인터의 값·문자열을 복사한 뒤 UI에 발행한다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bridge.h"
#include "../../collector/resource_private_display.h"

struct Bridge {
    MonitorEngine *engine;
    ResourceCollector *collector;
    PgTracker *tracker;
    PgMembership *membership;
    MonitorSnapshot *snapshot;
    ResourceDisplayBatch display;
    uint64_t *private_working_set;
    PgSnapshot *relations;
    PgGroups *groups;
    BridgeView view;
    NotificationPolicy notifications;
    NotificationEvent event, dispatched, clicked;
    unsigned session;
    int paused, demo, synthetic, failed;
    int group_refresh_pending;
    uint64_t revision, demo_sequence, demo_window, synthetic_start;
    unsigned synthetic_cycle;
    DWORD thread;
    char error[512];
    char failure_error[512];
};
static volatile LONG session_counter;
static unsigned next_session(void) { return (unsigned)InterlockedIncrement(&session_counter); }
static const BridgeView *demo_snapshot(Bridge *,int trigger);

static int worker(Bridge *b)
{
    if(!b) return 0;
    if(b->thread==GetCurrentThreadId()) return 1;
    snprintf(b->error,sizeof(b->error),"Engine calls must remain on the creating worker thread");
    return 0;
}
static void release_view(Bridge *b)
{
    pg_groups_free(b->groups); b->groups=NULL;
    pg_snapshot_free(b->relations); b->relations=NULL;
    resource_display_batch_free(&b->display);
    free(b->private_working_set); b->private_working_set=NULL;
    monitor_snapshot_free(b->snapshot); b->snapshot=NULL;
}
static const BridgeView *view(Bridge *b)
{
    b->view.snapshot=b->snapshot; b->view.display=&b->display;
    b->view.relations=b->relations; b->view.groups=b->groups;
    b->view.session=b->session; b->view.paused=b->paused;
    b->view.feedback_revision=b->revision;
    b->view.target_id=pg_target_id(b->membership);
    b->view.target_label=pg_target_label(b->membership);
    b->view.private_working_set=b->private_working_set;
    b->view.retention_pending=monitor_retention_pending(b->engine);
    return &b->view;
}
static int rebuild(Bridge *b)
{
    PgGroups *groups=NULL;
    if(!b->snapshot || !b->relations) return 0;
    if(!pg_groups_build(b->membership,b->snapshot,b->relations,&groups)) {
        snprintf(b->error,sizeof(b->error),"Group construction allocation failed"); return 0;
    }
    pg_groups_free(b->groups); b->groups=groups;
    /* 실제 최신 소속으로 만든 목록을 교체한 경우에만 대기가 끝난다. 관계 조회 실패나
     * 같은 view 재발행은 이 지점에 도달하지 않는다. 빈 정상 목록도 성공한 갱신이다. */
    b->group_refresh_pending=0;
    /* relations가 존재하면 resolver의 전체 실패는 아니다. 이전 목록 할당 실패 경고만
     * 해제하며 개별 관계의 grade/error와 probe_error는 그대로 둔다. */
    b->view.grouping_failed=0;
    return 1;
}
static void cancel_dispatch(Bridge *b)
{
    /* Pause나 알림 해제가 비동기 Shell 요청보다 먼저 처리돼도, dispatch한 요청에는
     * 결과를 정확히 한 번 돌려줘야 한다. 아직 남은 요청을 실패 결과로 종결한다. */
    if(b->dispatched.id) notification_policy_result(&b->notifications,&b->dispatched,0,GetTickCount64());
    b->dispatched=(NotificationEvent){0};
}
BRIDGE_API Bridge *bridge_open(const char *path,int mode,char *error,size_t capacity)
{
    uint32_t os_error=0;
    Bridge *b=calloc(1,sizeof(*b));
    if(!b) { if(error && capacity) snprintf(error,capacity,"Engine allocation failed"); return NULL; }
    b->thread=GetCurrentThreadId(); b->session=next_session(); b->demo=mode==2; b->synthetic=mode==1;
    b->synthetic_start=monitor_utc_now();
    b->tracker=pg_tracker_create(); b->membership=pg_membership_create();
    if(!b->tracker || !b->membership) snprintf(b->error,sizeof(b->error),"Grouping allocation failed");
    else if(!b->demo && !monitor_open(path,b->synthetic,&b->engine,b->error,sizeof(b->error))) { }
    else if(!b->demo && !b->synthetic && resource_collector_create(NULL,&b->collector,&os_error)!=RESOURCE_OK)
        snprintf(b->error,sizeof(b->error),"Collector initialization failed: Win32 %lu",(unsigned long)os_error);
    if(b->error[0]) {
        if(error && capacity) snprintf(error,capacity,"%s",b->error);
        bridge_close(b); return NULL;
    }
    notification_policy_enable(&b->notifications,1);
    return b;
}
BRIDGE_API void bridge_close(Bridge *b)
{
    if(!worker(b)) return;
    release_view(b); pg_membership_destroy(b->membership); pg_tracker_destroy(b->tracker);
    resource_collector_destroy(b->collector); monitor_close(b->engine); free(b);
}
BRIDGE_API const char *bridge_error(Bridge *b) { return b?b->error:"Engine is closed"; }
BRIDGE_API int bridge_failed(Bridge *b) { return b && b->failed; }
BRIDGE_API int bridge_group_refresh_pending(Bridge *b)
{
    /* 상태 조회는 오류 버퍼를 고치거나 목록을 재구성하지 않는다. */
    return b && b->thread==GetCurrentThreadId() ? b->group_refresh_pending : 0;
}
BRIDGE_API const BridgeView *bridge_current(Bridge *b) { return worker(b)?view(b):NULL; }
BRIDGE_API int bridge_completed_windows(Bridge *b,unsigned session,uint64_t sequence,MonitorCompletedWindow *out,size_t count)
{
    if(!worker(b)) return 0;
    if(session!=b->session || !b->snapshot || sequence!=b->snapshot->sequence ||
        count!=b->snapshot->count || (count && !out)) goto invalid;
    if(b->demo) {
        for(size_t i=0;i<count;++i) {
            memset(&out[i],0,sizeof(out[i]));
            for(unsigned m=0;m<MONITOR_METRICS;++m) {
                const MonitorMetricReport *r=&b->snapshot->rows[i].metric[m];
                if(r->window_end_utc_100ns>r->window_start_utc_100ns) {
                    out[i].states|=(uint32_t)(r->state+1)<<(m*4);
                    out[i].start=r->window_start_utc_100ns; out[i].end=r->window_end_utc_100ns;
                    out[i].duration=r->window_duration_seconds;
                    out[i].metric[m]=(MonitorCompletedMetric){r->current,r->median,r->threshold};
                }
            }
        }
        return 1;
    }
    if(monitor_completed_windows(b->engine,b->snapshot,out,count)) return 1;
invalid:
    snprintf(b->error,sizeof(b->error),"Completed-window metadata does not match the current snapshot");
    return 0;
}

/* 합성 ResourceBatch는 호출자의 stack fixture를 빌린다. 실제 수집 배열처럼
 * resource_batch_free로 해제하면 안 된다. 입력은 Win32 synthetic_batch와 같다. */
static void synthetic_batch(Bridge *b,ResourceBatch *batch,ResourceData *fixture)
{
    uint64_t now=b->synthetic_start+(uint64_t)b->synthetic_cycle*3*UINT64_C(10000000);
    fixture->pid=4242; fixture->creation_time_utc_100ns=b->synthetic_start-UINT64_C(10000000);
    fixture->process_name_utf8="SYNTHETIC fixture.exe"; fixture->measured_at_utc_100ns=now;
    fixture->valid_fields=RESOURCE_VALID_NAME|RESOURCE_VALID_IDENTITY|RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|
        RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_OBSERVATION_INTERVAL|RESOURCE_VALID_FOREGROUND;
    fixture->cpu_percent=b->synthetic_cycle>60?50:1; fixture->working_set_bytes=UINT64_C(67108864);
    fixture->observation_interval_seconds=fixture->cpu_interval_seconds=fixture->io_interval_seconds=3;
    fixture->foreground=RESOURCE_BACKGROUND;
    batch->items=fixture; batch->count=1; batch->started_at_utc_100ns=batch->finished_at_utc_100ns=now;
    batch->logical_processor_count=1;
}

/* 호출자는 치명적 실패의 실제 원인을 error에 먼저 쓴다. 이후 feedback/edit 등의
 * 거절은 자기 error를 계속 반환하므로, 중단 원인은 별도 사본으로 성공한 resume까지
 * 보존한다. 최근 조회가 failed를 반환할 때만 이 사본을 이번 요청의 오류로 사용한다. */
static void record_fatal_failure(Bridge *b)
{
    snprintf(b->failure_error,sizeof(b->failure_error),"%s",b->error);
    b->failed=1; notification_policy_reset(&b->notifications);
}

/* 실패한 회차의 표시 배열만 해제한다. 이전 정상 view는 Bridge에 남지만 failed로
 * 새 수집과 알림을 중단하고 NULL을 반환한다. managed 호출자는 실패를 화면에 발행한다.
 * 오류 문구는 호출자가 실제 실패 지점에서 먼저 정하며 batch 해제도 호출자가 담당한다. */
static const BridgeView *fail_collection(Bridge *b,ResourceDisplayBatch *display,uint64_t *private_working_set)
{
    resource_display_batch_free(display); free(private_working_set);
    record_fatal_failure(b);
    return NULL;
}

/* notification-only demo는 기존 근거를 유지하고 관측 시각·회차만 전진시킨다.
 * DB에 수집하거나 합성 입력을 실제 학습으로 넣는 경로가 아니다. */
static const BridgeView *collect_demo(Bridge *b)
{
    if(!b->snapshot) return demo_snapshot(b,0);
    b->snapshot->measured_utc_100ns=monitor_utc_now();
    b->snapshot->rows[0].raw.measured_at_utc_100ns=b->snapshot->measured_utc_100ns;
    b->snapshot->sequence=++b->demo_sequence;
    b->snapshot->completed_windows=0;
    MonitorSnapshot input=*b->snapshot; input.synthetic=0;
    if(b->demo_window) notification_policy_observe(&b->notifications,&input,b->session,GetTickCount64(),monitor_utc_now());
    return view(b);
}

/* 호출 중 준비하는 batch/display/snapshot은 성공 전까지 이 함수가 소유한다.
 * 저장 성공 뒤 원본 batch를 해제하고, 알림 관측을 마친 snapshot과 표시 배열을
 * Bridge에 넘긴다. 관계/그룹 실패는 저장 실패로 바꾸지 않고 grouping_failed로 남긴다. */
BRIDGE_API const BridgeView *bridge_collect(Bridge *b)
{
    ResourceBatch batch={0};
    ResourceData fixture={0};
    ResourceDisplayBatch display={0};
    uint64_t *private_working_set=NULL;
    MonitorSnapshot *snapshot=NULL;
    LARGE_INTEGER begin,end,freq;
    uint32_t os_error=0;
    double collect_ms;
    if(!worker(b)) return NULL;
    if(b->paused || b->failed) { snprintf(b->error,sizeof(b->error),"Collection is paused or failed"); return NULL; }
    if(b->demo) return collect_demo(b);
    QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&begin);
    if(b->synthetic) synthetic_batch(b,&batch,&fixture);
    else if(resource_collect_with_private_ws(b->collector,&batch,&display,&private_working_set,&os_error)!=RESOURCE_OK) {
        if(!monitor_record_failure(b->engine,monitor_utc_now(),os_error))
            snprintf(b->error,sizeof(b->error),"%s",monitor_error(b->engine));
        else snprintf(b->error,sizeof(b->error),"Collector failed: Win32 %lu; windows reset",(unsigned long)os_error);
        return fail_collection(b,&display,private_working_set);
    }
    QueryPerformanceCounter(&end);
    collect_ms=(double)(end.QuadPart-begin.QuadPart)*1000.0/(double)freq.QuadPart;
    const char *paths[]={"SYNTHETIC:\\fixture.exe"};
    if(!monitor_ingest(b->engine,&batch,b->synthetic?paths:NULL,&snapshot)) {
        if(!b->synthetic) resource_batch_free(&batch);
        snprintf(b->error,sizeof(b->error),"수집 회차 저장 실패 · %s",monitor_error(b->engine));
        return fail_collection(b,&display,private_working_set);
    }
    if(!b->synthetic) resource_batch_free(&batch);
    ++b->synthetic_cycle; snapshot->collect_ms=collect_ms;
    /* UI 검색·접기보다 먼저 커밋된 전체 회차로 알림을 관측한다. 이후 묶음 교체가
     * 성공 소유권 이전 지점이며, 이전 view의 모든 빌린 포인터는 여기서 무효가 된다. */
    notification_policy_observe(&b->notifications,snapshot,b->session,GetTickCount64(),monitor_utc_now());
    release_view(b); b->snapshot=snapshot; b->display=display; b->private_working_set=private_working_set;
    QueryPerformanceCounter(&begin);
    if(b->synthetic) { PgProbe probe={0}; b->view.grouping_failed=!pg_resolve(b->tracker,snapshot,&probe,&b->relations); }
    else b->view.grouping_failed=!pg_collect_windows(b->tracker,snapshot,NULL,&b->relations);
    if(b->relations && !rebuild(b)) b->view.grouping_failed=1;
    QueryPerformanceCounter(&end);
    b->view.grouping_ms=(double)(end.QuadPart-begin.QuadPart)*1000.0/(double)freq.QuadPart;
    b->error[0]=0;
    return view(b);
}
BRIDGE_API int bridge_pause(Bridge *b)
{
    if(!worker(b)) return 0;
    if(!b->paused) {
        b->paused=1;
        resource_collector_destroy(b->collector); b->collector=NULL;
        pg_tracker_reset(b->tracker); cancel_dispatch(b); notification_policy_reset(&b->notifications);
        b->event=(NotificationEvent){0}; b->clicked=(NotificationEvent){0};
    }
    return 1;
}
BRIDGE_API int bridge_resume(Bridge *b)
{
    uint32_t os_error=0;
    if(!worker(b)) return 0;
    if(!b->paused) return 1;
    if(!b->demo && !b->synthetic && resource_collector_create(NULL,&b->collector,&os_error)!=RESOURCE_OK) {
        snprintf(b->error,sizeof(b->error),"Collector resume failed: Win32 %lu",(unsigned long)os_error); return 0;
    }
    b->session=next_session(); b->paused=0; b->failed=0;
    if(b->engine) monitor_reset(b->engine);
    pg_tracker_reset(b->tracker); pg_membership_reset(b->membership); b->group_refresh_pending=0;
    notification_policy_reset(&b->notifications); b->demo_window=0;
    /* 이전 frame은 이전 세션에 속한다. Resume 뒤 그룹 수정·피드백·알림 신원 검증에
     * 이전 관계나 행을 재사용하지 않는다. */
    release_view(b); b->error[0]=0; b->failure_error[0]=0;
    return 1;
}
static size_t find_row(Bridge *b,uint32_t pid,uint64_t creation)
{
    return pg_find_identity(b->relations,(PgIdentity){pid,creation});
}
/* 현재 세션의 편집 자체가 거절되면 NULL이다. 적용 뒤 표시 목록 할당만 실패하면
 * membership과 기존 목록을 유지하고 성공 view를 돌려 재적용을 유도하지 않는다.
 * 호출자는 bridge_group_refresh_pending으로 이 부분 성공을 구분한다. */
BRIDGE_API const BridgeView *bridge_edit(Bridge *b,unsigned session,int action,uint32_t pid,uint64_t creation,PgGroupKey group)
{
    size_t row,gi;
    int ok=0;
    if(!worker(b)) return NULL;
    if(session!=b->session || !b->snapshot || !b->relations) goto invalid;
    /* 편집은 이미 적용됐어도 groups는 이전 소속의 목록일 수 있다. 그 목록에 의존하는
     * 확정·그룹 대상·펼침 명령은 재구성 성공 전까지 거절한다. 개별 tuple 명령은 현재
     * relations와 membership으로 검증하므로 허용하며, 앞선 편집을 다시 적용하지 않는다. */
    if(b->group_refresh_pending && (action==0 || action==1 || (action>=6 && action<=9))) goto invalid;
    row=find_row(b,pid,creation); gi=pg_group_find(b->groups,group);
    switch(action) {
    case 0: case 1: if(gi!=PG_NONE) ok=pg_confirm_members(b->membership,b->groups,gi,b->snapshot,b->relations,action==1); break;
    case 2: if(row!=PG_NONE) ok=pg_set_process_target(b->membership,b->snapshot,b->relations,row); break;
    case 3: if(row!=PG_NONE) ok=pg_move_to_target(b->membership,b->snapshot,b->relations,row); break;
    case 4: if(row!=PG_NONE) ok=pg_separate_shared(b->membership,b->snapshot,b->relations,row); break;
    case 5: if(row!=PG_NONE) ok=pg_restore_auto(b->membership,(PgIdentity){pid,creation}); break;
    case 6: case 7: if(gi!=PG_NONE) ok=pg_group_set_expanded(b->membership,group,action==6); break;
    case 8: case 9: pg_groups_expand_all(b->membership,b->groups,action==8); ok=1; break;
    default: break;
    }
    if(ok) {
        if(rebuild(b)) b->error[0]=0;
        else {
            b->group_refresh_pending=1;
            snprintf(b->error,sizeof(b->error),"Group edit was applied; group list refresh is pending");
        }
        return view(b);
    }
invalid:
    snprintf(b->error,sizeof(b->error),"Membership target is unavailable or changed; operation was not applied"); return NULL;
}
/* 피드백 자체 저장이 실패하면 이전 표시행을 고치지 않고 failed로 중단한다.
 * 커밋 뒤 보관 정리만 지연되면 성공으로 반영하고 view.retention_pending에 남긴다. */
BRIDGE_API int bridge_feedback(Bridge *b,unsigned session,uint32_t pid,uint64_t creation,int64_t program,MonitorFeedback kind)
{
    size_t row;
    uint64_t now,expires;
    if(!worker(b)) return 0;
    if(b->demo || b->failed || session!=b->session || !b->snapshot || program<=0 ||
        kind<MONITOR_FEEDBACK_CANCEL || kind>MONITOR_FEEDBACK_EXPECTED) goto invalid;
    /* 복사한 명령의 인스턴스와 정확한 경로 program ID를 모두 현재 행에 대조한다. */
    row=find_row(b,pid,creation);
    if(row==PG_NONE || b->snapshot->rows[row].program_id!=program) goto invalid;
    now=monitor_utc_now();
    if(!monitor_feedback(b->engine,program,kind,now)) {
        snprintf(b->error,sizeof(b->error),"알림 설정 저장 실패 · %s",monitor_error(b->engine));
        record_fatal_failure(b); return 0;
    }
    expires=kind==MONITOR_FEEDBACK_CANCEL?0:now+(kind==MONITOR_FEEDBACK_IGNORE?UINT64_C(3600):UINT64_C(86400))*UINT64_C(10000000);
    ++b->revision;
    for(size_t i=0;i<b->snapshot->count;++i) if(b->snapshot->rows[i].program_id==program) {
        b->snapshot->rows[i].feedback=kind; b->snapshot->rows[i].feedback_expires_utc_100ns=expires;
    }
    notification_policy_feedback(&b->notifications,program,kind,expires,now);
    return 1;
invalid:
    snprintf(b->error,sizeof(b->error),"Feedback target is unavailable or changed; no storage write was made"); return 0;
}
BRIDGE_API void bridge_notifications_enable(Bridge *b,int enabled)
{
    if(worker(b)) { cancel_dispatch(b); notification_policy_enable(&b->notifications,enabled); }
}
BRIDGE_API const NotificationEvent *bridge_notification_begin(Bridge *b)
{
    return worker(b) && !b->paused && !b->failed &&
        notification_policy_begin(&b->notifications,GetTickCount64(),0,&b->event)?&b->event:NULL;
}
BRIDGE_API const NotificationEvent *bridge_notification_dispatch(Bridge *b,uint64_t id)
{
    if(!worker(b) || b->paused || b->failed ||
        !notification_policy_dispatch(&b->notifications,id,GetTickCount64(),0,&b->dispatched)) return NULL;
    return &b->dispatched;
}
BRIDGE_API void bridge_notification_result(Bridge *b,uint64_t id,int accepted)
{
    if(worker(b) && id && b->dispatched.id==id) {
        notification_policy_result(&b->notifications,&b->dispatched,accepted,GetTickCount64());
        b->dispatched=(NotificationEvent){0};
    }
}
BRIDGE_API void bridge_notification_retire(Bridge *b,uint64_t id)
{
    if(worker(b)) notification_policy_retire(&b->notifications,id);
}
BRIDGE_API const NotificationEvent *bridge_notification_click(Bridge *b,uint64_t id)
{
    if(!worker(b) || b->paused || b->failed) return NULL;
    if(!notification_policy_click(&b->notifications,id,GetTickCount64(),&b->event)) return NULL;
    b->clicked=b->event; return &b->clicked;
}
/* 클릭한 복사 근거를 현재 행에 연결하기 전 세션·경로·신선도와 실제 프로세스의
 * PID+생성시각을 확인한다. 임시 HANDLE은 여기서 닫으며 실패의 -1은 당시 근거를 지우지 않는다. */
BRIDGE_API int64_t bridge_notification_validate(Bridge *b,uint64_t id,unsigned session,unsigned member)
{
    NotificationMember *m;
    const MonitorRow *current;
    size_t row;
    HANDLE p;
    FILETIME creation,exit_time,kernel,user;
    DWORD state;
    uint64_t now=monitor_utc_now(),created;
    if(!worker(b)) return -1;
    if(b->clicked.id!=id || member>=b->clicked.member_count || b->clicked.session!=session ||
        session!=b->session || b->paused || b->failed ||
        !b->snapshot || !b->snapshot->measured_utc_100ns || now<b->snapshot->measured_utc_100ns ||
        now-b->snapshot->measured_utc_100ns>NOTIFICATION_FRESH_MS*UINT64_C(10000)) goto stale;
    m=&b->clicked.members[member]; row=find_row(b,m->pid,m->creation);
    if(row==PG_NONE) goto stale;
    current=&b->snapshot->rows[row];
    if(current->program_id!=b->clicked.program || !current->path || !current->path[0] || current->path_error ||
        !(current->raw.valid_fields&RESOURCE_VALID_IDENTITY) || current->raw.creation_time_utc_100ns!=m->creation ||
        !current->raw.measured_at_utc_100ns || now<current->raw.measured_at_utc_100ns ||
        now-current->raw.measured_at_utc_100ns>NOTIFICATION_FRESH_MS*UINT64_C(10000)) goto stale;
    p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,m->pid);
    if(!p) { snprintf(b->error,sizeof(b->error),"알림 대상의 현재 신원을 확인할 수 없습니다 (Win32 %lu)",(unsigned long)GetLastError()); return -1; }
    if(!GetProcessTimes(p,&creation,&exit_time,&kernel,&user)) { CloseHandle(p); goto stale; }
    created=((uint64_t)creation.dwHighDateTime<<32)|creation.dwLowDateTime;
    state=WaitForSingleObject(p,0); CloseHandle(p);
    if(created!=m->creation || state!=WAIT_TIMEOUT) goto stale;
    b->error[0]=0; return (int64_t)row;
stale:
    snprintf(b->error,sizeof(b->error),"알림과 같은 실행 인스턴스를 최신 관측에서 확인할 수 없습니다. 당시 근거만 표시합니다."); return -1;
}
static const BridgeView *demo_snapshot(Bridge *b,int trigger)
{
    MonitorSnapshot *s;
    MonitorRow *r;
    PgProbe probe={0};
    FILETIME c,e,k,u;
    uint64_t now;
    if(!worker(b) || !b->demo || b->paused) return NULL;
    s=calloc(1,sizeof(*s)); r=calloc(1,sizeof(*r));
    if(!s || !r) { free(s); free(r); snprintf(b->error,sizeof(b->error),"Demo allocation failed"); return NULL; }
    r->name=_strdup("[합성 검수] 알림.exe"); r->path=_strdup("SYNTHETIC:\\Notification\\알림.exe");
    if(!r->name || !r->path || !GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u)) {
        free(r->name); free(r->path); free(r); free(s); return NULL;
    }
    now=monitor_utc_now();
    r->raw.pid=GetCurrentProcessId(); r->raw.creation_time_utc_100ns=((uint64_t)c.dwHighDateTime<<32)|c.dwLowDateTime;
    r->raw.measured_at_utc_100ns=now; r->raw.process_name_utf8=r->name;
    r->raw.valid_fields=RESOURCE_VALID_NAME|RESOURCE_VALID_IDENTITY|RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|
        RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_OBSERVATION_INTERVAL;
    r->raw.cpu_percent=2; r->raw.working_set_bytes=UINT64_C(134217728);
    r->raw.observation_interval_seconds=r->raw.cpu_interval_seconds=r->raw.io_interval_seconds=3;
    r->program_id=1;
    for(unsigned i=0;i<MONITOR_METRICS;++i) {
        r->metric[i].state=MONITOR_NORMAL; r->metric[i].window_start_utc_100ns=now-UINT64_C(600000000);
        r->metric[i].window_end_utc_100ns=now; r->metric[i].window_duration_seconds=60;
        r->metric[i].count=100; r->metric[i].days=4; r->metric[i].minutes=100;
        r->metric[i].valid_samples=20; r->metric[i].covered_seconds=60; r->metric[i].baseline_eligible=1;
        snprintf(r->metric[i].reason,sizeof(r->metric[i].reason),"synthetic_notification_demo");
    }
    r->metric[0].state=MONITOR_CANDIDATE; r->metric[0].current=52;
    r->metric[0].median=2; r->metric[0].threshold=12; r->metric[0].streak=3;
    s->rows=r; s->count=1; s->measured_utc_100ns=now; s->sequence=++b->demo_sequence;
    s->completed_windows=trigger?1:0; s->synthetic=1; b->demo_window=trigger?now:0;
    snprintf(s->status,sizeof(s->status),"합성 알림 검수 · 실제 사용량·저장·학습 근거가 아닙니다");
    release_view(b); b->snapshot=s;
    if(!pg_resolve(b->tracker,s,&probe,&b->relations) || !rebuild(b)) { release_view(b); return NULL; }
    MonitorSnapshot input=*s; input.synthetic=0;
    if(trigger) notification_policy_observe(&b->notifications,&input,b->session,GetTickCount64(),now);
    return view(b);
}
BRIDGE_API const BridgeView *bridge_notification_demo(Bridge *b) { return demo_snapshot(b,1); }
BRIDGE_API int bridge_storage_stats(Bridge *b,MonitorStorageStats *stats)
{
    return worker(b) && b->engine && monitor_storage_stats(b->engine,stats);
}
BRIDGE_API int bridge_group_expanded(Bridge *b,PgGroupKey group)
{
    return worker(b)?pg_group_expanded(b->membership,group):0;
}
BRIDGE_API int bridge_recent_result(Bridge *b,unsigned session,uint64_t sequence,uint32_t pid,
    uint64_t creation,int64_t program,const char *path,MonitorRecentComparison *out)
{
    const MonitorRow *row = NULL;
    size_t i, matches = 0;
    if (out) memset(out, 0, sizeof(*out));
    /* 잘못된 스레드가 소유 worker의 오류 버퍼를 덮어쓰지 않는다. managed Check도
     * 같은 경계를 검사한다. 이 경우 context의 오류를 읽는 것 역시 호출 계약 밖이다. */
    if (!b || b->thread != GetCurrentThreadId()) return BRIDGE_RECENT_ERROR;
    if (b->failed) {
        snprintf(b->error, sizeof(b->error), "%s", b->failure_error[0] ? b->failure_error :
            "최근 기록 조회 불가 · 수집 엔진이 실패 상태입니다.");
        return BRIDGE_RECENT_ERROR;
    }
    b->error[0] = 0;
    if (!out || !path || !path[0] || program <= 0 || !pid || !creation) {
        snprintf(b->error, sizeof(b->error), "최근 기록 조회 요청에 유효한 대상과 출력이 필요합니다.");
        return BRIDGE_RECENT_ERROR;
    }
    if (!b->snapshot || session != b->session || sequence != b->snapshot->sequence) {
        snprintf(b->error, sizeof(b->error), "최근 기록 조회 대상의 관측 세션 또는 회차가 바뀌었습니다.");
        return BRIDGE_RECENT_TARGET_CHANGED;
    }
    for (i = 0; i < b->snapshot->count; ++i) {
        const MonitorRow *candidate = &b->snapshot->rows[i];
        if (candidate->raw.pid == pid && candidate->raw.creation_time_utc_100ns == creation) {
            row = candidate; ++matches;
        }
    }
    if (matches != 1 || !row || !(row->raw.valid_fields & RESOURCE_VALID_IDENTITY) ||
        row->program_id != program || !row->path || row->path_error || strcmp(row->path, path)) {
        snprintf(b->error, sizeof(b->error), "최근 기록 조회 대상의 실행 인스턴스 또는 경로를 현재 관측에서 확인할 수 없습니다.");
        return BRIDGE_RECENT_TARGET_CHANGED;
    }
    if (b->demo) {
        for (i = 0; i < MONITOR_METRICS; ++i)
            out->metric[i].state = row->metric[i].state >= MONITOR_NORMAL ? MONITOR_RECENT_LONG_TERM : MONITOR_RECENT_NO_WINDOW;
        return BRIDGE_RECENT_SUCCESS;
    }
    if (!monitor_recent_read(b->engine, row, out)) {
        snprintf(b->error, sizeof(b->error), "최근 기록 조회 실패 · %s", monitor_error(b->engine));
        return BRIDGE_RECENT_ERROR;
    }
    return BRIDGE_RECENT_SUCCESS;
}
BRIDGE_API int bridge_recent(Bridge *b,unsigned session,uint64_t sequence,uint32_t pid,
    uint64_t creation,int64_t program,const char *path,MonitorRecentComparison *out)
{
    return bridge_recent_result(b,session,sequence,pid,creation,program,path,out) == BRIDGE_RECENT_SUCCESS;
}
BRIDGE_API size_t bridge_sizeof(int type)
{
    switch(type) {
    case 0:return sizeof(BridgeView); case 1:return sizeof(ResourceData); case 2:return sizeof(MonitorMetricReport);
    case 3:return sizeof(MonitorRow); case 4:return sizeof(MonitorSnapshot); case 5:return sizeof(ResourceDisplayInfo);
    case 6:return sizeof(PgGroup); case 7:return sizeof(PgGroups); case 8:return sizeof(PgGroupKey);
    case 9:return sizeof(NotificationEvent); case 10:return sizeof(NotificationMember);
    case 11:return sizeof(MonitorRecentMetric); case 12:return sizeof(MonitorRecentComparison);
    case 13:return sizeof(MonitorCompletedWindow); default:return 0;
    }
}
