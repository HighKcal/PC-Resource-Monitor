/* A안 프로그램 목록·오른쪽 상세와 수집 worker를 연결한다. UI는 전달받은 복사본만 읽고
 * collector·SQLite는 worker가 소유한다. 기본 관리자 요청은 실행 manifest에서 정한다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <psapi.h>
#include "monitor.h"
#include "resource_display.h"
#include "program_groups.h"
#include "native_availability.h"

#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define WM_SNAPSHOT (WM_APP + 1)
#define WM_WORKER_DONE (WM_APP + 2)
#define ID_LIST 101
#define ID_IGNORE 102
#define ID_EXPECTED 103
#define ID_CANCEL 104
#define ID_PAUSE 105
#define ID_FLAT 106
#define ID_EXPAND 107
#define ID_COLLAPSE 108
#define ID_CONFIRM 109
#define ID_TARGET 110
#define ID_MOVE 111
#define ID_SHARED 112
#define ID_AUTO 113
#define ID_TITLE 114
#define ID_SUBTITLE 115
#define ID_LIST_TITLE 116
#define ID_DETAIL_TITLE 117
#define ID_SEARCH 118
#define ID_SORT 119
#define ID_METRIC 120
#define FEEDBACK_LIMIT 16
typedef enum FeedbackOutcome { FEEDBACK_PENDING, FEEDBACK_SUCCEEDED, FEEDBACK_FAILED, FEEDBACK_CANCELLED } FeedbackOutcome;
/* 접수한 요청마다 UI가 소비할 종결 응답 한 칸을 예약한다. 대기·실행·응답 대기를 합쳐 16개다. */
typedef struct FeedbackCommand {
    int64_t program;
    MonitorFeedback kind;
    uint64_t expires, request_id, revision;
    unsigned session;
    FeedbackOutcome outcome;
} FeedbackCommand;
/* UI가 소유하며 접수 전에 pending 상태를 확보한다. revision 0은 덧씌울 성공 표시가 없다는 뜻이다.
   응답 소비와 성공 표시의 수명은 다르다. 만료·취소 저장 성공도 정상 frame이 반영할 때까지 남겨
   오래된 snapshot의 억제 표시가 되살아나지 않게 한다. 순서는 UTC가 아닌 worker revision으로 비교한다. */
typedef struct FeedbackOverlay {
    struct FeedbackOverlay *next;
    int64_t program;
    MonitorFeedback kind;
    uint64_t expires, revision;
    unsigned pending;
} FeedbackOverlay;
typedef enum FeedbackSubmit { FEEDBACK_ACCEPTED, FEEDBACK_BUSY, FEEDBACK_NO_MEMORY, FEEDBACK_CLOSED } FeedbackSubmit;
/* 자원·이름 표시 메타데이터·관계를 함께 소유한다. worker와 UI 사이에는 이 묶음의 소유권을 넘긴다. */
typedef struct GuiSnapshot {
    MonitorSnapshot *metrics;
    ResourceDisplayBatch display;
    PgSnapshot *relations;
    double grouping_ms;
    unsigned session;
    int grouping_failed;
    uint64_t feedback_revision;
    int feedback_current;
} GuiSnapshot;
typedef struct App {
    HWND window, list, detail, status, pause;
    HFONT body_font, title_font, heading_font;
    HIMAGELIST row_images;
    HBRUSH background_brush, panel_brush;
    UINT dpi, ui_scale;
    int ui_demo, name_width_dip, setting_columns, sort_mode, detail_metric;
    unsigned ui_width, ui_height;
    wchar_t search[256];
    PgIdentity detail_key;
    PgGroupKey detail_group;
    int detail_kind, shown_detail_metric;
    HANDLE stop, wake, worker;
    SRWLOCK lock;
    GuiSnapshot *latest, *shown; /* latest는 잠금 아래 전달 슬롯, shown은 인수 뒤 UI 소유다. */
    MonitorSnapshot *display; /* shown에서 빌린 포인터이며 shown 교체 시 무효가 된다. */
    PgGroups *groups;
    PgMembership *membership;
    PgIdentity selected_key, top_key;
    PgGroupKey selected_group, top_group;
    int selected_is_group, top_is_group;
    FeedbackCommand commands[FEEDBACK_LIMIT];
    FeedbackCommand feedback_results[FEEDBACK_LIMIT];
    unsigned feedback_result_count, feedback_outstanding;
    uint64_t feedback_request_id;
    int feedback_closed;
    FeedbackOverlay *feedback_overlay;
    unsigned feedback_overlay_count;
    unsigned command_count, count, interval_ms, completed;
    int headless, synthetic, pause_requested, stopping, worker_error, updating, stats, no_grouping, flat, diagnostics;
    unsigned session;
    char db[32768];
    uint64_t heartbeat, last_tick, max_heartbeat_gap, stop_started;
} App;
static App app;
#include "native_ui.h"

static const char *utc_text(uint64_t value, char *buffer, size_t size)
{
    FILETIME ft;
    SYSTEMTIME st;
    ft.dwLowDateTime = (DWORD)value; ft.dwHighDateTime = (DWORD)(value >> 32);
    if (!value || !FileTimeToSystemTime(&ft, &st)) snprintf(buffer, size, "시각 확인 불가");
    else snprintf(buffer, size, "%04u-%02u-%02u %02u:%02u:%02u.%03u UTC",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buffer;
}
static BOOL WINAPI console_handler(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        SetEvent(app.stop); return TRUE;
    }
    return FALSE;
}

