/* Includes the real GUI implementation to exercise its private row decoder and
 * worker state machine. Only collector/engine ingress is replaced for the
 * deterministic in-flight Pause/Resume barrier; no product source is edited. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "monitor.h"
#include "resource_display.h"
static int review_open(const char *,int,MonitorEngine **,char *,size_t);
static void review_close(MonitorEngine *);
static void review_reset(MonitorEngine *);
static int review_ingest(MonitorEngine *,const ResourceBatch *,const char *const *,MonitorSnapshot **);
static ResourceStatus review_create(const ResourceCollectorConfig *,ResourceCollector **,uint32_t *);
static void review_destroy(ResourceCollector *);
static ResourceStatus review_collect(ResourceCollector *,ResourceBatch *,ResourceDisplayBatch *,uint32_t *);
#define monitor_open review_open
#define monitor_close review_close
#define monitor_reset review_reset
#define monitor_ingest review_ingest
#define resource_collector_create review_create
#define resource_collector_destroy review_destroy
#define resource_collector_collect_display review_collect
#define wmain product_wmain_not_called
#include "../../gui/monitor_gui.c"
#undef wmain
#undef monitor_open
#undef monitor_close
#undef monitor_reset
#undef monitor_ingest
#undef resource_collector_create
#undef resource_collector_destroy
#undef resource_collector_collect_display

static unsigned assertions,failures;
#define CHECK(x) do { ++assertions; if(!(x)) { ++failures; printf("FAIL GUI line %u: %s\n",(unsigned)__LINE__,#x); } } while(0)
static HANDLE entered,proceed;
static LONG create_calls,destroy_calls,reset_calls,collect_calls,second_first_sample;
typedef struct ReviewCollector { unsigned samples; } ReviewCollector;
static int review_open(const char *path,int synthetic,MonitorEngine **out,char *error,size_t size)
{ (void)path; (void)synthetic; (void)error; (void)size; *out=(MonitorEngine *)malloc(1); return *out!=NULL; }
static void review_close(MonitorEngine *engine) { free(engine); }
static void review_reset(MonitorEngine *engine) { (void)engine; InterlockedIncrement(&reset_calls); }
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
    pg_groups_free(app.groups); app.groups=NULL; pg_membership_destroy(app.membership); app.membership=NULL;
    gui_snapshot_free(app.latest); app.latest=NULL; gui_snapshot_free(app.shown); app.shown=NULL; app.display=NULL;
    if(app.window) DestroyWindow(app.window); app.window=NULL;
    if(app.stop) CloseHandle(app.stop); if(app.wake) CloseHandle(app.wake); if(app.worker) CloseHandle(app.worker);
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
    { wchar_t intro[8192]; GetWindowTextW(app.detail,intro,(int)(sizeof(intro)/sizeof(intro[0]))); CHECK(wcsstr(intro,L"app totals are not measured")==NULL); }
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
int main(void)
{
    test_ui_mapping(); test_rapid_pause_resume(); printf("independent_gui_review assertions=%u failures=%u\n",assertions,failures); return failures?1:0;
}
