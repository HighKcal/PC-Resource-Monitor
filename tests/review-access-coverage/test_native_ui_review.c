/* Independent source-inclusion tests. Hidden native controls are unit fixtures,
 * never visible app automation. Collector/engine ingress is deterministic and
 * no database or real collector is opened. Product source is not edited. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include "monitor.h"
#include "resource_display.h"
static int review_open(const char *,int,MonitorEngine **,char *,size_t);
static void review_close(MonitorEngine *);
static void review_reset(MonitorEngine *);
static int review_ingest(MonitorEngine *,const ResourceBatch *,const char *const *,MonitorSnapshot **);
static int review_feedback(MonitorEngine *,int64_t,MonitorFeedback,uint64_t);
static const char *review_error(const MonitorEngine *);
static ResourceStatus review_create(const ResourceCollectorConfig *,ResourceCollector **,uint32_t *);
static void review_destroy(ResourceCollector *);
static ResourceStatus review_collect(ResourceCollector *,ResourceBatch *,ResourceDisplayBatch *,uint32_t *);
static void *review_calloc(size_t,size_t);
static void *review_malloc(size_t);
static void review_snapshot_free(MonitorSnapshot *);
#define monitor_open review_open
#define monitor_close review_close
#define monitor_reset review_reset
#define monitor_ingest review_ingest
#define monitor_feedback review_feedback
#define monitor_error review_error
#define resource_collector_create review_create
#define resource_collector_destroy review_destroy
#define resource_collector_collect_display review_collect
#define calloc review_calloc
#define malloc review_malloc
#define monitor_snapshot_free review_snapshot_free
#define wmain product_wmain_not_called
#include "../../gui/monitor_gui.c"
#undef wmain
#undef monitor_open
#undef monitor_close
#undef monitor_reset
#undef monitor_ingest
#undef monitor_feedback
#undef monitor_error
#undef resource_collector_create
#undef resource_collector_destroy
#undef resource_collector_collect_display
#undef calloc
#undef malloc
#undef monitor_snapshot_free

static unsigned assertions,failures;
#define CHECK(x) do { ++assertions; if(!(x)) { ++failures; printf("FAIL GUI line %u: %s\n",(unsigned)__LINE__,#x); } } while(0)
static HANDLE entered,proceed;
static LONG create_calls,destroy_calls,reset_calls,collect_calls,second_first_sample;
static unsigned fail_next_gui_allocation,snapshot_free_calls;
static unsigned fail_nth_gui_malloc,gui_malloc_index;
static int feedback_mock_failure;
static LONG feedback_calls;
static int64_t feedback_program;
static MonitorFeedback feedback_kind;
static void *review_calloc(size_t count,size_t size)
{
    if(fail_next_gui_allocation) { --fail_next_gui_allocation; return NULL; }
    return calloc(count,size);
}
static void *review_malloc(size_t size)
{
    if(fail_nth_gui_malloc && ++gui_malloc_index==fail_nth_gui_malloc) { fail_nth_gui_malloc=0; return NULL; }
    return malloc(size);
}
static void review_snapshot_free(MonitorSnapshot *snapshot)
{
    if(snapshot) ++snapshot_free_calls;
    monitor_snapshot_free(snapshot);
}
typedef struct ReviewCollector { unsigned samples; } ReviewCollector;
static int review_open(const char *path,int synthetic,MonitorEngine **out,char *error,size_t size)
{ (void)path; (void)synthetic; (void)error; (void)size; *out=(MonitorEngine *)malloc(1); return *out!=NULL; }
static void review_close(MonitorEngine *engine) { free(engine); }
static void review_reset(MonitorEngine *engine) { (void)engine; InterlockedIncrement(&reset_calls); }
static int review_feedback(MonitorEngine *engine,int64_t program,MonitorFeedback kind,uint64_t now)
{
    (void)engine; (void)now; feedback_program=program; feedback_kind=kind;
    InterlockedIncrement(&feedback_calls); return !feedback_mock_failure;
}
static const char *review_error(const MonitorEngine *engine) { (void)engine; return "review injected durable feedback failure"; }
static ResourceStatus review_create(const ResourceCollectorConfig *config,ResourceCollector **out,uint32_t *error)
{ (void)config; (void)error; *out=(ResourceCollector *)calloc(1,sizeof(ReviewCollector)); InterlockedIncrement(&create_calls); return *out?RESOURCE_OK:RESOURCE_OUT_OF_MEMORY; }
static void review_destroy(ResourceCollector *collector) { if(collector) { InterlockedIncrement(&destroy_calls); free(collector); } }
static ResourceStatus review_collect(ResourceCollector *collector,ResourceBatch *out,ResourceDisplayBatch *display,uint32_t *error)
{
    ReviewCollector *state=(ReviewCollector *)collector; LONG call=InterlockedIncrement(&collect_calls); ResourceData *row;
    (void)error; memset(display,0,sizeof(*display)); memset(out,0,sizeof(*out)); out->items=calloc(1,sizeof(*out->items)); if(!out->items) return RESOURCE_OUT_OF_MEMORY;
    row=out->items; out->count=1; out->started_at_utc_100ns=out->finished_at_utc_100ns=monitor_utc_now();
    row->pid=12345; row->creation_time_utc_100ns=11111; row->measured_at_utc_100ns=out->finished_at_utc_100ns;
    row->valid_fields=RESOURCE_VALID_IDENTITY|RESOURCE_VALID_MEMORY; row->cpu_status=RESOURCE_DELTA_FIRST_SAMPLE;
    if(state->samples++) { row->valid_fields|=RESOURCE_VALID_CPU; row->cpu_status=RESOURCE_DELTA_VALID; row->cpu_percent=7; row->cpu_interval_seconds=3; }
    if(call==1) { SetEvent(entered); if(WaitForSingleObject(proceed,5000)!=WAIT_OBJECT_0) return RESOURCE_PLATFORM_ERROR; }
    if(call==2) InterlockedExchange(&second_first_sample,row->cpu_status==RESOURCE_DELTA_FIRST_SAMPLE && !(row->valid_fields&RESOURCE_VALID_CPU));
    return RESOURCE_OK;
}
static int review_ingest(MonitorEngine *engine,const ResourceBatch *batch,const char *const *paths,MonitorSnapshot **out)
{
    MonitorSnapshot *snapshot=calloc(1,sizeof(*snapshot)); (void)engine; (void)paths;
    if(!snapshot) return 0; snapshot->rows=calloc(1,sizeof(*snapshot->rows)); if(!snapshot->rows) { free(snapshot); return 0; }
    snapshot->count=1; snapshot->rows[0].raw=batch->items[0]; snapshot->rows[0].name=_strdup("review.exe");
    snapshot->rows[0].raw.process_name_utf8=snapshot->rows[0].name; snapshot->measured_utc_100ns=batch->finished_at_utc_100ns;
    *out=snapshot; return 1;
}
static void init_app(void)
{
    memset(&app,0,sizeof(app)); InitializeSRWLock(&app.lock); app.session=1; app.interval_ms=1;
    app.membership=pg_membership_create(); app.stop=CreateEventW(NULL,TRUE,FALSE,NULL); app.wake=CreateEventW(NULL,TRUE,FALSE,NULL);
}
static void clear_app(void)
{
    feedback_dispose();
    pg_groups_free(app.groups); app.groups=NULL; pg_membership_destroy(app.membership); app.membership=NULL;
    gui_snapshot_free(app.latest); app.latest=NULL; gui_snapshot_free(app.shown); app.shown=NULL; app.display=NULL;
    if(app.window) DestroyWindow(app.window); app.window=NULL;
    if(app.stop) CloseHandle(app.stop); if(app.wake) CloseHandle(app.wake); if(app.worker) CloseHandle(app.worker);
}
static HWND create_hidden_window(void)
{
    INITCOMMONCONTROLSEX common={sizeof(common),ICC_LISTVIEW_CLASSES}; WNDCLASSW wc; HWND window;
    InitCommonControlsEx(&common); memset(&wc,0,sizeof(wc)); wc.lpfnWndProc=window_proc;
    wc.hInstance=GetModuleHandleW(NULL); wc.lpszClassName=L"IndependentNativeAReviewHidden";
    RegisterClassW(&wc);
    window=CreateWindowW(wc.lpszClassName,L"independent hidden unit fixture",WS_OVERLAPPEDWINDOW,
        0,0,1280,900,NULL,NULL,wc.hInstance,NULL);
    if(window) layout(window); /* An invisible fixture need not receive initial WM_SIZE. */
    return window;
}
static GuiSnapshot *test_view(unsigned session,int reordered)
{
    GuiSnapshot *view=calloc(1,sizeof(*view)); MonitorSnapshot *m=calloc(1,sizeof(*m)); PgProbe probes[4]; size_t i;
    size_t order[4]={0,1,2,3};
    if(reordered) { order[0]=3; order[1]=2; order[2]=0; order[3]=1; }
    if(!view || !m) exit(2); m->rows=calloc(4,sizeof(*m->rows)); if(!m->rows) exit(2); m->count=4; m->measured_utc_100ns=monitor_utc_now();
    memset(probes,0,sizeof(probes));
    for(i=0;i<4;++i) {
        size_t n=order[i]; MonitorRow *r=&m->rows[i]; r->name=_strdup(n==1?"helper.exe":n==3?"unknown":"program.exe"); r->path=_strdup("C:\\fixture.exe");
        r->program_id=(int64_t)n+50; r->raw.pid=(uint32_t)n+200; r->raw.creation_time_utc_100ns=(uint64_t)n+1000;
        r->raw.valid_fields=RESOURCE_VALID_NAME|RESOURCE_VALID_IDENTITY|RESOURCE_VALID_MEMORY|RESOURCE_VALID_CPU; r->raw.process_name_utf8=r->name;
        r->raw.measured_at_utc_100ns=m->measured_utc_100ns; r->raw.working_set_bytes=(uint64_t)n*1048576; r->raw.cpu_percent=(double)n; r->raw.cpu_interval_seconds=3;
        probes[i].grade=PG_PROBE_FAILED;
        if(n==1) { probes[i].grade=PG_CURRENT_INFERRED; probes[i].parent_pid_valid=1; probes[i].parent_pid=200; probes[i].parent_creation=1000; }
        if(n==3) r->raw.valid_fields&=~RESOURCE_VALID_IDENTITY;
    }
    if(!pg_resolve(NULL,m,probes,&view->relations)) exit(2); view->metrics=m; view->session=session; return view;
}
static void select_item(int index)
{
    ListView_SetItemState(app.list,-1,0,LVIS_SELECTED|LVIS_FOCUSED); ListView_SetItemState(app.list,index,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED); show_detail();
}
static int index_for_pid(uint32_t pid)
{
    int i; for(i=0;i<ListView_GetItemCount(app.list);++i) { int source=source_row_at(i); if(source>=0 && app.display->rows[source].raw.pid==pid) return i; } return -1;
}
static void assert_source_projection(void)
{
    int i; unsigned seen[4]={0,0,0,0},headers=0;
    for(i=0;i<ListView_GetItemCount(app.list);++i) { int source=source_row_at(i); if(source<0) { CHECK(group_at(i)!=PG_NONE); ++headers; } else { CHECK(source<4); if(source<4) ++seen[source]; } }
    for(i=0;i<4;++i) CHECK(seen[i]==1); CHECK(headers==1);
}
static void test_ui_mapping(void)
{
    INITCOMMONCONTROLSEX common={sizeof(common),ICC_LISTVIEW_CLASSES}; WNDCLASSW wc; HWND window; unsigned queued; uint64_t target;
    init_app(); InitCommonControlsEx(&common); memset(&wc,0,sizeof(wc)); wc.lpfnWndProc=window_proc; wc.hInstance=GetModuleHandleW(NULL); wc.lpszClassName=L"IndependentGroupingReviewHidden";
    RegisterClassW(&wc); window=CreateWindowW(wc.lpszClassName,L"review",WS_OVERLAPPEDWINDOW,0,0,1200,900,NULL,NULL,wc.hInstance,NULL); CHECK(window!=NULL); if(!window) { clear_app(); return; }
    app.latest=test_view(1,0); consume_snapshot(); CHECK(ListView_GetItemCount(app.list)==3); CHECK(source_row_at(0)<0);
    select_item(0); CHECK(selected_row()<0); CHECK(!IsWindowEnabled(GetDlgItem(window,ID_IGNORE))); queued=app.command_count;
    SendMessageW(window,WM_COMMAND,ID_IGNORE,0); CHECK(app.command_count==queued);
    {
        NMLISTVIEW click; NMLVKEYDOWN key; memset(&click,0,sizeof(click)); click.hdr.idFrom=ID_LIST; click.iItem=0; click.iSubItem=0; click.ptAction.x=10;
        click.hdr.code=NM_CLICK; SendMessageW(window,WM_NOTIFY,0,(LPARAM)&click); CHECK(ListView_GetItemCount(app.list)==5);
        click.hdr.code=NM_DBLCLK; SendMessageW(window,WM_NOTIFY,0,(LPARAM)&click); CHECK(ListView_GetItemCount(app.list)==5);
        click.ptAction.x=100; SendMessageW(window,WM_NOTIFY,0,(LPARAM)&click); CHECK(ListView_GetItemCount(app.list)==3);
        memset(&key,0,sizeof(key)); key.hdr.idFrom=ID_LIST; key.hdr.code=LVN_KEYDOWN; key.wVKey=VK_RIGHT;
        SendMessageW(window,WM_NOTIFY,0,(LPARAM)&key); CHECK(ListView_GetItemCount(app.list)==5);
        key.wVKey=VK_LEFT; SendMessageW(window,WM_NOTIFY,0,(LPARAM)&key); CHECK(ListView_GetItemCount(app.list)==3);
    }
    SendMessageW(window,WM_COMMAND,ID_EXPAND,0); CHECK(ListView_GetItemCount(app.list)==5); assert_source_projection();
    select_item(index_for_pid(201)); CHECK(selected_row()==1); CHECK(IsWindowEnabled(GetDlgItem(window,ID_EXPECTED)));
    SendMessageW(window,WM_COMMAND,ID_EXPECTED,0); CHECK(app.command_count==queued+1); CHECK(app.commands[queued].program==51);
    SendMessageW(window,WM_COMMAND,ID_COLLAPSE,0); CHECK(selected_row()<0); CHECK(!IsWindowEnabled(GetDlgItem(window,ID_EXPECTED)));
    select_item(0); SendMessageW(window,WM_COMMAND,ID_TARGET,0); target=pg_target_id(app.membership); CHECK(target!=0);
    SendMessageW(window,WM_COMMAND,ID_EXPAND,0); select_item(index_for_pid(201));
    app.latest=test_view(1,1); consume_snapshot(); CHECK(selected_row()>=0); if(selected_row()>=0) CHECK(app.display->rows[selected_row()].raw.pid==201);
    CHECK(pg_target_id(app.membership)==target); CHECK(app.groups->groups[app.groups->owner[3]].key.kind==PG_OWNER_USER); assert_source_projection();
    select_item(-1); app.latest=test_view(1,1); consume_snapshot(); CHECK(selected_item()<0);
    SendMessageW(window,WM_COMMAND,ID_PAUSE,0); CHECK(app.pause_requested); CHECK(pg_target_id(app.membership)==target);
    SendMessageW(window,WM_COMMAND,ID_PAUSE,0); CHECK(!app.pause_requested); CHECK(app.session==2); CHECK(pg_target_id(app.membership)==0); CHECK(current_relations()==NULL); CHECK(app.groups==NULL);
    app.latest=test_view(1,0); consume_snapshot(); CHECK(current_relations()==NULL); CHECK(app.groups==NULL); CHECK(!IsWindowEnabled(GetDlgItem(window,ID_CONFIRM)));
    app.latest=test_view(2,0); consume_snapshot(); CHECK(current_relations()!=NULL); CHECK(app.groups!=NULL); CHECK(pg_target_id(app.membership)==0);
    SendMessageW(window,WM_COMMAND,ID_EXPAND,0); select_item(index_for_pid(201));
    {
        GuiSnapshot *duplicate=test_view(2,0); PgProbe probes[4]; memset(probes,0,sizeof(probes));
        duplicate->metrics->rows[2].raw.pid=201;
        pg_snapshot_free(duplicate->relations); duplicate->relations=NULL;
        CHECK(pg_resolve(NULL,duplicate->metrics,probes,&duplicate->relations));
        app.latest=duplicate; consume_snapshot(); CHECK(selected_item()<0);
    }
    clear_app();
}
static void test_rapid_pause_resume(void)
{
    init_app(); app.no_grouping=1; app.count=2; snprintf(app.db,sizeof(app.db),"review-mocked-no-db");
    entered=CreateEventW(NULL,TRUE,FALSE,NULL); proceed=CreateEventW(NULL,TRUE,FALSE,NULL);
    create_calls=destroy_calls=reset_calls=collect_calls=second_first_sample=0;
    app.worker=CreateThread(NULL,0,worker_main,NULL,0,NULL); CHECK(app.worker!=NULL);
    CHECK(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    /* The worker cannot observe pause_requested=1 because its collection is
     * held here. The new epoch must independently reset it after release. */
    window_proc(NULL,WM_COMMAND,ID_PAUSE,0); window_proc(NULL,WM_COMMAND,ID_PAUSE,0); CHECK(app.session==2); CHECK(!app.pause_requested);
    SetEvent(proceed); CHECK(WaitForSingleObject(app.worker,5000)==WAIT_OBJECT_0);
    CHECK(collect_calls==2); CHECK(create_calls>=2); CHECK(reset_calls>=1); CHECK(second_first_sample==1);
    CHECK(app.latest!=NULL); if(app.latest) CHECK(app.latest->session==2);
    CloseHandle(entered); CloseHandle(proceed); clear_app();
}
static GuiSnapshot *long_view(int reverse)
{
    GuiSnapshot *view=calloc(1,sizeof(*view)); MonitorSnapshot *snapshot=calloc(1,sizeof(*snapshot));
    PgProbe probes[96]; size_t i; unsigned m;
    if(!view || !snapshot) exit(2);
    snapshot->rows=calloc(96,sizeof(*snapshot->rows)); if(!snapshot->rows) exit(2);
    snapshot->count=96; snapshot->measured_utc_100ns=monitor_utc_now(); view->metrics=snapshot; view->session=1;
    memset(probes,0,sizeof(probes));
    for(i=0;i<snapshot->count;++i) {
        size_t n=reverse?95-i:i; MonitorRow *row=&snapshot->rows[i]; char text[160];
        snprintf(text,sizeof(text),"%s %02zu.exe",n%2?"한글 도구":"English program",n); row->name=_strdup(text);
        snprintf(text,sizeof(text),"C:\\independent-review\\%02zu\\same.exe",n); row->path=_strdup(text);
        row->program_id=10000+(int64_t)n; row->raw.pid=10000+(uint32_t)n; row->raw.creation_time_utc_100ns=100000+n;
        row->raw.process_name_utf8=row->name; row->raw.measured_at_utc_100ns=snapshot->measured_utc_100ns;
        row->raw.valid_fields=RESOURCE_VALID_IDENTITY|RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE;
        row->raw.cpu_percent=(double)n; row->raw.working_set_bytes=(uint64_t)n*UINT64_C(100000000);
        row->raw.cpu_interval_seconds=row->raw.io_interval_seconds=3;
        for(m=0;m<4;++m) { row->metric[m].state=MONITOR_LEARNING; row->metric[m].current=row->metric[m].median=row->metric[m].mad=row->metric[m].threshold=NAN; }
        probes[i].grade=PG_PROBE_FAILED;
    }
    if(!pg_resolve(NULL,snapshot,probes,&view->relations)) exit(2);
    return view;
}
static void test_identity_scroll_restore(void)
{
    int top,source; uint32_t top_pid; RECT bounds;
    init_app(); app.no_grouping=1; CHECK(create_hidden_window()!=NULL);
    app.latest=long_view(0); consume_snapshot(); CHECK(ListView_GetItemCount(app.list)==96);
    select_item(index_for_pid(10035));
    CHECK(ListView_GetItemRect(app.list,0,&bounds,LVIR_BOUNDS));
    ListView_Scroll(app.list,0,25*(bounds.bottom-bounds.top));
    top=ListView_GetTopIndex(app.list); source=source_row_at(top); CHECK(source>=0);
    top_pid=source>=0?app.display->rows[source].raw.pid:0;
    printf("scroll_before top_index=%d top_pid=%lu selected_index=%d row_height=%ld page=%d\n",top,(unsigned long)top_pid,selected_item(),bounds.bottom-bounds.top,ListView_GetCountPerPage(app.list));
    CHECK(top_pid>=10020 && top_pid<=10080);
    app.latest=long_view(1); consume_snapshot(); source=selected_row(); CHECK(source>=0);
    if(source>=0) CHECK(app.display->rows[source].raw.pid==10035);
    source=source_row_at(ListView_GetTopIndex(app.list)); CHECK(source>=0);
    printf("scroll_after top_index=%d top_pid=%lu selected_index=%d stored_top_pid=%lu\n",ListView_GetTopIndex(app.list),(unsigned long)(source>=0?app.display->rows[source].raw.pid:0),selected_item(),(unsigned long)app.top_key.pid);
    if(source>=0) CHECK(app.display->rows[source].raw.pid==top_pid);
    ListView_Scroll(app.list,0,15*(bounds.bottom-bounds.top));
    printf("scroll_direct top_index=%d\n",ListView_GetTopIndex(app.list));
    select_item(-1); app.latest=long_view(0); consume_snapshot(); CHECK(selected_item()<0);
    clear_app();
}
static void test_utf8_and_feedback_ack(void)
{
    wchar_t *wide=to_wide("한글 프로그램 English 🚀.exe");
    MonitorRow first, same_path, other_path;
    uint64_t expires,now=monitor_utc_now();
    CHECK(wide!=NULL);
    if(wide) { CHECK(!wcscmp(wide,L"한글 프로그램 English 🚀.exe")); free(wide); }
    init_app(); memset(&first,0,sizeof(first)); first.program_id=4001; first.path="C:\\alpha\\same.exe";
    same_path=first; other_path=first; other_path.program_id=4002; other_path.path="C:\\beta\\same.exe";
    CHECK(displayed_feedback(&first,&expires)==MONITOR_FEEDBACK_CANCEL); CHECK(expires==0);
    /* Queuing a command is not a durable acknowledgement. */
    CHECK(feedback_submit(4001,MONITOR_FEEDBACK_EXPECTED)==FEEDBACK_ACCEPTED);
    CHECK(displayed_feedback(&first,&expires)==MONITOR_FEEDBACK_CANCEL);
    app.feedback_overlay->kind=MONITOR_FEEDBACK_EXPECTED;
    app.feedback_overlay->expires=now+UINT64_C(864000000000); app.feedback_overlay->revision=1;
    CHECK(displayed_feedback(&first,&expires)==MONITOR_FEEDBACK_EXPECTED);
    CHECK(expires==app.feedback_overlay->expires);
    CHECK(displayed_feedback(&same_path,&expires)==MONITOR_FEEDBACK_EXPECTED);
    CHECK(displayed_feedback(&other_path,&expires)==MONITOR_FEEDBACK_CANCEL);
    app.feedback_overlay->expires=now-1;
    CHECK(displayed_feedback(&first,&expires)==MONITOR_FEEDBACK_CANCEL);
    app.feedback_overlay->kind=MONITOR_FEEDBACK_CANCEL; app.feedback_overlay->expires=0;
    CHECK(displayed_feedback(&first,&expires)==MONITOR_FEEDBACK_CANCEL);
    CHECK(expires==0); clear_app();
}
static void test_worker_feedback_commit_ack(void)
{
    int failure;
    for(failure=0;failure<2;++failure) {
        init_app(); app.no_grouping=1; app.count=1; feedback_mock_failure=failure;
        collect_calls=2; feedback_calls=0; feedback_program=0;
        CHECK(feedback_submit(7777,MONITOR_FEEDBACK_IGNORE)==FEEDBACK_ACCEPTED);
        app.worker=CreateThread(NULL,0,worker_main,NULL,0,NULL); CHECK(app.worker!=NULL);
        CHECK(WaitForSingleObject(app.worker,5000)==WAIT_OBJECT_0);
        CHECK(feedback_calls==1); CHECK(feedback_program==7777); CHECK(feedback_kind==MONITOR_FEEDBACK_IGNORE);
        CHECK(app.command_count==0);
        CHECK(app.feedback_result_count==1); CHECK(app.feedback_overlay_count==1);
        CHECK(app.feedback_outstanding==1);
        CHECK(app.feedback_results[0].outcome==(failure?FEEDBACK_FAILED:FEEDBACK_SUCCEEDED));
        CHECK(app.worker_error==failure);
        CHECK(app.latest!=NULL);
        if(app.latest) {
            if(failure) {
                CHECK(app.latest->metrics->storage_error==1);
                CHECK(strstr(app.latest->metrics->status,"durable feedback failure")!=NULL);
            } else {
                CHECK(app.feedback_results[0].program==7777);
                CHECK(app.feedback_results[0].expires>monitor_utc_now());
            }
        }
        clear_app();
    }
    feedback_mock_failure=0;
}
static void test_snapshot_ownership_and_errors(void)
{
    unsigned before=snapshot_free_calls;
    GuiSnapshot *one,*two; MonitorSnapshot *error;
    init_app(); CHECK(create_hidden_window()!=NULL);
    one=test_view(1,0); publish_view(one); CHECK(app.latest==one); CHECK(snapshot_free_calls==before);
    two=test_view(1,1); publish_view(two); CHECK(app.latest==two); CHECK(snapshot_free_calls==before+1);
    consume_snapshot(); CHECK(app.latest==NULL); CHECK(app.shown==two); CHECK(app.display==two->metrics);
    one=test_view(1,0); publish_view(one); consume_snapshot(); CHECK(snapshot_free_calls==before+2);
    CHECK(app.shown==one); CHECK(app.display==one->metrics);
    error=error_snapshot("raw diagnostic: 저장 실패 Ω",1); CHECK(error!=NULL);
    if(error) { CHECK(error->storage_error==1); CHECK(error->collector_error==0); CHECK(!strcmp(error->status,"raw diagnostic: 저장 실패 Ω")); }
    publish(error); consume_snapshot(); CHECK(snapshot_free_calls==before+3);
    CHECK(app.display!=NULL && app.display->storage_error==1);
    CHECK(ListView_GetItemCount(app.list)==0);
    error=error_snapshot("collector failed",0); CHECK(error!=NULL);
    if(error) { CHECK(error->storage_error==0); CHECK(error->collector_error==1); }
    /* The wrapper allocation failure must release the already-owned snapshot. */
    fail_next_gui_allocation=1; publish(error);
    CHECK(app.worker_error==1); CHECK(app.latest==NULL); CHECK(snapshot_free_calls==before+4);
    app.worker_error=0; fail_next_gui_allocation=1; error=error_snapshot("allocation failure",1);
    CHECK(error==NULL); publish(error); CHECK(app.worker_error==1);
    clear_app(); CHECK(snapshot_free_calls==before+5);
}
static int control_inside(HWND window,HWND child)
{
    RECT client,bounds;
    if(!GetClientRect(window,&client) || !GetWindowRect(child,&bounds)) return 0;
    MapWindowPoints(HWND_DESKTOP,window,(POINT *)&bounds,2);
    return bounds.left>=0 && bounds.top>=0 && bounds.right>bounds.left && bounds.bottom>bounds.top &&
        bounds.right<=client.right && bounds.bottom<=client.bottom;
}
static void test_hidden_layout_and_handles(void)
{
    DWORD before_user,before_gdi,after_user,after_gdi,stable_user=0,stable_gdi=0;
    unsigned iteration,scale;
    init_app(); CHECK(create_hidden_window()!=NULL);
    /* Warm every tested font size before counting process-local GDI caches. */
    for(scale=100;scale<=200;scale+=25) { app.dpi=(UINT)MulDiv(96,(int)scale,100); ui_fonts(); }
    clear_app();
    before_user=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    before_gdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(iteration=0;iteration<12;++iteration) {
        RECT outer; HWND window;
        init_app(); app.ui_scale=100; window=create_hidden_window(); CHECK(window!=NULL);
        if(!window) { clear_app(); continue; }
        for(scale=100;scale<=200;scale+=25) {
            int column; RECT row;
            app.ui_scale=scale; app.dpi=(UINT)MulDiv(96,(int)scale,100); ui_fonts();
            outer=(RECT){0,0,ui_px(960),ui_px(640)};
            AdjustWindowRectEx(&outer,WS_OVERLAPPEDWINDOW,FALSE,0);
            SetWindowPos(window,NULL,0,0,outer.right-outer.left,outer.bottom-outer.top,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);
            layout(window);
            CHECK(control_inside(window,app.list)); CHECK(control_inside(window,app.detail));
            CHECK(control_inside(window,app.pause)); CHECK(control_inside(window,app.status));
            CHECK(ListView_GetColumnWidth(app.list,0)>=ui_px(140));
            for(column=1;column<6;++column) CHECK(ListView_GetColumnWidth(app.list,column)>=ui_px(60));
            CHECK(app.body_font!=NULL); CHECK(app.heading_font!=NULL); CHECK(app.title_font!=NULL); CHECK(app.row_images!=NULL);
            app.latest=test_view(1,0); consume_snapshot();
            CHECK(ListView_GetItemRect(app.list,0,&row,LVIR_BOUNDS));
            CHECK(row.bottom-row.top>=ui_px(30));
        }
        clear_app(); CHECK(app.body_font==NULL); CHECK(app.row_images==NULL);
        if(iteration==0) { stable_user=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS); stable_gdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS); }
        printf("hidden_handle_iteration=%u user=%lu gdi=%lu\n",iteration,(unsigned long)GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS),(unsigned long)GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS));
    }
    after_user=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    after_gdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    printf("hidden_handle_cycle user_before=%lu user_after=%lu gdi_before=%lu gdi_after=%lu iterations=12\n",
        (unsigned long)before_user,(unsigned long)after_user,(unsigned long)before_gdi,(unsigned long)after_gdi);
    printf("hidden_handle_stable user_baseline=%lu gdi_baseline=%lu repeated_cycles=11\n",(unsigned long)stable_user,(unsigned long)stable_gdi);
    CHECK(after_user<=stable_user+1); CHECK(after_gdi<=stable_gdi+1);
}
#ifdef ID_SEARCH
static void search_for(const wchar_t *text)
{
    SetDlgItemTextW(app.window,ID_SEARCH,text);
    SendMessageW(app.window,WM_COMMAND,MAKEWPARAM(ID_SEARCH,EN_CHANGE),(LPARAM)GetDlgItem(app.window,ID_SEARCH));
}
static void sort_by(int mode)
{
    SendDlgItemMessageW(app.window,ID_SORT,CB_SETCURSEL,(WPARAM)mode,0);
    SendMessageW(app.window,WM_COMMAND,MAKEWPARAM(ID_SORT,CBN_SELCHANGE),(LPARAM)GetDlgItem(app.window,ID_SORT));
}
static void detail_text(wchar_t *buffer,size_t count)
{
    GetWindowTextW(app.detail,buffer,(int)count);
}
static void status_text(wchar_t *buffer,size_t count)
{
    GetWindowTextW(app.status,buffer,(int)count);
}
static uint32_t pid_at(int visible)
{
    int source=source_row_at(visible); return source>=0?app.display->rows[source].raw.pid:0;
}
static void test_decimal_values(void)
{
    unsigned metric; char value[128]; PgSum sum;
    for(metric=0;metric<4;++metric) {
        double scale=metric==MONITOR_CPU?1.0:metric==MONITOR_RAM?1e9:1e6;
        CHECK(!strcmp(ui_value(value,sizeof(value),1,0,metric),"0.00"));
        CHECK(!strcmp(ui_value(value,sizeof(value),1,0.001*scale,metric),"<0.01"));
        CHECK(!strcmp(ui_value(value,sizeof(value),1,2.5*scale,metric),"2.50"));
        CHECK(!strcmp(ui_value(value,sizeof(value),0,0,metric),"NA"));
        CHECK(!strcmp(ui_value(value,sizeof(value),1,NAN,metric),"NA"));
    }
    memset(&sum,0,sizeof(sum)); sum.valid=2; sum.total=3; sum.bytes=UINT64_C(2500000000);
    sum.value=(double)sum.bytes/1048576.0;
    CHECK(!strcmp(sum_text(&sum,value,sizeof(value),MONITOR_RAM),"2.50 *"));
    sum.value=2500000; CHECK(!strcmp(sum_text(&sum,value,sizeof(value),MONITOR_READ),"2.50 *"));
    sum.value=0; sum.valid=sum.total=2; CHECK(!strcmp(sum_text(&sum,value,sizeof(value),MONITOR_CPU),"0.00"));
    sum.value=0.001; CHECK(!strcmp(sum_text(&sum,value,sizeof(value),MONITOR_CPU),"<0.01"));
    sum.valid=0; CHECK(strstr(sum_text(&sum,value,sizeof(value),MONITOR_CPU),"NA")!=NULL);
    sum.overflow=1; CHECK(strstr(sum_text(&sum,value,sizeof(value),MONITOR_CPU),"오류")!=NULL);
}
static void test_search_sort_projection(void)
{
    size_t group; int selected,mode; PgSum original[4]; wchar_t text[2048];
    init_app(); CHECK(create_hidden_window()!=NULL); app.latest=test_view(1,0); consume_snapshot();
    group=group_at(0); CHECK(group!=PG_NONE); if(group==PG_NONE) { clear_app(); return; }
    memcpy(original,app.groups->groups[group].metric,sizeof(original));
    select_item(0); search_for(L"helper");
    CHECK(ListView_GetItemCount(app.list)==1); CHECK(source_row_at(0)<0); CHECK(group_at(0)==group);
    CHECK(!memcmp(original,app.groups->groups[group].metric,sizeof(original)));
    CHECK(app.groups->groups[group].count==2); CHECK(selected_row()<0);
    CHECK(!IsWindowEnabled(GetDlgItem(app.window,ID_IGNORE)));
    SendMessageW(app.window,WM_COMMAND,ID_EXPAND,0);
    CHECK(ListView_GetItemCount(app.list)==2); CHECK(source_row_at(0)<0); CHECK(pid_at(1)==201);
    CHECK(!memcmp(original,app.groups->groups[group].metric,sizeof(original)));
    select_item(1); CHECK(group_at(1)==group); CHECK(selected_row()==1); /* group membership is not a header token */
    CHECK(IsWindowEnabled(GetDlgItem(app.window,ID_IGNORE)));
    search_for(L"no such process"); CHECK(ListView_GetItemCount(app.list)==0); CHECK(selected_item()<0);
    search_for(L""); CHECK(selected_item()<0); CHECK(ListView_GetItemCount(app.list)==5);
    search_for(L"201"); CHECK(ListView_GetItemCount(app.list)==2); CHECK(pid_at(1)==201);
    search_for(L"FIXTURE.EXE"); CHECK(ListView_GetItemCount(app.list)==5);
    search_for(L""); SendMessageW(app.window,WM_COMMAND,ID_FLAT,0); CHECK(ListView_GetItemCount(app.list)==4);
    app.display->rows[0].raw.cpu_percent=0; app.display->rows[1].raw.cpu_percent=11; app.display->rows[2].raw.cpu_percent=2;
    app.display->rows[0].raw.working_set_bytes=0; app.display->rows[1].raw.working_set_bytes=UINT64_C(11000000000); app.display->rows[2].raw.working_set_bytes=UINT64_C(2000000000);
    app.display->rows[0].raw.io_read_bytes_per_second=app.display->rows[0].raw.io_write_bytes_per_second=0;
    app.display->rows[1].raw.io_read_bytes_per_second=app.display->rows[1].raw.io_write_bytes_per_second=11000000;
    app.display->rows[2].raw.io_read_bytes_per_second=app.display->rows[2].raw.io_write_bytes_per_second=2000000;
    for(mode=0;mode<3;++mode) app.display->rows[mode].raw.valid_fields|=RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE;
    app.display->rows[3].raw.valid_fields&=~(RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE);
    select_item(index_for_pid(201));
    for(mode=3;mode<=10;++mode) {
        sort_by(mode); CHECK(ListView_GetItemCount(app.list)==4); CHECK(pid_at(3)==203);
        CHECK(pid_at(0)==(mode%2?201u:200u)); CHECK(pid_at(1)==202); CHECK(pid_at(2)==(mode%2?200u:201u));
        selected=selected_row(); CHECK(selected>=0); if(selected>=0) CHECK(app.display->rows[selected].raw.pid==201);
    }
    CHECK(app.command_count==0); SendMessageW(app.window,WM_COMMAND,ID_EXPECTED,0); CHECK(app.command_count==1); CHECK(app.commands[0].program==51);
    app.flat=0; rebuild_groups(); sort_by(3); CHECK(source_row_at(0)<0); CHECK(group_at(0)!=PG_NONE);
    select_item(0); CHECK(selected_row()<0); CHECK(!IsWindowEnabled(GetDlgItem(app.window,ID_EXPECTED)));
    SendMessageW(app.window,WM_COMMAND,ID_EXPECTED,0); CHECK(app.command_count==1);
    app.ui_demo=1; select_item(index_for_pid(201)); update_actions(); CHECK(!IsWindowEnabled(GetDlgItem(app.window,ID_IGNORE)));
    SendMessageW(app.window,WM_COMMAND,ID_IGNORE,0); CHECK(app.command_count==1);
    status_text(text,sizeof(text)/sizeof(text[0])); clear_app();
}
static void test_completed_report_rendering(void)
{
    MonitorRow *row; MonitorMetricReport before; wchar_t text[16384]; uint64_t now=monitor_utc_now(); unsigned m;
    init_app(); CHECK(create_hidden_window()!=NULL); app.latest=test_view(1,0); consume_snapshot();
    SendMessageW(app.window,WM_COMMAND,ID_EXPAND,0); select_item(index_for_pid(201)); row=&app.display->rows[selected_row()];
    free(row->name); row->name=_strdup("한글 근거 English 🚀.exe"); row->raw.process_name_utf8=row->name;
    row->raw.cpu_percent=7; row->raw.working_set_bytes=UINT64_C(3500000000); row->raw.io_read_bytes_per_second=3000000; row->raw.io_write_bytes_per_second=4000000;
    row->raw.valid_fields|=RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE;
    for(m=0;m<4;++m) {
        MonitorMetricReport *r=&row->metric[m]; double scale=m==MONITOR_CPU?1.0:m==MONITOR_RAM?1e9:1e6;
        memset(r,0,sizeof(*r)); r->state=MONITOR_NORMAL; r->current=2.5*scale; r->median=1.0*scale; r->mad=0.13*scale; r->threshold=3.0*scale;
        r->count=64; r->days=3; r->minutes=61; r->covered_seconds=60.2; r->valid_samples=20; r->baseline_eligible=1;
        r->window_start_utc_100ns=now-UINT64_C(700000000); r->window_end_utc_100ns=now-UINT64_C(98000000); r->window_duration_seconds=60.2;
        snprintf(r->reason,sizeof(r->reason),"within threshold"); app.detail_metric=(int)m; before=*r; show_detail(); detail_text(text,sizeof(text)/sizeof(text[0]));
        CHECK(wcsstr(text,L"한글 근거 English 🚀.exe")!=NULL); CHECK(wcsstr(text,L"현재 사용량 · 최근 표본")!=NULL);
        CHECK(wcsstr(text,L"RAM 3.50 GB")!=NULL); CHECK(wcsstr(text,L"완료창 값  2.50")!=NULL);
        CHECK(wcsstr(text,L"과거 중앙값  1.00")!=NULL); CHECK(wcsstr(text,L"과거 MAD  0.13")!=NULL);
        CHECK(wcsstr(text,L"평소 상한  3.00")!=NULL); CHECK(wcsstr(text,L"완료창 시작")!=NULL); CHECK(wcsstr(text,L"완료창 끝")!=NULL);
        CHECK(!memcmp(&before,r,sizeof(before)));
    }
    app.detail_metric=MONITOR_RAM; row->raw.valid_fields&=~RESOURCE_VALID_MEMORY; row->metric[MONITOR_RAM].state=MONITOR_UNAVAILABLE;
    row->metric[MONITOR_RAM].streak=0; snprintf(row->metric[MONITOR_RAM].reason,sizeof(row->metric[MONITOR_RAM].reason),"latest metric missing; persistence reset");
    before=row->metric[MONITOR_RAM]; show_detail(); detail_text(text,sizeof(text)/sizeof(text[0]));
    CHECK(wcsstr(text,L"RAM NA GB")!=NULL); CHECK(wcsstr(text,L"현재 비교 상태: 비교 불가")!=NULL);
    CHECK(wcsstr(text,L"완료창 값  2.50 GB")!=NULL); CHECK(wcsstr(text,L"현재 판정이 아닙니다")!=NULL);
    CHECK(!memcmp(&before,&row->metric[MONITOR_RAM],sizeof(before)));
    row->metric[MONITOR_RAM].state=MONITOR_LEARNING; row->metric[MONITOR_RAM].count=20; row->metric[MONITOR_RAM].days=2; row->metric[MONITOR_RAM].minutes=20;
    snprintf(row->metric[MONITOR_RAM].reason,sizeof(row->metric[MONITOR_RAM].reason),"history gate not met"); show_detail(); detail_text(text,sizeof(text)/sizeof(text[0]));
    CHECK(wcsstr(text,L"학습 중 상한 계산값")!=NULL); CHECK(wcsstr(text,L"비교 기준 미확정")!=NULL);
    row->metric[MONITOR_RAM].window_end_utc_100ns=0; row->metric[MONITOR_RAM].window_start_utc_100ns=0;
    row->metric[MONITOR_RAM].current=row->metric[MONITOR_RAM].median=row->metric[MONITOR_RAM].mad=row->metric[MONITOR_RAM].threshold=NAN;
    show_detail(); detail_text(text,sizeof(text)/sizeof(text[0])); CHECK(wcsstr(text,L"완료창 없음")!=NULL); CHECK(wcsstr(text,L"완료창 값")==NULL);
    clear_app();
}
static void test_name_width_and_lifecycle_status(void)
{
    NMHEADERW event; HDITEMW item; wchar_t text[4096];
    init_app(); CHECK(create_hidden_window()!=NULL); app.latest=test_view(1,0); consume_snapshot();
    app.name_width_dip=340; layout(app.window); CHECK(ListView_GetColumnWidth(app.list,0)==ui_px(340));
    app.dpi=144; ui_fonts(); layout(app.window); CHECK(ListView_GetColumnWidth(app.list,0)==510);
    memset(&event,0,sizeof(event)); memset(&item,0,sizeof(item)); event.hdr.hwndFrom=ListView_GetHeader(app.list); event.hdr.code=HDN_ENDTRACKW;
    event.iItem=0; event.pitem=&item; item.mask=HDI_WIDTH; item.cxy=ui_px(276);
    SendMessageW(app.window,WM_NOTIFY,0,(LPARAM)&event); CHECK(app.name_width_dip==276); layout(app.window); CHECK(ListView_GetColumnWidth(app.list,0)==ui_px(276));
    app.dpi=96; ui_fonts(); layout(app.window); CHECK(ListView_GetColumnWidth(app.list,0)==276);
    app.worker_error=1; SendMessageW(app.window,WM_WORKER_DONE,0,0); search_for(L"program"); status_text(text,sizeof(text)/sizeof(text[0]));
    CHECK(wcsstr(text,L"오류")!=NULL || wcsstr(text,L"중단")!=NULL); CHECK(wcsstr(text,L"로컬 수집 중")==NULL);
    app.display->measured_utc_100ns=monitor_utc_now()-UINT64_C(1000000000);
    SendMessageW(app.window,WM_TIMER,1,0); status_text(text,sizeof(text)/sizeof(text[0])); CHECK(wcsstr(text,L"중단")!=NULL);
    app.worker_error=0; app.stopping=1; render_list(); status_text(text,sizeof(text)/sizeof(text[0])); CHECK(wcsstr(text,L"중단")!=NULL);
    SendMessageW(app.window,WM_TIMER,1,0); status_text(text,sizeof(text)/sizeof(text[0])); CHECK(wcsstr(text,L"중단")!=NULL);
    app.stopping=0; app.pause_requested=1; render_list(); status_text(text,sizeof(text)/sizeof(text[0])); CHECK(wcsstr(text,L"일시정지")!=NULL);
    app.pause_requested=0; app.display->collector_error=1; render_list(); status_text(text,sizeof(text)/sizeof(text[0])); CHECK(wcsstr(text,L"수집 오류")!=NULL);
    app.display->collector_error=0; app.display->storage_error=1; render_list(); status_text(text,sizeof(text)/sizeof(text[0])); CHECK(wcsstr(text,L"저장 오류")!=NULL);
    app.display->storage_error=0; app.ui_demo=1; app.display->measured_utc_100ns=monitor_utc_now()-UINT64_C(1000000000);
    render_list(); SendMessageW(app.window,WM_TIMER,1,0); status_text(text,sizeof(text)/sizeof(text[0])); CHECK(wcsstr(text,L"합성")!=NULL);
    clear_app();
}
static void test_sort_allocation_fallback(void)
{
    wchar_t text[2048];
    init_app(); CHECK(create_hidden_window()!=NULL); app.latest=test_view(1,0); consume_snapshot();
    SendMessageW(app.window,WM_COMMAND,ID_EXPAND,0); app.sort_mode=4;
    gui_malloc_index=0; fail_nth_gui_malloc=2; render_list(); CHECK(fail_nth_gui_malloc==0);
    CHECK(ListView_GetItemCount(app.list)==5); assert_source_projection(); status_text(text,sizeof(text)/sizeof(text[0]));
    CHECK(wcsstr(text,L"정렬")!=NULL && wcsstr(text,L"부족")!=NULL);
    clear_app();
}
#endif
#include "availability_cases.h"
int main(void)
{
    test_access_metadata_binding(); test_access_display_and_causes();
    test_ui_mapping(); test_rapid_pause_resume(); test_identity_scroll_restore(); test_utf8_and_feedback_ack(); test_worker_feedback_commit_ack(); test_snapshot_ownership_and_errors();
    test_hidden_layout_and_handles();
#ifdef ID_SEARCH
    test_decimal_values(); test_search_sort_projection(); test_completed_report_rendering(); test_name_width_and_lifecycle_status(); test_sort_allocation_fallback();
#endif
    printf("independent_native_ui_review assertions=%u failures=%u\n",assertions,failures); return failures?1:0;
}
