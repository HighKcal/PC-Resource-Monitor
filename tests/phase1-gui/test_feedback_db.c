/* 합성 입력과 실제 GUI worker·SQLite로 피드백 저장 계약을 검사한다.
   지정한 출력 폴더의 새 DB만 사용하며 실제 수집·전경 화면 조작 시험은 아니다. */
#define wmain unused_gui_wmain
#include "../../gui/monitor_gui.c"
#undef wmain
#include "sqlite3.h"
static unsigned checks,failures;
#define CHECK(v) do { ++checks; if(!(v)) { ++failures; printf("FAIL DB line=%u %s\n",(unsigned)__LINE__,#v); } } while(0)
static int64_t query(const char *sql)
{
    sqlite3 *db=NULL; sqlite3_stmt *statement=NULL; int64_t value=-1;
    CHECK(sqlite3_open_v2(app.db,&db,SQLITE_OPEN_READONLY,NULL)==SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db,sql,-1,&statement,NULL)==SQLITE_OK);
    if(statement && sqlite3_step(statement)==SQLITE_ROW) value=sqlite3_column_int64(statement,0); else CHECK(0);
    sqlite3_finalize(statement); sqlite3_close(db); return value;
}
static void wait_results(unsigned count)
{
    unsigned actual=0; uint64_t deadline=GetTickCount64()+5000;
    do {
        AcquireSRWLockShared(&app.lock); actual=app.feedback_result_count; ReleaseSRWLockShared(&app.lock);
        if(actual>=count) break;
        Sleep(1);
    } while(GetTickCount64()<deadline);
    CHECK(actual==count);
}
int main(int argc,char **argv)
{
    MonitorEngine *engine=NULL; MonitorSnapshot *seed=NULL;
    ResourceData rows[17]; ResourceBatch batch={0};
    char error[256],path_text[17][100]; const char *paths[17]; int64_t ids[17];
    uint64_t now=monitor_utc_now();
    if(argc!=2) { fprintf(stderr,"output directory required\n"); return 2; }
    memset(&app,0,sizeof(app)); InitializeSRWLock(&app.lock);
    app.stop=CreateEventW(NULL,TRUE,FALSE,NULL); app.wake=CreateEventW(NULL,TRUE,FALSE,NULL);
    app.synthetic=1; app.no_grouping=1; app.pause_requested=1; app.session=1; app.interval_ms=1;
    snprintf(app.db,sizeof(app.db),"%s/feedback-%lu.sqlite",argv[1],(unsigned long)GetCurrentProcessId());
    CHECK(GetFileAttributesA(app.db)==INVALID_FILE_ATTRIBUTES);
    if(GetFileAttributesA(app.db)!=INVALID_FILE_ATTRIBUTES) return 2;
    CHECK(monitor_open(app.db,1,&engine,error,sizeof(error))); if(!engine) return 2;
    memset(rows,0,sizeof(rows)); batch.items=rows; batch.count=17; batch.logical_processor_count=1;
    batch.started_at_utc_100ns=batch.finished_at_utc_100ns=now;
    for(unsigned i=0;i<17;++i) {
        if(i) snprintf(path_text[i],sizeof(path_text[i]),"SYNTHETIC:\\phase1\\fixture-%u.exe",i);
        else snprintf(path_text[i],sizeof(path_text[i]),"SYNTHETIC:\\fixture.exe");
        paths[i]=path_text[i]; rows[i].pid=4242+i; rows[i].creation_time_utc_100ns=now-10000000;
        rows[i].process_name_utf8="phase1 fixture.exe"; rows[i].measured_at_utc_100ns=now;
        rows[i].valid_fields=RESOURCE_VALID_NAME|RESOURCE_VALID_IDENTITY|RESOURCE_VALID_MEMORY;
        rows[i].working_set_bytes=123456;
    }
    CHECK(monitor_ingest(engine,&batch,paths,&seed)); CHECK(seed && seed->count==17); if(!seed) return 2;
    for(unsigned i=0;i<17;++i) { ids[i]=seed->rows[i].program_id; CHECK(ids[i]>0); }
    monitor_close(engine);
    app.shown=calloc(1,sizeof(*app.shown)); app.shown->metrics=seed; app.shown->session=1; app.display=seed;
    for(unsigned i=0;i<16;++i) CHECK(feedback_submit(ids[i],MONITOR_FEEDBACK_IGNORE)==FEEDBACK_ACCEPTED);
    app.worker=CreateThread(NULL,0,worker_main,NULL,0,NULL); CHECK(app.worker!=NULL); wait_results(16);
    CHECK(query("SELECT count(*) FROM feedback")==16);
    CHECK(feedback_submit(ids[16],MONITOR_FEEDBACK_EXPECTED)==FEEDBACK_BUSY);
    CHECK(query("SELECT count(*) FROM feedback")==16);
    consume_feedback(); CHECK(feedback_submit(ids[16],MONITOR_FEEDBACK_EXPECTED)==FEEDBACK_ACCEPTED); wait_results(1);
    CHECK(app.feedback_results[0].outcome==FEEDBACK_SUCCEEDED); CHECK(app.feedback_results[0].revision==17);
    consume_feedback(); CHECK(query("SELECT count(*) FROM feedback")==17); CHECK(app.feedback_overlay_count==17);
    for(unsigned i=0;i<17;++i) {
        uint64_t expires;
        CHECK(displayed_feedback(&seed->rows[i],&expires)==(i==16?MONITOR_FEEDBACK_EXPECTED:MONITOR_FEEDBACK_IGNORE));
    }
    CHECK(feedback_submit(ids[0],MONITOR_FEEDBACK_CANCEL)==FEEDBACK_ACCEPTED); wait_results(1); consume_feedback();
    CHECK(query("SELECT count(*) FROM feedback")==18);
    CHECK(query("SELECT count(*) FROM feedback WHERE cancelled=0 AND kind<>0")==16);
    /* 재개 뒤 DB 저장 결과와 revision 18을 포함한 정상 frame이 공개되는지 검사한다. */
    AcquireSRWLockExclusive(&app.lock); app.pause_requested=0; ++app.session; app.count=1; ReleaseSRWLockExclusive(&app.lock);
    SetEvent(app.wake); CHECK(WaitForSingleObject(app.worker,5000)==WAIT_OBJECT_0);
    CHECK(!app.worker_error); CHECK(app.latest && app.latest->feedback_current && app.latest->feedback_revision==18);
    if(app.latest && app.latest->metrics->count) {
        CHECK(app.latest->metrics->rows[0].program_id==ids[0]); CHECK(app.latest->metrics->rows[0].feedback==MONITOR_FEEDBACK_CANCEL);
    } else CHECK(0);
    consume_feedback(); CHECK(app.feedback_outstanding==0); CHECK(feedback_submit(ids[0],MONITOR_FEEDBACK_IGNORE)==FEEDBACK_CLOSED);
    CHECK(query("SELECT count(*) FROM feedback")==18);
    CHECK(query("SELECT count(*) FROM pragma_foreign_key_check")==0);
    CHECK(query("SELECT count(*) FROM pragma_integrity_check WHERE integrity_check<>'ok'")==0);
    CHECK(query("SELECT count(*) FROM session WHERE ended IS NULL")==0);
    printf("phase1 GUI real DB checks=%u failures=%u path=%s\n",checks,failures,app.db);
    feedback_dispose(); gui_snapshot_free(app.latest); gui_snapshot_free(app.shown);
    CloseHandle(app.worker); CloseHandle(app.stop); CloseHandle(app.wake);
    return failures?1:0;
}