static wchar_t *to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n ? malloc((size_t)n * sizeof(*w)) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static void set_text(HWND window, const char *s)
{
    wchar_t *w = to_wide(s); if (w) { SetWindowTextW(window, w); free(w); }
}
static void set_stale_status(HWND window)
{
    static const wchar_t text[] = L"표본 오래됨 · 15초 넘게 성공한 관측이 없습니다. 표시 수치는 마지막 표본입니다.";
    /* 다른 상태 문구가 끼어들 수 있어 실제 컨트롤의 길이와 전체 내용을 대조한다.
       여분 한 글자는 조회 도중 더 긴 문구로 바뀐 경우 같은 접두어만 보고 갱신을 생략하지 않게 한다. */
    wchar_t current[sizeof(text) / sizeof(text[0]) + 1];
    const int length = (int)(sizeof(text) / sizeof(text[0])) - 1;
    if (GetWindowTextLengthW(window) == length &&
        GetWindowTextW(window, current, (int)(sizeof(current) / sizeof(current[0]))) == length &&
        wcscmp(current, text) == 0) return;
    SetWindowTextW(window, text);
}
static void gui_snapshot_free(GuiSnapshot *s)
{
    if(s) { resource_display_batch_free(&s->display); monitor_snapshot_free(s->metrics); pg_snapshot_free(s->relations); free(s); }
}
static FeedbackOverlay *feedback_overlay_find(int64_t program)
{
    FeedbackOverlay *entry;
    for(entry=app.feedback_overlay;entry && entry->program!=program;entry=entry->next) { }
    return entry;
}
static void feedback_reconcile(void)
{
    FeedbackOverlay **link=&app.feedback_overlay;
    while(*link) {
        FeedbackOverlay *entry=*link;
        size_t row;
        int shown=0, normal=app.shown && app.shown->feedback_current;
        if(entry->revision && normal &&
            entry->revision<=app.shown->feedback_revision) entry->revision=0;
        if(entry->revision && normal && app.display) for(row=0;row<app.display->count;++row)
            if(app.display->rows[row].program_id==entry->program) { shown=1; break; }
        /* 오류 frame은 프로세스 집합을 관측한 결과가 아니다. 빈 행만으로 program 부재를
           확인하거나 마지막 정상 frame의 성공 표시를 지우면 안 된다. */
        if(!entry->pending && (!entry->revision || (normal && !shown))) {
            *link=entry->next; free(entry); --app.feedback_overlay_count;
        } else link=&entry->next;
    }
}
static void feedback_dispose(void)
{
    FeedbackOverlay *entry=app.feedback_overlay;
    while(entry) { FeedbackOverlay *next=entry->next; free(entry); entry=next; }
    app.feedback_overlay=NULL; app.feedback_overlay_count=0;
}
static FeedbackSubmit feedback_submit(int64_t program,MonitorFeedback kind)
{
    FeedbackOverlay *entry;
    int allocated=0;
    FeedbackSubmit result=FEEDBACK_ACCEPTED;
    FeedbackCommand command={0};
    /* 쓰기를 접수하기 전에 UI 표시 노드를 확보한다. 종결 응답을 추가할 때는 할당하지 않는다. */
    feedback_reconcile(); entry=feedback_overlay_find(program);
    if(!entry) {
        entry=calloc(1,sizeof(*entry));
        if(!entry) return FEEDBACK_NO_MEMORY;
        entry->program=program; allocated=1;
    }
    AcquireSRWLockExclusive(&app.lock);
    if(app.feedback_closed || app.stopping || WaitForSingleObject(app.stop,0)==WAIT_OBJECT_0) result=FEEDBACK_CLOSED;
    else if(app.feedback_outstanding>=FEEDBACK_LIMIT || app.feedback_request_id==UINT64_MAX) result=FEEDBACK_BUSY;
    else {
        command.program=program; command.kind=kind; command.session=app.session;
        command.request_id=++app.feedback_request_id;
        app.commands[app.command_count++]=command; ++app.feedback_outstanding;
    }
    ReleaseSRWLockExclusive(&app.lock);
    if(result!=FEEDBACK_ACCEPTED) { if(allocated) free(entry); return result; }
    /* 이 함수는 UI 스레드에서 메시지를 처리하지 않고 끝난다. worker가 즉시 완료해도
       UI가 응답을 소비하기 전에 예약 노드 연결과 pending 증가가 먼저 끝난다. */
    if(allocated) { entry->next=app.feedback_overlay; app.feedback_overlay=entry; ++app.feedback_overlay_count; }
    ++entry->pending; SetEvent(app.wake);
    return FEEDBACK_ACCEPTED;
}
static void feedback_complete(FeedbackCommand command,FeedbackOutcome outcome,uint64_t now,uint64_t revision)
{
    command.outcome=outcome; command.revision=revision;
    command.expires=outcome==FEEDBACK_SUCCEEDED && command.kind ? now+
        (command.kind==MONITOR_FEEDBACK_IGNORE?UINT64_C(36000000000):UINT64_C(864000000000)):0;
    /* 접수 시 예약한 outstanding에는 worker가 가져간 명령과 미소비 응답도 포함된다.
       따라서 대기 큐가 비었더라도 이 추가를 위한 결과 슬롯은 계속 확보되어 있다. */
    AcquireSRWLockExclusive(&app.lock);
    app.feedback_results[app.feedback_result_count++]=command;
    ReleaseSRWLockExclusive(&app.lock);
    if(app.window) PostMessageW(app.window,WM_SNAPSHOT,0,0);
}
static void feedback_finish(void)
{
    unsigned i;
    AcquireSRWLockExclusive(&app.lock);
    app.feedback_closed=1;
    for(i=0;i<app.command_count;++i) {
        FeedbackCommand command=app.commands[i]; command.outcome=FEEDBACK_CANCELLED;
        app.feedback_results[app.feedback_result_count++]=command;
    }
    app.command_count=0;
    ReleaseSRWLockExclusive(&app.lock);
}
#include "native_demo.h"
static void publish_view(GuiSnapshot *s)
{
    GuiSnapshot *old;
    /* 잠금 아래 s의 소유권을 슬롯으로 넘긴다. 덮어쓴 미소비 묶음은 이 함수가 해제한다. */
    AcquireSRWLockExclusive(&app.lock); old = app.latest; app.latest = s;
    ReleaseSRWLockExclusive(&app.lock);
    gui_snapshot_free(old);
    if (app.window) PostMessageW(app.window, WM_SNAPSHOT, 0, 0);
}
static void publish(MonitorSnapshot *s)
{
    GuiSnapshot *view;
    if(!s) { app.worker_error=1; return; }
    view=calloc(1,sizeof(*view));
    if(!view) { monitor_snapshot_free(s); app.worker_error=1; fprintf(stderr,"GUI snapshot wrapper allocation failed\n"); return; }
    view->metrics=s;
    AcquireSRWLockShared(&app.lock); view->session=app.session; ReleaseSRWLockShared(&app.lock);
    publish_view(view);
}
static MonitorSnapshot *error_snapshot(const char *message, int store)
{
    MonitorSnapshot *s = calloc(1, sizeof(*s));
    if (s) {
        s->storage_error = store; s->collector_error = !store; s->synthetic = app.synthetic;
        s->measured_utc_100ns = monitor_utc_now(); snprintf(s->status, sizeof(s->status), "%s", message);
    }
    return s;
}
static void synthetic_batch(ResourceBatch *batch, ResourceData *row, uint64_t now, unsigned cycle)
{
    memset(row, 0, sizeof(*row));
    row->pid = 4242; row->creation_time_utc_100ns = now - (uint64_t)cycle * 3 * UINT64_C(10000000) - UINT64_C(10000000);
    row->process_name_utf8 = "SYNTHETIC fixture.exe";
    row->measured_at_utc_100ns = now;
    row->valid_fields = RESOURCE_VALID_NAME | RESOURCE_VALID_IDENTITY | RESOURCE_VALID_CPU | RESOURCE_VALID_MEMORY |
        RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL | RESOURCE_VALID_FOREGROUND;
    row->cpu_percent = cycle > 60 ? 50 : 1; row->working_set_bytes = UINT64_C(67108864);
    row->observation_interval_seconds = row->cpu_interval_seconds = row->io_interval_seconds = 3;
    row->foreground = RESOURCE_BACKGROUND;
    memset(batch, 0, sizeof(*batch)); batch->items = row; batch->count = 1;
    batch->started_at_utc_100ns = batch->finished_at_utc_100ns = now; batch->logical_processor_count = 1;
}
/* 피드백 저장 → 한 회차 수집·ingest → 관계를 붙인 묶음 공개 → 대기 순서다.
 * 기본 3초 대기는 처리 뒤 시작하며 wake로 일찍 끝날 수 있어 고정 수집 주기가 아니다. */
static DWORD WINAPI worker_main(void *unused)
{
    MonitorEngine *engine = NULL;
    ResourceCollector *collector = NULL;
    char error[256];
    uint32_t os_error = 0;
    uint64_t synthetic_start = monitor_utc_now();
    uint64_t feedback_revision=0;
    PgTracker *tracker=pg_tracker_create();
    unsigned session=0;
    int was_paused = 0;
    (void)unused;
    if (!monitor_open(app.db, app.synthetic, &engine, error, sizeof(error))) {
        publish(error_snapshot(error, 1)); app.worker_error = 1; goto finished;
    }
    printf("SQLite=%s mode=%s raw_retention=1h summary_retention=28d DB=%s\n", monitor_storage_version(),
        app.synthetic ? "SYNTHETIC" : "real", app.db); fflush(stdout);
    if (!app.synthetic && resource_collector_create(NULL, &collector, &os_error) != RESOURCE_OK) {
        snprintf(error, sizeof(error), "Collector initialization failed: Win32 %lu", (unsigned long)os_error);
        publish(error_snapshot(error, 0)); app.worker_error = 1; goto finished;
    }
    while (WaitForSingleObject(app.stop, 0) != WAIT_OBJECT_0) {
        ResourceBatch batch = {0};
        ResourceDisplayBatch display = {0};
        ResourceData fixture;
        MonitorSnapshot *snapshot = NULL;
        LARGE_INTEGER begin, end, freq;
        FeedbackCommand commands[FEEDBACK_LIMIT];
        unsigned n, i;
        int paused;
        unsigned current_session;
        AcquireSRWLockExclusive(&app.lock);
        n = app.command_count; memcpy(commands, app.commands, n * sizeof(*commands)); app.command_count = 0;
        paused = app.pause_requested; ResetEvent(app.wake);
        current_session=app.session;
        ReleaseSRWLockExclusive(&app.lock);
        if(session!=current_session) {
            if(session && !was_paused) { monitor_reset(engine); resource_collector_destroy(collector); collector=NULL; was_paused=1; }
            pg_tracker_reset(tracker); session=current_session;
        }
        for (i = 0; i < n; ++i) {
            uint64_t feedback_now = monitor_utc_now();
            if(WaitForSingleObject(app.stop,0)==WAIT_OBJECT_0) {
                for(;i<n;++i) feedback_complete(commands[i],FEEDBACK_CANCELLED,0,0);
                goto finished;
            }
            if (!monitor_feedback(engine, commands[i].program, commands[i].kind, feedback_now)) {
                feedback_complete(commands[i],FEEDBACK_FAILED,0,0);
                for(++i;i<n;++i) feedback_complete(commands[i],FEEDBACK_CANCELLED,0,0);
                publish(error_snapshot(monitor_error(engine), 1)); app.worker_error = 1; goto finished;
            }
            feedback_complete(commands[i],FEEDBACK_SUCCEEDED,feedback_now,++feedback_revision);
        }
        /* Pause는 현재 회차 뒤 미완료창·지속성을 버린다. Resume은 새 collector의
           첫 표본부터 시작하며 관계 tracker도 이전 세션과 연결하지 않는다. */
        if (paused) {
            HANDLE waits[2] = { app.stop, app.wake };
            if (!was_paused) {
                monitor_reset(engine); resource_collector_destroy(collector); collector = NULL; was_paused = 1;
                pg_tracker_reset(tracker);
            }
            WaitForMultipleObjects(2, waits, FALSE, 250); continue;
        }
        if (was_paused && !app.synthetic) {
            if (resource_collector_create(NULL, &collector, &os_error) != RESOURCE_OK) {
                publish(error_snapshot("Collector resume failed", 0)); app.worker_error = 1; goto finished;
            }
        }
        was_paused = 0;
        QueryPerformanceCounter(&begin); QueryPerformanceFrequency(&freq);
        if (app.synthetic) synthetic_batch(&batch, &fixture,
            synthetic_start + (uint64_t)app.completed * 3 * UINT64_C(10000000), app.completed);
        else if (resource_collector_collect_display(collector, &batch, &display, &os_error) != RESOURCE_OK) {
            resource_display_batch_free(&display);
            if (!monitor_record_failure(engine, monitor_utc_now(), os_error)) {
                publish(error_snapshot(monitor_error(engine), 1)); app.worker_error = 1; goto finished;
            }
            snprintf(error, sizeof(error), "Collector failed: Win32 %lu; windows reset", (unsigned long)os_error);
            publish(error_snapshot(error, 0)); app.worker_error = 1; goto finished;
        }
        QueryPerformanceCounter(&end);
        {
            const char *paths[1] = { "SYNTHETIC:\\fixture.exe" };
            int ok = monitor_ingest(engine, &batch, app.synthetic ? paths : NULL, &snapshot);
            if (!app.synthetic) resource_batch_free(&batch);
            if (!ok) { resource_display_batch_free(&display); publish(error_snapshot(monitor_error(engine), 1)); app.worker_error = 1; goto finished; }
        }
        snapshot->collect_ms = (double)(end.QuadPart - begin.QuadPart) * 1000 / (double)freq.QuadPart;
        ++app.completed;
        {
            GuiSnapshot *view=calloc(1,sizeof(*view));
            PROCESS_MEMORY_COUNTERS memory;
            if(!view) { resource_display_batch_free(&display); monitor_snapshot_free(snapshot); app.worker_error=1; fprintf(stderr,"GUI snapshot wrapper allocation failed\n"); goto finished; }
            /* 같은 회차의 독립 snapshot과 표시 배열을 묶음으로 옮긴다.
               정상 frame만 feedback_current를 세워 revision과 program 부재를 관측할 수 있게 한다. */
            view->metrics=snapshot; view->display=display; view->session=session;
            view->feedback_revision=feedback_revision; view->feedback_current=1;
            QueryPerformanceCounter(&begin);
            if(!app.no_grouping && !app.synthetic && (!tracker || !pg_collect_windows(tracker,snapshot,app.stop,&view->relations))) view->grouping_failed=1;
            QueryPerformanceCounter(&end);
            view->grouping_ms=(double)(end.QuadPart-begin.QuadPart)*1000/(double)freq.QuadPart;
            printf("batch=%u rows=%zu windows=%zu collect_ms=%.3f store_ms=%.3f grouping_ms=%.3f inferred=%zu historical=%zu grouping_error=%lu %s\n",app.completed,
                snapshot->count,snapshot->completed_windows,snapshot->collect_ms,snapshot->store_ms,view->grouping_ms,
                view->relations?view->relations->inferred:0,view->relations?view->relations->historical:0,
                (unsigned long)(view->relations?view->relations->probe_error:view->grouping_failed?ERROR_OUTOFMEMORY:0),snapshot->status);
            if(app.diagnostics) {
                size_t row_index;
                memset(&memory,0,sizeof(memory)); memory.cb=sizeof(memory);
                if(GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory))) printf("grouping_diag batch=%u working_set_bytes=%llu\n",app.completed,(unsigned long long)memory.WorkingSetSize);
                for(row_index=0;row_index<snapshot->count;++row_index) {
                    const MonitorRow *row=&snapshot->rows[row_index];
                    const ResourceDisplayInfo *info=ui_display_metadata(&view->display,snapshot,row_index);
                    printf("coverage batch=%u row=%zu pid=%lu creation=%llu measured=%llu valid=%lu display_source=%u metadata=%u attempted=%lu exit_observed=%d exit_code=%lu exit_error=%lu special=%u snapshot_name_error=%lu open_error=%lu times_error=%lu memory_error=%lu io_error=%lu name_error=%lu path_error=%lu path_valid=%u program_id=%lld cpu_status=%u read_status=%u write_status=%u comparison=%u,%u,%u,%u\n",
                        app.completed,row_index,(unsigned long)row->raw.pid,(unsigned long long)row->raw.creation_time_utc_100ns,(unsigned long long)row->raw.measured_at_utc_100ns,
                        (unsigned long)row->raw.valid_fields,(unsigned)ui_name_source(row,info),info?1u:0u,(unsigned long)(info?info->attempted_fields:0),
                        info?info->exit_observed:0,(unsigned long)(info?info->exit_code:0),(unsigned long)(info?info->exit_query_error:0),(unsigned)(info?info->special_process:0),
                        (unsigned long)(info?info->snapshot_name_error:0),(unsigned long)row->raw.open_error,(unsigned long)row->raw.times_error,(unsigned long)row->raw.memory_error,
                        (unsigned long)row->raw.io_error,(unsigned long)row->raw.name_error,(unsigned long)row->path_error,row->path?1u:0u,(long long)row->program_id,
                        (unsigned)row->raw.cpu_status,(unsigned)row->raw.io_read_status,(unsigned)row->raw.io_write_status,
                        (unsigned)row->metric[0].state,(unsigned)row->metric[1].state,(unsigned)row->metric[2].state,(unsigned)row->metric[3].state);
                }
            }
            fflush(stdout); publish_view(view);
        }
        if (app.count && app.completed >= app.count) break;
        {
            HANDLE waits[2] = { app.stop, app.wake };
            WaitForMultipleObjects(2, waits, FALSE, app.interval_ms);
        }
    }
finished:
    /* 새 접수를 먼저 닫고 worker가 아직 가져가지 않은 명령을 미실행 취소로 종결한다. */
    feedback_finish();
    if (engine && app.stats) {
        MonitorStorageStats stats;
        if (monitor_storage_stats(engine, &stats)) printf(
            "stats raw_rows=%llu windows=%llu metric_rows=%llu page_bytes=%llu raw_page_bytes=%llu aggregate_page_bytes=%llu raw_bytes_per_row=%.3f window_bytes_including_metrics=%.3f\n",
            (unsigned long long)stats.raw_rows, (unsigned long long)stats.windows, (unsigned long long)stats.metric_rows,
            (unsigned long long)stats.database_page_bytes, (unsigned long long)stats.raw_page_bytes,
            (unsigned long long)stats.aggregate_page_bytes, stats.raw_rows ? (double)stats.raw_page_bytes / (double)stats.raw_rows : 0,
            stats.windows ? (double)stats.aggregate_page_bytes / (double)stats.windows : 0);
        else fprintf(stderr, "stats unavailable: %s\n", monitor_error(engine));
    }
    pg_tracker_destroy(tracker); resource_collector_destroy(collector); monitor_close(engine);
    if (app.window) PostMessageW(app.window, WM_WORKER_DONE, 0, 0);
    return 0;
}
static void list_text(int row, int column, const char *text_value)
{
    wchar_t *w = to_wide(text_value);
    if (w) { ListView_SetItemText(app.list, row, column, w); free(w); }
}
static PgSnapshot *current_relations(void)
{
    /* shown에서 빌린 관계다. 재개 전 세션의 표시 묶음은 새 소속 구성에 사용하지 않는다. */
    return app.shown && app.shown->session==app.session?app.shown->relations:NULL;
}
static const ResourceDisplayInfo *display_info(size_t source)
{
    /* 메타데이터와 반환 이름은 shown의 수명을 빌린다. 다음 묶음 인수 뒤까지 보관하지 않는다. */
    return app.shown?ui_display_metadata(&app.shown->display,app.display,source):NULL;
}
static const char *display_name(size_t source)
{
    return ui_name_text(&app.display->rows[source],display_info(source));
}
static size_t display_group_source(size_t group)
{
    const PgGroup *g=&app.groups->groups[group]; size_t source;
    if(g->key.kind!=PG_OWNER_AUTO || !g->count) return PG_NONE;
    source=pg_find_identity(current_relations(),g->key.anchor);
    if(source==PG_NONE) source=app.groups->members[g->first];
    return source<app.display->count?source:PG_NONE;
}
static const char *display_group_name(size_t group)
{
    size_t source=display_group_source(group);
    return source!=PG_NONE?display_name(source):app.groups->groups[group].label;
}
static const char *display_group_name_note(size_t group)
{
    size_t source=display_group_source(group);
    return source!=PG_NONE && ui_name_source(&app.display->rows[source],display_info(source))==RESOURCE_NAME_LIST?" [목록 이름 · 참고]":"";
}
/* ListView lParam의 비음수 값은 원본 행, 음수 값은 -(그룹 인덱스+1)이다.
 * 검색·정렬한 화면 행 번호를 snapshot 인덱스나 프로세스 신원으로 사용하지 않는다. */
static int source_row_at(int index)
{
    LVITEMW item;
    if(index<0 || !app.display) return -1;
    memset(&item,0,sizeof(item)); item.mask=LVIF_PARAM; item.iItem=index;
    if(!ListView_GetItem(app.list,&item) || item.lParam<0 || (size_t)item.lParam>=app.display->count) return -1;
    return (int)item.lParam;
}
static size_t group_at(int index)
{
    LVITEMW item; int source;
    if(index<0 || !app.groups) return PG_NONE;
    memset(&item,0,sizeof(item)); item.mask=LVIF_PARAM; item.iItem=index;
    if(!ListView_GetItem(app.list,&item)) return PG_NONE;
    if(item.lParam<0) { size_t group=(size_t)(-item.lParam-1); return group<app.groups->count?group:PG_NONE; }
    source=source_row_at(index);
    return source>=0?app.groups->owner[source]:PG_NONE;
}
static int selected_item(void) { return ListView_GetNextItem(app.list,-1,LVNI_SELECTED); }
static int selected_row(void) { return source_row_at(selected_item()); }
static PgIdentity persistent_identity(int source)
{
    PgIdentity key={0,0}; PgSnapshot *relations=current_relations(); size_t i;
    if(source<0 || !app.display || (size_t)source>=app.display->count) return key;
    key=pg_row_identity(&app.display->rows[source]);
    if(relations) { if(pg_find_identity(relations,key)!=(size_t)source) key=(PgIdentity){0,0}; }
    else if(key.pid) for(i=0;i<app.display->count;++i) if(i!=(size_t)source && app.display->rows[i].raw.pid==key.pid) { key=(PgIdentity){0,0}; break; }
    return key;
}
static const char *owner_name(PgOwnerKind kind)
{
    return kind==PG_OWNER_USER?"이번 세션 지정":kind==PG_OWNER_SHARED?"공유 / 미지정":"실행 관계 추정";
}
static void update_actions(void)
{
    int row=selected_row(); size_t group=group_at(selected_item());
    int identity=row>=0 && pg_find_identity(current_relations(),pg_row_identity(&app.display->rows[row]))==(size_t)row;
    int confirmable=group!=PG_NONE && current_relations()!=NULL;
    if(confirmable) {
        size_t i; const PgGroup *g=&app.groups->groups[group];
        for(i=0;i<g->count;++i) { size_t source=app.groups->members[g->first+i]; if(pg_find_identity(current_relations(),pg_row_identity(&app.display->rows[source]))!=source) { confirmable=0; break; } }
    }
    int feedback=row>=0 && app.display->rows[row].program_id && !app.stopping && !app.worker_error && !app.ui_demo;
    EnableWindow(GetDlgItem(app.window,ID_IGNORE),feedback); EnableWindow(GetDlgItem(app.window,ID_EXPECTED),feedback); EnableWindow(GetDlgItem(app.window,ID_CANCEL),feedback);
    EnableWindow(GetDlgItem(app.window,ID_CONFIRM),confirmable && !app.stopping);
    EnableWindow(GetDlgItem(app.window,ID_TARGET),confirmable && !app.stopping);
    EnableWindow(GetDlgItem(app.window,ID_MOVE),identity && pg_target_id(app.membership)!=0 && !app.stopping);
    EnableWindow(GetDlgItem(app.window,ID_SHARED),identity && !app.stopping); EnableWindow(GetDlgItem(app.window,ID_AUTO),identity && !app.stopping);
}
/* 저장·합산 단위는 유지하고 표시할 때만 RAM bytes를 decimal GB, I/O B/s를 MB/s로 바꾼다.
 * 부분합은 *와 유효 구성원 수로 구분하며 유효 0·작은 양수·결측을 서로 다르게 표시한다. */
static const char *sum_text(const PgSum *sum,char *buffer,size_t size,unsigned metric)
{
    double value=metric==MONITOR_RAM?(double)sum->bytes/1e9:metric==MONITOR_CPU?sum->value:sum->value/1e6;
    if(sum->overflow) snprintf(buffer,size,"합계 오류 (%zu/%zu)",sum->valid,sum->total);
    else if(!sum->valid) snprintf(buffer,size,"NA (0/%zu)",sum->total);
    else if(value>0 && value<0.01) snprintf(buffer,size,"<0.01%s",sum->valid<sum->total?" *":"");
    else snprintf(buffer,size,"%.2f%s",value,sum->valid<sum->total?" *":"");
    return buffer;
}
static const char *ui_state(MonitorState state)
{
    return state==MONITOR_CANDIDATE?"변화 후보":state==MONITOR_ATTENTION?"상한 초과":
        state==MONITOR_NORMAL?"상한 이내":state==MONITOR_LEARNING?"평소 학습 중":"비교 불가";
}
static MonitorFeedback displayed_feedback(const MonitorRow *row, uint64_t *expires)
{
    FeedbackOverlay *entry=feedback_overlay_find(row->program_id);
    *expires = row->feedback_expires_utc_100ns;
    if(entry && entry->revision) {
        *expires=entry->expires;
        return entry->expires>monitor_utc_now()?entry->kind:MONITOR_FEEDBACK_CANCEL;
    }
    return *expires > monitor_utc_now() ? row->feedback : MONITOR_FEEDBACK_CANCEL;
}
#include "native_detail.h"
/* 선택·스크롤 복원은 PID+생성시각 또는 그룹 키를 쓴다. 신원이 모호하거나
 * 세션이 바뀐 행에는 이전 선택을 승계하지 않는다. */
static void remember_position(void)
{
    int selected=selected_item(), top=ListView_GetTopIndex(app.list);
    int source=source_row_at(selected); size_t group=group_at(selected);
    if(app.shown && app.shown->session!=app.session) {
        app.selected_key=(PgIdentity){0,0}; app.top_key=(PgIdentity){0,0}; app.selected_is_group=app.top_is_group=0; return;
    }
    if(selected>=0) {
        app.selected_is_group=source<0 && group!=PG_NONE;
        app.selected_key=persistent_identity(source);
        if(app.selected_is_group) app.selected_group=app.groups->groups[group].key;
    } else { app.selected_key=(PgIdentity){0,0}; app.selected_is_group=0; }
    source=source_row_at(top); group=group_at(top);
    app.top_is_group=source<0 && group!=PG_NONE;
    app.top_key=persistent_identity(source);
    if(app.top_is_group) app.top_group=app.groups->groups[group].key;
}
static int insert_item(int visible,const char *text_value,LPARAM token)
{
    LVITEMW item; int inserted;
    memset(&item,0,sizeof(item)); item.mask=LVIF_TEXT|LVIF_PARAM; item.iItem=visible; item.lParam=token;
    item.pszText=to_wide(text_value);
    if(!item.pszText) return -1;
    inserted=ListView_InsertItem(app.list,&item); free(item.pszText); return inserted;
}
#include "native_projection.h"
static void process_item(size_t source,int visible,int indented)
{
    MonitorRow *row=&app.display->rows[source];
    char value[384],name[768]; unsigned m,ready=0;
    MonitorState overall=MONITOR_UNAVAILABLE; uint64_t feedback_expires;
    size_t group=app.groups?app.groups->owner[source]:PG_NONE;
    ResourceNameSource name_source=ui_name_source(row,display_info(source));
    if(name_source==RESOURCE_NAME_HANDLE) snprintf(name,sizeof(name),"%s%s%s",indented?"    ":"",display_name(source),group!=PG_NONE && app.groups->groups[group].key.kind==PG_OWNER_SHARED?" [공유]":"");
    else snprintf(name,sizeof(name),"%s%s%s · PID %lu%s",indented?"    ":"",display_name(source),name_source==RESOURCE_NAME_LIST?" [목록 이름]":"",(unsigned long)row->raw.pid,group!=PG_NONE && app.groups->groups[group].key.kind==PG_OWNER_SHARED?" [공유]":"");
    if(insert_item(visible,name,(LPARAM)source)<0) return;
    list_text(visible,1,ui_value(value,sizeof(value),row->raw.valid_fields&RESOURCE_VALID_CPU,row->raw.cpu_percent,MONITOR_CPU));
    list_text(visible,2,ui_value(value,sizeof(value),row->raw.valid_fields&RESOURCE_VALID_MEMORY,(double)row->raw.working_set_bytes,MONITOR_RAM));
    list_text(visible,3,ui_value(value,sizeof(value),row->raw.valid_fields&RESOURCE_VALID_IO_READ,row->raw.io_read_bytes_per_second,MONITOR_READ));
    list_text(visible,4,ui_value(value,sizeof(value),row->raw.valid_fields&RESOURCE_VALID_IO_WRITE,row->raw.io_write_bytes_per_second,MONITOR_WRITE));
    for(m=0;m<4;++m) { if(row->metric[m].state>overall) overall=row->metric[m].state; if(row->metric[m].state>=MONITOR_NORMAL) ++ready; }
    snprintf(value,sizeof(value),"%s%s%s",ui_state(overall),ready>0 && ready<4?" · 일부":"",displayed_feedback(row,&feedback_expires)?" · 억제":""); list_text(visible,5,value);
}
/* 검색·정렬은 표시 인덱스만 바꾼다. 그룹 합계는 검색 전 전체 구성원 범위를 유지하며
 * 목록에는 최신 원시값, 개별 상세 비교에는 별도 완료창 보고를 사용한다. */
static void render_list(void)
{
    size_t i,shown=0,headers=0,hidden=0,filtered=0;
    size_t *rows=NULL,*groups=NULL;
    int visible=0,selected=-1,restored_top=-1,top=ListView_GetTopIndex(app.list),sort_failed=0;
    size_t wanted_row=pg_find_identity(current_relations(),app.selected_key),wanted_group=PG_NONE;
    char status[1400],subtitle[512],observed[64]; LARGE_INTEGER begin,end,freq;
    if(!app.display) return;
    QueryPerformanceCounter(&begin); QueryPerformanceFrequency(&freq);
    if(app.sort_mode && app.display->count<=SIZE_MAX/sizeof(*rows)) rows=malloc(app.display->count*sizeof(*rows));
    if(rows) {
        if(app.flat || !app.groups) { for(i=0;i<app.display->count;++i) rows[i]=i; qsort_s(rows,app.display->count,sizeof(*rows),ui_source_compare,NULL); }
        else {
            memcpy(rows,app.groups->members,app.display->count*sizeof(*rows));
            for(i=0;i<app.groups->count;++i) qsort_s(rows+app.groups->groups[i].first,app.groups->groups[i].count,sizeof(*rows),ui_source_compare,NULL);
        }
    }
    if(app.sort_mode && app.groups && app.groups->count<=SIZE_MAX/sizeof(*groups)) groups=malloc(app.groups->count*sizeof(*groups));
    if(groups) { memcpy(groups,app.groups->order,app.groups->count*sizeof(*groups)); qsort_s(groups,app.groups->count,sizeof(*groups),ui_group_compare,NULL); }
    sort_failed=app.sort_mode && ((app.display->count && !rows) || (!app.flat && app.groups && app.groups->count && !groups));
    if(sort_failed) { free(rows); free(groups); rows=NULL; groups=NULL; }
    if(app.groups) {
        if(app.selected_is_group) wanted_group=pg_group_find(app.groups,app.selected_group);
        else if(wanted_row!=PG_NONE) wanted_group=app.groups->owner[wanted_row];
    }
    app.updating=1; SendMessageW(app.list,WM_SETREDRAW,FALSE,0); ListView_DeleteAllItems(app.list);
    if(app.flat || !app.groups) {
        for(i=0;i<app.display->count;++i) {
            size_t source=rows?rows[i]:i;
            if(!ui_matches_row(source)) { ++filtered; continue; }
            process_item(source,visible,0);
            if(pg_identity_equal(persistent_identity((int)source),app.selected_key)) selected=visible;
            if(pg_identity_equal(persistent_identity((int)source),app.top_key)) restored_top=visible;
            ++visible; ++shown;
        }
    } else for(i=0;i<app.groups->count;++i) {
        size_t gi=groups?groups[i]:app.groups->order[i],j; PgGroup *g=&app.groups->groups[gi];
        int expanded=pg_group_expanded(app.membership,g->key);
        int group_name_matches=ui_matches_text(display_group_name(gi));
        if(!ui_group_matches(gi)) { filtered+=g->count; continue; }
        if(g->count>1) {
            char label[768],value[128]; unsigned metric;
            snprintf(label,sizeof(label),"%s %s%s · %zu개",expanded?"−":"+",display_group_name(gi),display_group_name_note(gi),g->count);
            insert_item(visible,label,-(LPARAM)gi-1);
            for(metric=0;metric<MONITOR_METRICS;++metric) list_text(visible,1+(int)metric,sum_text(&g->metric[metric],value,sizeof(value),metric));
            list_text(visible,5,"현재 합계");
            if(wanted_group==gi && (app.selected_is_group || !expanded)) selected=visible;
            if(app.top_is_group && pg_group_key_equal(g->key,app.top_group)) restored_top=visible;
            if(!app.top_is_group) { size_t row=pg_find_identity(current_relations(),app.top_key); if(row!=PG_NONE && app.groups->owner[row]==gi && !expanded) restored_top=visible; }
            ++visible; ++headers;
            if(!expanded) { hidden+=g->count; continue; }
        }
        for(j=0;j<g->count;++j) {
            size_t row=rows?rows[g->first+j]:app.groups->members[g->first+j];
            if(!group_name_matches && !ui_matches_row(row)) { ++filtered; continue; }
            process_item(row,visible,g->count>1);
            if(pg_identity_equal(persistent_identity((int)row),app.selected_key)) selected=visible;
            if(g->count==1 && wanted_group==gi && app.selected_is_group) selected=visible;
            if(pg_identity_equal(persistent_identity((int)row),app.top_key)) restored_top=visible;
            ++visible; ++shown;
        }
    }
    free(rows); free(groups);
    if(selected>=0) ListView_SetItemState(app.list,selected,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    app.updating=0; SendMessageW(app.list,WM_SETREDRAW,TRUE,0);
    if(visible) {
        RECT bounds;
        if(restored_top<0) restored_top=top<visible?top:visible-1;
        if(ListView_GetItemRect(app.list,0,&bounds,LVIR_BOUNDS)) ListView_Scroll(app.list,0,(restored_top-ListView_GetTopIndex(app.list))*(bounds.bottom-bounds.top));
    }
    InvalidateRect(app.list,NULL,TRUE);
    QueryPerformanceCounter(&end);
    snprintf(status,sizeof(status),"%s · 원본 %zu개 · 표시 %zu개 / 접힘 %zu개 / 검색 제외 %zu개 · 그룹 %zu개%s%s",
        app.stopping?"중단 중":app.display->storage_error?"저장 오류 · 수집 중단":app.display->collector_error?"수집 오류 · 수집 중단":app.worker_error?"수집 작업 중단 · 콘솔 진단 확인":app.pause_requested?"일시정지 · 표시 보존":app.shown && app.shown->session!=app.session?"재개 · 이전 세션 표시 보존 / 새 표본 대기":app.ui_demo?"합성 화면 검수 · 수집/저장 없음":
            !app.synthetic && monitor_utc_now()>app.display->measured_utc_100ns+UINT64_C(150000000)?"표본 오래됨 · 마지막 표본 표시":"로컬 수집 중",
        app.display->count,shown,hidden,filtered,headers,app.groups?"":" · 관계 정보 없음",sort_failed?" · 정렬 메모리 부족: 기본 순서로 표시":"");
    snprintf(subtitle,sizeof(subtitle),"%s최근 표본 %s · CPU %% / RAM GB / 읽기·쓰기 MB/s%s",
        app.shown && app.shown->session!=app.session?"이전 세션 · ":app.ui_demo?"합성 · ":"",utc_text(app.display->measured_utc_100ns,observed,sizeof(observed)),app.ui_scale?" · 배율 모사":"");
    set_text(GetDlgItem(app.window,ID_SUBTITLE),subtitle);
    set_text(app.status,status); show_detail();
    if(app.diagnostics) { printf("ui_refresh sequence=%llu processes=%zu headers=%zu shown=%zu hidden=%zu render_ms=%.3f\n",(unsigned long long)app.display->sequence,app.display->count,headers,shown,hidden,(double)(end.QuadPart-begin.QuadPart)*1000/(double)freq.QuadPart); fflush(stdout); }
}
static void rebuild_groups(void)
{
    LARGE_INTEGER begin,end,freq;
    QueryPerformanceCounter(&begin); QueryPerformanceFrequency(&freq);
    pg_groups_free(app.groups); app.groups=NULL;
    if(!app.no_grouping && current_relations() && app.membership) pg_groups_build(app.membership,app.display,current_relations(),&app.groups);
    QueryPerformanceCounter(&end);
    if(app.diagnostics) { printf("ui_membership sequence=%llu grouping_build_ms=%.3f\n",(unsigned long long)(app.display?app.display->sequence:0),(double)(end.QuadPart-begin.QuadPart)*1000/(double)freq.QuadPart); fflush(stdout); }
    render_list();
}
static void consume_snapshot(void)
{
    GuiSnapshot *s;
    /* 잠금 아래 latest에서 분리한다. 이 시점부터 묶음은 UI만 소유한다. */
    AcquireSRWLockExclusive(&app.lock); s=app.latest; app.latest=NULL; ReleaseSRWLockExclusive(&app.lock);
    if(!s) return;
    remember_position();
    pg_groups_free(app.groups); app.groups=NULL;
    /* 기존 그룹 인덱스와 표시 묶음을 폐기한 뒤 새 display를 빌리고 그룹을 다시 구성한다. */
    gui_snapshot_free(app.shown); app.shown=s; app.display=s->metrics;
    feedback_reconcile();
    rebuild_groups();
}
static void toggle_group(int action)
{
    size_t group=group_at(selected_item()); int row=selected_row();
    PgGroup *g;
    if(app.flat || !app.groups || group==PG_NONE) return;
    g=&app.groups->groups[group];
    if(row>=0 && action>0) return;
    if(g->count<2) return;
    remember_position();
    pg_group_set_expanded(app.membership,g->key,action==0?!pg_group_expanded(app.membership,g->key):action>0);
    app.selected_is_group=1; app.selected_group=g->key; app.selected_key=(PgIdentity){0,0};
    render_list();
}
static void layout(HWND window)
{
    RECT r; int width,height,pad=ui_px(16),gap=ui_px(16),right,left,rx,y,bh=ui_px(32),third;
    GetClientRect(window,&r); width=r.right; height=r.bottom;
    right=width/3; if(right<ui_px(344)) right=ui_px(344); if(right>ui_px(460)) right=ui_px(460);
    left=width-pad*2-gap-right; rx=pad+left+gap;
    MoveWindow(GetDlgItem(window,ID_TITLE),pad,ui_px(10),width-ui_px(200),ui_px(36),TRUE);
    MoveWindow(GetDlgItem(window,ID_SUBTITLE),pad,ui_px(50),width-pad*2,ui_px(26),TRUE);
    MoveWindow(app.pause,width-pad-ui_px(112),ui_px(16),ui_px(112),bh,TRUE);
    MoveWindow(GetDlgItem(window,ID_LIST_TITLE),pad,ui_px(88),ui_px(170),ui_px(28),TRUE);
    MoveWindow(GetDlgItem(window,ID_FLAT),pad+left-ui_px(264),ui_px(84),ui_px(88),bh,TRUE);
    MoveWindow(GetDlgItem(window,ID_EXPAND),pad+left-ui_px(172),ui_px(84),ui_px(82),bh,TRUE);
    MoveWindow(GetDlgItem(window,ID_COLLAPSE),pad+left-ui_px(86),ui_px(84),ui_px(86),bh,TRUE);
    MoveWindow(GetDlgItem(window,ID_DETAIL_TITLE),rx,ui_px(88),right,ui_px(28),TRUE);
    MoveWindow(GetDlgItem(window,ID_SEARCH),pad,ui_px(124),left-ui_px(208),ui_px(32),TRUE);
    MoveWindow(GetDlgItem(window,ID_SORT),pad+left-ui_px(200),ui_px(124),ui_px(200),ui_px(320),TRUE);
    MoveWindow(GetDlgItem(window,ID_METRIC),rx,ui_px(124),right,ui_px(240),TRUE);
    MoveWindow(app.list,pad,ui_px(164),left,height-ui_px(282),TRUE);
    MoveWindow(app.detail,rx,ui_px(164),right,height-ui_px(282),TRUE);
    y=height-ui_px(108); third=(left-ui_px(16))/3;
    MoveWindow(GetDlgItem(window,ID_CONFIRM),pad,y,third,bh,TRUE);
    MoveWindow(GetDlgItem(window,ID_TARGET),pad+third+ui_px(8),y,third,bh,TRUE);
    MoveWindow(GetDlgItem(window,ID_MOVE),pad+2*(third+ui_px(8)),y,third,bh,TRUE);
    MoveWindow(GetDlgItem(window,ID_SHARED),pad,y+ui_px(38),third,bh,TRUE);
    MoveWindow(GetDlgItem(window,ID_AUTO),pad+third+ui_px(8),y+ui_px(38),third,bh,TRUE);
    MoveWindow(GetDlgItem(window,ID_IGNORE),rx,y,(right-ui_px(8))/2,bh,TRUE);
    MoveWindow(GetDlgItem(window,ID_EXPECTED),rx+(right+ui_px(8))/2,y,(right-ui_px(8))/2,bh,TRUE);
    MoveWindow(GetDlgItem(window,ID_CANCEL),rx,y+ui_px(38),right,bh,TRUE);
    MoveWindow(app.status,pad,height-ui_px(28),width-pad*2,ui_px(24),TRUE);
    ui_columns(left);
}
/* UI 스레드에서 worker를 즉시 기다리지 않는다. stop을 알리고 timer/메시지를 계속
 * 처리하다 종료를 관측하면 창을 닫아, DB 정리 중에도 화면 응답을 유지한다. */
static void begin_close(void)
{
    if (app.stopping) return;
    app.stopping = 1; app.stop_started = GetTickCount64();
    set_text(app.status, "중단 중 · worker가 현재 작업을 마치고 로컬 DB를 닫을 때까지 기다립니다…");
    SetEvent(app.stop); EnableWindow(app.pause, FALSE);
    if(!app.worker) DestroyWindow(app.window);
}
static void consume_feedback(void)
{
    FeedbackCommand results[FEEDBACK_LIMIT];
    unsigned i, n, succeeded=0, failed=0, cancelled=0;
    char message[256];
    /* 표시 frame이 없거나 재개한 뒤라도 모든 종결 응답을 소비해 접수 용량을 반환한다.
       실패·미실행 취소는 기존 저장 성공을 덮지 않으며 성공 표시는 별도로 정리한다. */
    AcquireSRWLockExclusive(&app.lock);
    n = app.feedback_result_count; memcpy(results, app.feedback_results, n * sizeof(*results)); app.feedback_result_count = 0;
    app.feedback_outstanding-=n;
    ReleaseSRWLockExclusive(&app.lock);
    if (!n) return;
    for (i = 0; i < n; ++i) {
        FeedbackOverlay *entry=feedback_overlay_find(results[i].program);
        if(entry && entry->pending) --entry->pending;
        if(results[i].outcome==FEEDBACK_SUCCEEDED) {
            ++succeeded;
            if(entry && results[i].revision>entry->revision) {
                entry->kind=results[i].kind; entry->expires=results[i].expires; entry->revision=results[i].revision;
            }
        } else if(results[i].outcome==FEEDBACK_FAILED) ++failed;
        else if(results[i].outcome==FEEDBACK_CANCELLED) ++cancelled;
        printf("feedback request=%llu session=%u program=%lld outcome=%s revision=%llu\n",
            (unsigned long long)results[i].request_id,results[i].session,(long long)results[i].program,
            results[i].outcome==FEEDBACK_SUCCEEDED?"stored":results[i].outcome==FEEDBACK_FAILED?"failed":"cancelled",
            (unsigned long long)results[i].revision);
    }
    feedback_reconcile(); fflush(stdout);
    if(failed || cancelled) snprintf(message,sizeof(message),"알림 억제 요청 결과 · 저장 %u건 / 실패 %u건 / 실행 전 취소 %u건",succeeded,failed,cancelled);
    else snprintf(message,sizeof(message),"알림 억제 로컬 저장 확인 · %u건. 후보 근거와 평소 기준은 유지됩니다.",succeeded);
    if(app.window && IsWindow(app.window) && !app.stopping) { set_text(app.status,message); show_detail(); }
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message) {
    case WM_CREATE: {
        const wchar_t *columns[] = { L"프로그램 / 구성원", L"CPU %", L"RAM GB", L"읽기 MB/s", L"쓰기 MB/s", L"개별 완료창" };
        unsigned i;
        const wchar_t *sorts[]={L"기본 순서",L"이름 · 오름차순",L"이름 · 내림차순",L"CPU · 높은 순",L"CPU · 낮은 순",L"RAM · 높은 순",L"RAM · 낮은 순",L"읽기 · 높은 순",L"읽기 · 낮은 순",L"쓰기 · 높은 순",L"쓰기 · 낮은 순"};
        const wchar_t *metrics[]={L"CPU · 개별 완료창 근거",L"RAM · 개별 완료창 근거",L"읽기 · 개별 완료창 근거",L"쓰기 · 개별 완료창 근거"};
        app.window = window;
        app.dpi=ui_window_dpi(window);
        app.background_brush=CreateSolidBrush(UI_BACKGROUND); app.panel_brush=CreateSolidBrush(RGB(255,255,255));
        CreateWindowW(L"STATIC",L"내 PC의 사용 흐름",WS_CHILD|WS_VISIBLE,0,0,0,0,window,(HMENU)(INT_PTR)ID_TITLE,NULL,NULL);
        CreateWindowW(L"STATIC",app.ui_demo?L"합성 화면 검수 · 실제 수집 / Baseline 판정 / 저장을 실행하지 않습니다":L"지금의 사용량을 보고, 개별 프로세스의 평소와 비교합니다",WS_CHILD|WS_VISIBLE,0,0,0,0,window,(HMENU)(INT_PTR)ID_SUBTITLE,NULL,NULL);
        CreateWindowW(L"STATIC",L"현재 사용량",WS_CHILD|WS_VISIBLE,0,0,0,0,window,(HMENU)(INT_PTR)ID_LIST_TITLE,NULL,NULL);
        CreateWindowW(L"STATIC",L"선택한 항목의 근거",WS_CHILD|WS_VISIBLE,0,0,0,0,window,(HMENU)(INT_PTR)ID_DETAIL_TITLE,NULL,NULL);
        app.status = CreateWindowW(L"STATIC", L"로컬 수집을 준비하고 있습니다…", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, NULL, NULL, NULL);
        app.list = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS | WS_BORDER,
            0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_LIST, NULL, NULL);
        ListView_SetExtendedListViewStyle(app.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        for (i = 0; i < sizeof(columns)/sizeof(columns[0]); ++i) {
            LVCOLUMNW c; memset(&c, 0, sizeof(c)); c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            c.pszText = (wchar_t *)columns[i]; c.cx = ui_px(100); c.fmt=i>0 && i<5?LVCFMT_RIGHT:LVCFMT_LEFT;
            ListView_InsertColumn(app.list, (int)i, &c);
        }
        app.detail = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | WS_BORDER,
            0, 0, 0, 0, window, NULL, NULL, NULL);
        CreateWindowW(L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,0,0,0,0,window,(HMENU)(INT_PTR)ID_SEARCH,NULL,NULL);
        SendMessageW(GetDlgItem(window,ID_SEARCH),EM_SETCUEBANNER,TRUE,(LPARAM)L"이름 / 경로 / PID 검색");
        SendMessageW(GetDlgItem(window,ID_SEARCH),EM_SETLIMITTEXT,255,0);
        CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,0,0,0,0,window,(HMENU)(INT_PTR)ID_SORT,NULL,NULL);
        CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,0,0,0,0,window,(HMENU)(INT_PTR)ID_METRIC,NULL,NULL);
        for(i=0;i<sizeof(sorts)/sizeof(sorts[0]);++i) SendDlgItemMessageW(window,ID_SORT,CB_ADDSTRING,0,(LPARAM)sorts[i]);
        for(i=0;i<sizeof(metrics)/sizeof(metrics[0]);++i) SendDlgItemMessageW(window,ID_METRIC,CB_ADDSTRING,0,(LPARAM)metrics[i]);
        SendDlgItemMessageW(window,ID_SORT,CB_SETCURSEL,0,0); SendDlgItemMessageW(window,ID_METRIC,CB_SETCURSEL,0,0);
        app.pause = CreateWindowW(L"BUTTON", L"일시정지", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_PAUSE, NULL, NULL);
        CreateWindowW(L"BUTTON", L"1시간 알림 억제", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_IGNORE, NULL, NULL);
        CreateWindowW(L"BUTTON", L"24시간 예상 사용", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_EXPECTED, NULL, NULL);
        CreateWindowW(L"BUTTON", L"이 경로의 알림 억제 취소", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_CANCEL, NULL, NULL);
        CreateWindowW(L"BUTTON",L"개별 목록",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,window,(HMENU)(INT_PTR)ID_FLAT,NULL,NULL);
        CreateWindowW(L"BUTTON",L"모두 펼침",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,window,(HMENU)(INT_PTR)ID_EXPAND,NULL,NULL);
        CreateWindowW(L"BUTTON",L"모두 접기",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,window,(HMENU)(INT_PTR)ID_COLLAPSE,NULL,NULL);
        CreateWindowW(L"BUTTON",L"현재 구성원 확정",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,window,(HMENU)(INT_PTR)ID_CONFIRM,NULL,NULL);
        CreateWindowW(L"BUTTON",L"이동 대상 지정",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,window,(HMENU)(INT_PTR)ID_TARGET,NULL,NULL);
        CreateWindowW(L"BUTTON",L"대상으로 이동",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,window,(HMENU)(INT_PTR)ID_MOVE,NULL,NULL);
        CreateWindowW(L"BUTTON",L"공유 / 별도 분리",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,window,(HMENU)(INT_PTR)ID_SHARED,NULL,NULL);
        CreateWindowW(L"BUTTON",L"자동 추정으로",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,window,(HMENU)(INT_PTR)ID_AUTO,NULL,NULL);
        ListView_SetTextColor(app.list,UI_INK); ListView_SetBkColor(app.list,RGB(255,255,255)); ListView_SetTextBkColor(app.list,CLR_NONE);
        ui_fonts(); show_detail();
        app.last_tick = GetTickCount64(); SetTimer(window, 1, 100, NULL);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *info=(MINMAXINFO *)lp; RECT minimum={0,0,ui_px(960),ui_px(640)};
        AdjustWindowRectEx(&minimum,WS_OVERLAPPEDWINDOW,FALSE,0);
        info->ptMinTrackSize.x=minimum.right-minimum.left; info->ptMinTrackSize.y=minimum.bottom-minimum.top; return 0;
    }
    case WM_DPICHANGED: {
        RECT *suggested=(RECT *)lp;
        if(!app.ui_scale) app.dpi=HIWORD(wp);
        ui_fonts();
        SetWindowPos(window,NULL,suggested->left,suggested->top,suggested->right-suggested->left,suggested->bottom-suggested->top,SWP_NOACTIVATE|SWP_NOZORDER);
        layout(window); return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp,(HWND)lp==app.status?UI_MUTED:UI_INK);
        SetBkColor((HDC)wp,(HWND)lp==app.detail?RGB(255,255,255):UI_BACKGROUND);
        return (LRESULT)((HWND)lp==app.detail?app.panel_brush:app.background_brush);
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp,UI_INK); SetBkColor((HDC)wp,RGB(255,255,255)); return (LRESULT)app.panel_brush;
    case WM_ERASEBKGND: {
        RECT bounds; GetClientRect(window,&bounds); FillRect((HDC)wp,&bounds,app.background_brush); return 1;
    }
    case WM_SIZE: layout(window); return 0;
    case WM_SNAPSHOT: consume_snapshot(); consume_feedback(); return 0;
    case WM_NOTIFY:
        if(((NMHDR *)lp)->hwndFrom==ListView_GetHeader(app.list) && ((NMHDR *)lp)->code==HDN_ENDTRACKW && !app.setting_columns) {
            NMHEADERW *header=(NMHEADERW *)lp;
            if(header->iItem==0 && header->pitem && (header->pitem->mask&HDI_WIDTH)) app.name_width_dip=MulDiv(header->pitem->cxy,96,(int)app.dpi);
        }
        if(((NMHDR *)lp)->idFrom==ID_LIST && ((NMHDR *)lp)->code==NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *draw=(NMLVCUSTOMDRAW *)lp;
            if(draw->nmcd.dwDrawStage==CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if(draw->nmcd.dwDrawStage==CDDS_ITEMPREPAINT) {
                LVITEMW item; memset(&item,0,sizeof(item)); item.mask=LVIF_PARAM; item.iItem=(int)draw->nmcd.dwItemSpec;
                if(ListView_GetItem(app.list,&item) && item.lParam<0) { draw->clrText=UI_TEAL; draw->clrTextBk=RGB(233,245,244); }
                else { draw->clrText=UI_INK; draw->clrTextBk=draw->nmcd.dwItemSpec%2?RGB(248,250,251):RGB(255,255,255); }
                return CDRF_DODEFAULT;
            }
        }
        if(!app.updating && ((NMHDR *)lp)->idFrom==ID_LIST) {
            if(((NMHDR *)lp)->code==LVN_ITEMCHANGED) show_detail();
            else if(((NMHDR *)lp)->code==LVN_COLUMNCLICK) {
                NMLISTVIEW *event=(NMLISTVIEW *)lp; int mode=event->iSubItem==0?1:event->iSubItem<5?event->iSubItem*2+1:0;
                if(mode) { remember_position(); app.sort_mode=app.sort_mode==mode?mode+1:mode; SendDlgItemMessageW(window,ID_SORT,CB_SETCURSEL,(WPARAM)app.sort_mode,0); render_list(); }
            }
            else if(((NMHDR *)lp)->code==NM_DBLCLK) { NMLISTVIEW *event=(NMLISTVIEW *)lp; if(event->iSubItem!=0 || event->ptAction.x>=ui_px(40)) toggle_group(0); }
            else if(((NMHDR *)lp)->code==NM_CLICK) {
                NMLISTVIEW *event=(NMLISTVIEW *)lp;
                if(event->iSubItem==0 && event->ptAction.x<ui_px(40) && source_row_at(event->iItem)<0) toggle_group(0);
            } else if(((NMHDR *)lp)->code==LVN_KEYDOWN) {
                NMLVKEYDOWN *event=(NMLVKEYDOWN *)lp;
                if(event->wVKey==VK_LEFT) toggle_group(-1);
                else if(event->wVKey==VK_RIGHT) toggle_group(1);
            }
        }
        return 0;
    case WM_COMMAND:
        if(LOWORD(wp)==ID_SEARCH && HIWORD(wp)==EN_CHANGE && !app.updating) {
            remember_position(); GetDlgItemTextW(window,ID_SEARCH,app.search,(int)(sizeof(app.search)/sizeof(app.search[0]))); render_list();
        } else if(LOWORD(wp)==ID_SORT && HIWORD(wp)==CBN_SELCHANGE) {
            remember_position(); app.sort_mode=(int)SendDlgItemMessageW(window,ID_SORT,CB_GETCURSEL,0,0); render_list();
        } else if(LOWORD(wp)==ID_METRIC && HIWORD(wp)==CBN_SELCHANGE) {
            app.detail_metric=(int)SendDlgItemMessageW(window,ID_METRIC,CB_GETCURSEL,0,0); show_detail();
        } else if (LOWORD(wp) == ID_PAUSE && !app.stopping) {
            AcquireSRWLockExclusive(&app.lock); app.pause_requested = !app.pause_requested;
            if(!app.pause_requested) ++app.session;
            ReleaseSRWLockExclusive(&app.lock); SetEvent(app.wake);
            if(!app.pause_requested) {
                pg_membership_reset(app.membership); app.selected_key=(PgIdentity){0,0}; app.top_key=(PgIdentity){0,0}; app.selected_is_group=app.top_is_group=0;
                if(app.display) rebuild_groups();
            }
            if(app.ui_demo && !app.pause_requested) { app.latest=ui_demo_snapshot(); if(app.latest) { ui_demo_first_sample(app.latest); consume_snapshot(); } }
            SetWindowTextW(app.pause, app.pause_requested ? L"재개" : L"일시정지");
            set_text(app.status, app.pause_requested ? "일시정지 · 진행 중 회차 뒤 수집을 멈춥니다. 표시와 세션 소속을 유지하고 미완료창은 폐기합니다." : "재개 · 세션 소속/대상/펼침/선택 초기화. 새 표본과 완료창을 기다립니다.");
        } else if(LOWORD(wp)>=ID_FLAT && LOWORD(wp)<=ID_AUTO && !app.stopping) {
            int action=LOWORD(wp),ok=1,row=selected_row(); size_t group=group_at(selected_item());
            remember_position();
            if(action==ID_FLAT) { app.flat=!app.flat; SetWindowTextW(GetDlgItem(window,ID_FLAT),app.flat?L"그룹 목록":L"개별 목록"); render_list(); }
            else if(action==ID_EXPAND || action==ID_COLLAPSE) { pg_groups_expand_all(app.membership,app.groups,action==ID_EXPAND); render_list(); }
            else {
                if(action==ID_CONFIRM || action==ID_TARGET) {
                    if(row>=0) app.selected_key=pg_row_identity(&app.display->rows[row]);
                    else if(group!=PG_NONE && app.groups->groups[group].count) app.selected_key=pg_row_identity(&app.display->rows[app.groups->members[app.groups->groups[group].first]]);
                    app.selected_is_group=0;
                    if(action==ID_TARGET && row>=0) ok=pg_set_process_target(app.membership,app.display,current_relations(),(size_t)row);
                    else ok=pg_confirm_members(app.membership,app.groups,group,app.display,current_relations(),action==ID_TARGET);
                } else if(action==ID_MOVE) ok=row>=0 && pg_move_to_target(app.membership,app.display,current_relations(),(size_t)row);
                else if(action==ID_SHARED) ok=row>=0 && pg_separate_shared(app.membership,app.display,current_relations(),(size_t)row);
                else if(action==ID_AUTO) ok=row>=0 && pg_restore_auto(app.membership,pg_row_identity(&app.display->rows[row]));
                rebuild_groups();
                if(!ok) set_text(app.status,"소속 변경 불가 · 현재 신원이 확인된 구성원과 이동 대상을 선택하세요. 최대 8,192개 세션 지정. 변경을 적용하지 않았습니다.");
            }
        } else if (LOWORD(wp) >= ID_IGNORE && LOWORD(wp) <= ID_CANCEL && !app.stopping && !app.worker_error && !app.ui_demo) {
            int selected = selected_row();
            if (selected >= 0 && app.display && (size_t)selected < app.display->count && app.display->rows[selected].program_id) {
                MonitorFeedback kind = LOWORD(wp) == ID_IGNORE ? MONITOR_FEEDBACK_IGNORE :
                    LOWORD(wp) == ID_EXPECTED ? MONITOR_FEEDBACK_EXPECTED : MONITOR_FEEDBACK_CANCEL;
                FeedbackSubmit submitted=feedback_submit(app.display->rows[selected].program_id,kind);
                set_text(app.status, submitted==FEEDBACK_ACCEPTED ? "알림 억제 저장 요청 중 · worker의 로컬 저장 확인을 기다립니다. 측정/학습/후보 근거는 유지됩니다." :
                    submitted==FEEDBACK_BUSY?"저장 요청 처리 한도에 도달했습니다. 저장하지 않았습니다. 결과를 확인한 뒤 다시 시도하세요.":
                    submitted==FEEDBACK_NO_MEMORY?"저장 확인 표시를 준비할 메모리가 부족합니다. 저장 요청을 접수하지 않았습니다.":
                    "수집 작업이 종료되어 저장 요청을 접수하지 않았습니다.");
            } else set_text(app.status, "검증 가능한 실행 경로가 있는 개별 프로세스를 선택하세요.");
        }
        return 0;
    case WM_TIMER: {
        uint64_t now = GetTickCount64(), gap = now - app.last_tick;
        app.last_tick = now; ++app.heartbeat;
        if (gap > app.max_heartbeat_gap) app.max_heartbeat_gap = gap;
        consume_snapshot(); consume_feedback();
        if (!app.stopping && app.worker && WaitForSingleObject(app.worker, 0) == WAIT_OBJECT_0) {
            if (app.count && app.completed >= app.count) begin_close();
            else if (app.worker_error) EnableWindow(app.pause, FALSE);
        }
        if (app.stopping && app.worker && WaitForSingleObject(app.worker, 0) == WAIT_OBJECT_0) DestroyWindow(window);
        else if (app.display && !app.stopping && !app.worker_error && !app.pause_requested && (!app.shown || app.shown->session==app.session) && !app.display->storage_error && !app.display->collector_error &&
            monitor_utc_now() > app.display->measured_utc_100ns + UINT64_C(150000000) && !app.synthetic && !app.ui_demo)
            set_stale_status(app.status);
        return 0;
    }
    case WM_WORKER_DONE:
        consume_snapshot(); consume_feedback();
        if(app.worker_error && (!app.display || (!app.display->storage_error && !app.display->collector_error))) set_text(app.status,"수집 작업 중단 · 새 오류 표본을 만들지 못했습니다(메모리 할당 실패 포함). 콘솔 진단을 확인하세요.");
        if (app.count && app.completed >= app.count) begin_close();
        else if (app.worker_error) EnableWindow(app.pause, FALSE);
        update_actions();
        return 0;
    case WM_CLOSE: begin_close(); return 0;
    case WM_DESTROY: KillTimer(window, 1); ui_dispose(); PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window, message, wp, lp);
    }
}
int wmain(int argc, wchar_t **argv)
{
    INITCOMMONCONTROLSEX common;
    WNDCLASSW wc;
    MSG msg;
    int i, result = 0;
    memset(&app, 0, sizeof(app)); app.interval_ms = 3000; app.session=1;
    app.membership=pg_membership_create();
    snprintf(app.db, sizeof(app.db), "monitor-data.sqlite");
    for (i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--headless")) app.headless = 1;
        else if (!wcscmp(argv[i], L"--stats")) app.stats = 1;
        else if (!wcscmp(argv[i], L"--no-grouping")) app.no_grouping=1;
        else if (!wcscmp(argv[i], L"--diagnostics")) app.diagnostics=1;
        else if (!wcscmp(argv[i], L"--synthetic")) app.synthetic = 1;
        else if (!wcscmp(argv[i], L"--ui-demo")) app.ui_demo = 1;
        else if (!wcscmp(argv[i], L"--ui-scale") && i + 1 < argc) app.ui_scale=(unsigned)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--ui-size") && i + 1 < argc) {
            wchar_t *separator,*tail; unsigned long width=wcstoul(argv[++i],&separator,10),height=0;
            if(*separator==L'x') height=wcstoul(separator+1,&tail,10); else tail=separator;
            if(*separator!=L'x' || *tail || width<960 || height<640 || width>3840 || height>2160) {
                fprintf(stderr,"UI size expects WIDTHxHEIGHT DIP within 960x640..3840x2160\n"); return 2;
            }
            app.ui_width=(unsigned)width; app.ui_height=(unsigned)height;
        }
        else if (!wcscmp(argv[i], L"--count") && i + 1 < argc) app.count = (unsigned)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--interval-ms") && i + 1 < argc) app.interval_ms = (unsigned)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--db") && i + 1 < argc) {
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[++i], -1, app.db, sizeof(app.db), NULL, NULL)) return 2;
        } else {
            fwprintf(stderr, L"Unrecognized or incomplete argument: %ls\n", argv[i]);
            printf("monitor_gui [--headless] [--stats] [--db LOCAL_PATH] [--count N] [--interval-ms N] [--synthetic] [--no-grouping] [--diagnostics] [--ui-demo] [--ui-scale 100|125|150|200] [--ui-size 960x640]\n"); return 2;
        }
    }
    if (app.interval_ms < 1 || app.interval_ms > 60000 || (app.synthetic && !strcmp(app.db, "monitor-data.sqlite"))) {
        fprintf(stderr, "interval must be 1..60000; SYNTHETIC requires explicit separate --db\n"); return 2;
    }
    if((app.ui_scale && app.ui_scale!=100 && app.ui_scale!=125 && app.ui_scale!=150 && app.ui_scale!=200) || (app.ui_demo && app.headless)) {
        fprintf(stderr,"UI scale accepts 100/125/150/200; UI demo requires a window\n"); return 2;
    }
    InitializeSRWLock(&app.lock); app.stop = CreateEventW(NULL, TRUE, FALSE, NULL); app.wake = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!app.stop || !app.wake) return 1;
    SetConsoleCtrlHandler(console_handler, TRUE);
    if (!app.headless) {
        RECT initial={0,0,0,0}; wchar_t title[256];
        ui_enable_dpi(); app.dpi=ui_window_dpi(NULL);
        common.dwSize = sizeof(common); common.dwICC = ICC_LISTVIEW_CLASSES; InitCommonControlsEx(&common);
        memset(&wc, 0, sizeof(wc)); wc.lpfnWndProc = window_proc; wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"LocalResourceMonitorV1"; wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); RegisterClassW(&wc);
        swprintf(title,sizeof(title)/sizeof(title[0]),L"%ls내 PC 자원 모니터%ls",app.ui_demo?L"[합성 화면 검수] ":app.synthetic?L"[합성 수집] ":L"",app.ui_scale?L" · 배율 모사 (실제 OS DPI 시험 아님)":L"");
        initial.right=ui_px(app.ui_width?(int)app.ui_width:1320); initial.bottom=ui_px(app.ui_height?(int)app.ui_height:820); AdjustWindowRectEx(&initial,WS_OVERLAPPEDWINDOW,FALSE,0);
        if (!CreateWindowW(wc.lpszClassName,title,
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, initial.right-initial.left,initial.bottom-initial.top, NULL, NULL, wc.hInstance, NULL)) return 1;
        if(app.diagnostics) { RECT client; GetClientRect(app.window,&client); printf("ui_layout dpi=%u simulated_scale=%u client_px=%ldx%ld name_column_px=%d row_dip=36 body_pt=11\n",app.dpi,app.ui_scale,client.right,client.bottom,ListView_GetColumnWidth(app.list,0)); fflush(stdout); }
    }
    if(app.ui_demo) { app.latest=ui_demo_snapshot(); if(!app.latest) { DestroyWindow(app.window); return 1; } consume_snapshot(); }
    else {
        app.worker = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
        if (!app.worker) { if (app.window) DestroyWindow(app.window); return 1; }
    }
    if (app.headless) WaitForSingleObject(app.worker, INFINITE);
    else while ((result = (int)GetMessageW(&msg, NULL, 0, 0)) > 0) {
        if (!IsDialogMessageW(app.window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    SetEvent(app.stop); if(app.worker) WaitForSingleObject(app.worker, INFINITE);
    /* 닫기/WM_QUIT이 마지막 worker 알림보다 먼저 올 수 있다. worker 종료를 기다린 뒤
       예약 UI 상태를 해제하기 전에 마지막 응답을 소비한다. 이 단계에서 새 쓰기는 하지 않는다. */
    consume_feedback();
    printf("exit batches=%u ui_heartbeat=%llu max_ui_gap_ms=%llu graceful_stop_ms=%llu\n", app.completed,
        (unsigned long long)app.heartbeat, (unsigned long long)app.max_heartbeat_gap,
        (unsigned long long)(app.stop_started ? GetTickCount64() - app.stop_started : 0));
    if (app.headless && app.latest) printf("final_status=%s\n", app.latest->metrics->status);
    pg_groups_free(app.groups); pg_membership_destroy(app.membership);
    gui_snapshot_free(app.latest); gui_snapshot_free(app.shown);
    feedback_dispose();
    if(app.worker) CloseHandle(app.worker); CloseHandle(app.stop); CloseHandle(app.wake);
    return app.worker_error || result < 0 ? 1 : 0;
}
