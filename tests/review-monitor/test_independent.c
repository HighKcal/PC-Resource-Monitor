#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <io.h>
#include "monitor.h"
#include "../../third_party/sqlite/sqlite3.h"

static unsigned checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; fprintf(stderr,"FAIL line %u: %s\n",(unsigned)__LINE__,#c); } } while(0)
#define REVIEW_NEAR(a,b) CHECK(fabs((a)-(b)) < 0.00001)
#define TICKS UINT64_C(10000000)
static const uint64_t base = UINT64_C(134058240000000000);
static const char *dbpath="build-review-monitor/independent.sqlite";
static const char *fixture="C:\\review\\weighted.exe";
static MonitorEngine *open_fixture(void) {
    MonitorEngine *e=NULL; char error[256];
    _unlink(dbpath); _unlink("build-review-monitor/independent.sqlite-wal"); _unlink("build-review-monitor/independent.sqlite-shm");
    CHECK(monitor_open(dbpath,1,&e,error,sizeof error));
    if (!e) fprintf(stderr,"open: %s\n",error);
    return e;
}
static MonitorSnapshot *sample(MonitorEngine *e,uint64_t time,double dt,double cpu,uint64_t ram,double read,double write,unsigned mask) {
    ResourceData r; ResourceBatch b; MonitorSnapshot *s=NULL; const char *paths[1]={fixture};
    memset(&r,0,sizeof r); memset(&b,0,sizeof b);
    r.pid=77; r.creation_time_utc_100ns=base-TICKS*100;
    r.measured_at_utc_100ns=time; r.process_name_utf8="weighted.exe";
    r.valid_fields=RESOURCE_VALID_IDENTITY|RESOURCE_VALID_NAME|mask;
    r.observation_interval_seconds=dt; r.cpu_interval_seconds=dt; r.io_interval_seconds=dt;
    r.cpu_percent=cpu; r.working_set_bytes=ram; r.io_read_bytes_per_second=read; r.io_write_bytes_per_second=write;
    b.items=&r; b.count=1; b.started_at_utc_100ns=b.finished_at_utc_100ns=time;
    CHECK(monitor_ingest(e,&b,paths,&s)); CHECK(s!=NULL);
    return s;
}
static void weighted(void) {
    const unsigned valid=RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_OBSERVATION_INTERVAL;
    MonitorEngine *e=open_fixture(); MonitorSnapshot *s; uint64_t now=base; unsigned i;
    if(!e)return;
    s=sample(e,now,0,99,9999,9999,9999,RESOURCE_VALID_MEMORY); monitor_snapshot_free(s);
    for(i=0;i<18;i++) {
        double dt=i<16?3:6; now+=(uint64_t)dt*TICKS;
        s=sample(e,now,dt,i<16?1:9,i<16?100:1000,i<16?100:1000,i<16?200:2000,valid);
        if(i<17) CHECK(s->completed_windows==0);
        else {
            CHECK(s->completed_windows==1); CHECK(s->count==1);
            REVIEW_NEAR(s->rows[0].metric[MONITOR_CPU].current,2.6);
            REVIEW_NEAR(s->rows[0].metric[MONITOR_RAM].current,200);
            REVIEW_NEAR(s->rows[0].metric[MONITOR_READ].current,280);
            REVIEW_NEAR(s->rows[0].metric[MONITOR_WRITE].current,560);
            REVIEW_NEAR(s->rows[0].metric[MONITOR_CPU].covered_seconds,60);
            CHECK(s->rows[0].metric[MONITOR_RAM].valid_samples==18);
            CHECK(s->rows[0].metric[MONITOR_CPU].state==MONITOR_LEARNING);
        }
        monitor_snapshot_free(s);
    }
    for(i=0;i<20;i++) {
        now+=3*TICKS; s=sample(e,now,3,0,0,0,0,valid);
        if(i==19) {
            CHECK(s->completed_windows==1);
            REVIEW_NEAR(s->rows[0].metric[MONITOR_CPU].current,0);
            REVIEW_NEAR(s->rows[0].metric[MONITOR_RAM].current,0);
            REVIEW_NEAR(s->rows[0].metric[MONITOR_READ].current,0);
            CHECK(s->rows[0].metric[MONITOR_CPU].state==MONITOR_LEARNING);
            CHECK(s->rows[0].metric[MONITOR_RAM].valid_samples==20);
        }
        monitor_snapshot_free(s);
    }
    /* This retained snapshot must own name/path independently of engine. */
    now+=3*TICKS; s=sample(e,now,3,0,0,0,0,valid);
    monitor_close(e);
    CHECK(s && s->rows[0].name && !strcmp(s->rows[0].name,"weighted.exe"));
    CHECK(s && s->rows[0].path && !strcmp(s->rows[0].path,fixture));
    monitor_snapshot_free(s); monitor_snapshot_free(NULL);
}
static void identity(void) {
    FILETIME c,x,k,u; uint64_t expected; uint32_t error=0; char *path;
    CHECK(GetProcessTimes(GetCurrentProcess(),&c,&x,&k,&u));
    expected=((uint64_t)c.dwHighDateTime<<32)|c.dwLowDateTime;
    path=monitor_query_path(GetCurrentProcessId(),expected,&error); CHECK(path!=NULL); CHECK(error==0); free(path);
    path=monitor_query_path(GetCurrentProcessId(),expected+1,&error); CHECK(path==NULL); CHECK(error==ERROR_INVALID_DATA); free(path);
    path=monitor_query_path(GetCurrentProcessId(),0,&error); CHECK(path==NULL); CHECK(error==ERROR_INVALID_PARAMETER); free(path);
}
static int minute_selection(void); static int minute_shadow(int); static int load(void); static void same_cycle(void); static int live(void); static int capacity(void); static void date_cap(void); static void oom(void); static void policy(void); static void minute_gate(void); static void rollback(void);
int main(int argc,char **argv) {
    if(argc>1 && !strcmp(argv[1],"--live")) return live();
    if(argc>1 && !strcmp(argv[1],"--capacity")) return capacity();
    if(argc>1 && !strcmp(argv[1],"--load")) return load();
    if(argc>1 && !strcmp(argv[1],"--minute-shadow")) return minute_shadow(argc>2 && !strcmp(argv[2],"--expect-ready"));
    if(argc>1 && !strcmp(argv[1],"--minute-selection")) return minute_selection();
    identity(); weighted(); policy(); minute_gate(); rollback(); oom(); date_cap(); same_cycle();
    printf("Independent monitor: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}

/* DB fixtures below use only this reviewer's synthetic database. */
static void sqlok(sqlite3 *db,const char *sql) {
    char *error=NULL; int rc=sqlite3_exec(db,sql,NULL,NULL,&error);
    if(rc!=SQLITE_OK)fprintf(stderr,"fixture SQL: %s\n",error?error:"");
    CHECK(rc==SQLITE_OK); sqlite3_free(error);
}
static unsigned review_days=3, review_offset=1;
static void seed(sqlite3 *db,int64_t program,unsigned total,unsigned same_minutes,double cpu) {
    unsigned i,m; sqlite3_stmt *w=NULL,*v=NULL;
    sqlok(db,"BEGIN IMMEDIATE");
    CHECK(sqlite3_prepare_v2(db,"INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) VALUES((SELECT min(id) FROM collection_run),?,999,1,?,?,60,1)",-1,&w,NULL)==SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db,"INSERT INTO aggregate_metric(window_id,metric,value,covered,valid_count,eligible,state,reason,n,days,minutes,streak) VALUES(?,?,?,60,20,1,1,'review fixture',0,0,0,0)",-1,&v,NULL)==SQLITE_OK);
    for(i=0;i<total;i++) {
        uint64_t end=base-(uint64_t)(review_offset+i%review_days)*86400*TICKS+(uint64_t)(same_minutes?0:(i/review_days)%1440)*60*TICKS;
        sqlite3_int64 id;
        sqlite3_bind_int64(w,1,program); sqlite3_bind_int64(w,2,(sqlite3_int64)(end-60*TICKS)); sqlite3_bind_int64(w,3,(sqlite3_int64)end);
        CHECK(sqlite3_step(w)==SQLITE_DONE); id=sqlite3_last_insert_rowid(db); sqlite3_reset(w);
        for(m=0;m<4;m++) {
            sqlite3_bind_int64(v,1,id); sqlite3_bind_int(v,2,(int)m); sqlite3_bind_double(v,3,m==0?cpu:(m==1?100:0));
            CHECK(sqlite3_step(v)==SQLITE_DONE); sqlite3_reset(v);
        }
    }
    sqlite3_finalize(w); sqlite3_finalize(v); sqlok(db,"COMMIT");
}
static void policy(void) {
    const unsigned valid=RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_OBSERVATION_INTERVAL;
    MonitorEngine *e=open_fixture(); MonitorSnapshot *s; sqlite3 *db=NULL; uint64_t now=base; unsigned i; int64_t program;
    if(!e)return;
    s=sample(e,now,0,30,100,0,0,RESOURCE_VALID_MEMORY); program=s->rows[0].program_id; monitor_snapshot_free(s);
    CHECK(sqlite3_open(dbpath,&db)==SQLITE_OK); seed(db,program,60,0,1);
    for(i=1;i<=80;i++) {
        unsigned mask=valid; now+=3*TICKS;
        if(i==30) mask &= ~RESOURCE_VALID_CPU;
        s=sample(e,now,3,30,100,0,0,mask);
        if(i%20==0) {
            MonitorMetricReport *r=&s->rows[0].metric[MONITOR_CPU];
            CHECK(r->count==60); CHECK(r->days==3); CHECK(r->minutes==60);
            REVIEW_NEAR(r->median,1); REVIEW_NEAR(r->mad,0); REVIEW_NEAR(r->threshold,6);
            CHECK(r->baseline_eligible==0);
            CHECK(r->streak==(i==20?1:(i/20-1)));
            CHECK(r->state==(i==80?MONITOR_CANDIDATE:MONITOR_ATTENTION));
        }
        monitor_snapshot_free(s);
    }
    /* Feedback is program-wide, persisted, and never edits the reports. */
    CHECK(monitor_feedback(e,program,MONITOR_FEEDBACK_IGNORE,now));
    s=sample(e,now+3*TICKS,3,30,100,0,0,valid);
    CHECK(s->rows[0].feedback==MONITOR_FEEDBACK_IGNORE);
    CHECK(s->rows[0].feedback_expires_utc_100ns==now+3600*TICKS);
    CHECK(s->rows[0].metric[MONITOR_CPU].state==MONITOR_CANDIDATE); monitor_snapshot_free(s);
    CHECK(monitor_feedback(e,program,MONITOR_FEEDBACK_EXPECTED,now));
    s=sample(e,now+6*TICKS,3,30,100,0,0,valid);
    CHECK(s->rows[0].feedback==MONITOR_FEEDBACK_EXPECTED);
    CHECK(s->rows[0].feedback_expires_utc_100ns==now+86400*TICKS); monitor_snapshot_free(s);
    CHECK(monitor_feedback(e,program,MONITOR_FEEDBACK_CANCEL,now+TICKS));
    s=sample(e,now+9*TICKS,3,30,100,0,0,valid);
    CHECK(s->rows[0].feedback==MONITOR_FEEDBACK_CANCEL); monitor_snapshot_free(s);
    sqlite3_close(db); monitor_close(e);
}
static void minute_gate(void) {
    const unsigned valid=RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_OBSERVATION_INTERVAL;
    MonitorEngine *e=open_fixture(); MonitorSnapshot *s; sqlite3 *db=NULL; unsigned i; int64_t program;
    if(!e)return;
    s=sample(e,base,0,30,100,0,0,RESOURCE_VALID_MEMORY); program=s->rows[0].program_id; monitor_snapshot_free(s);
    CHECK(sqlite3_open(dbpath,&db)==SQLITE_OK); seed(db,program,60,1,1);
    for(i=1;i<=20;i++) {
        s=sample(e,base+(uint64_t)i*3*TICKS,3,30,100,0,0,valid);
        if(i==20) { CHECK(s->rows[0].metric[0].count==3); CHECK(s->rows[0].metric[0].days==3); CHECK(s->rows[0].metric[0].minutes==3); CHECK(s->rows[0].metric[0].state==MONITOR_LEARNING); }
        monitor_snapshot_free(s);
    }
    sqlite3_close(db); monitor_close(e);
}
static void rollback(void) {
    const unsigned valid=RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_OBSERVATION_INTERVAL;
    MonitorEngine *e=open_fixture(); MonitorSnapshot *s=NULL; sqlite3 *db=NULL; sqlite3_stmt *q=NULL; ResourceBatch b; ResourceData r; const char *paths[1]={fixture}; unsigned i;
    if(!e)return;
    s=sample(e,base,0,1,100,0,0,RESOURCE_VALID_MEMORY); monitor_snapshot_free(s);
    CHECK(sqlite3_open(dbpath,&db)==SQLITE_OK);
    for(i=1;i<20;i++){s=sample(e,base+(uint64_t)i*3*TICKS,3,1,100,0,0,valid);monitor_snapshot_free(s);}
    sqlok(db,"CREATE TRIGGER review_fail BEFORE INSERT ON aggregate_metric BEGIN SELECT RAISE(ABORT,'review controlled failure'); END");
    memset(&b,0,sizeof b);memset(&r,0,sizeof r);r.pid=77;r.creation_time_utc_100ns=base-100*TICKS;r.process_name_utf8="weighted.exe";r.measured_at_utc_100ns=base+60*TICKS;r.valid_fields=valid|RESOURCE_VALID_IDENTITY|RESOURCE_VALID_NAME;r.cpu_percent=1;r.working_set_bytes=100;r.observation_interval_seconds=r.cpu_interval_seconds=r.io_interval_seconds=3;b.items=&r;b.count=1;b.started_at_utc_100ns=b.finished_at_utc_100ns=r.measured_at_utc_100ns;
    CHECK(!monitor_ingest(e,&b,paths,&s)); CHECK(s==NULL);
    CHECK(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM raw),(SELECT count(*) FROM aggregate_window),(SELECT count(*) FROM collection_run)",-1,&q,NULL)==SQLITE_OK);
    CHECK(sqlite3_step(q)==SQLITE_ROW);CHECK(sqlite3_column_int(q,0)==20);CHECK(sqlite3_column_int(q,1)==0);CHECK(sqlite3_column_int(q,2)==20);sqlite3_finalize(q);
    sqlok(db,"DROP TRIGGER review_fail");
    s=sample(e,base+63*TICKS,3,1,100,0,0,valid); CHECK(s->completed_windows==0); monitor_snapshot_free(s);
    sqlite3_close(db);monitor_close(e);
}


/* monitor.c is compiled with /Dcalloc=review_calloc; SQLite and this test are not. */
static unsigned fail_calloc;
void *review_calloc(size_t n,size_t size) {
    if(fail_calloc && --fail_calloc==0)return NULL;
    return calloc(n,size);
}
static void oom(void) {
    unsigned fail;
    for(fail=1;fail<=3;fail++) {
        MonitorEngine *e=open_fixture(); MonitorSnapshot *s=NULL; ResourceData r; ResourceBatch b; const char *paths[1]={fixture};
        if(!e)return;
        memset(&r,0,sizeof r); memset(&b,0,sizeof b); r.pid=77;r.measured_at_utc_100ns=base;r.creation_time_utc_100ns=base-100*TICKS;r.valid_fields=RESOURCE_VALID_IDENTITY|RESOURCE_VALID_NAME;r.process_name_utf8="weighted.exe";b.items=&r;b.count=1;b.started_at_utc_100ns=b.finished_at_utc_100ns=base;
        fail_calloc=fail; CHECK(!monitor_ingest(e,&b,paths,&s)); CHECK(s==NULL); fail_calloc=0;
        s=sample(e,base+3*TICKS,3,0,0,0,0,RESOURCE_VALID_MEMORY|RESOURCE_VALID_OBSERVATION_INTERVAL);
        CHECK(s->completed_windows==0);monitor_snapshot_free(s);monitor_close(e);
    }
}


static void date_cap(void) {
    const unsigned valid=RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_OBSERVATION_INTERVAL;
    MonitorEngine *e=open_fixture(); MonitorSnapshot *s; sqlite3 *db=NULL; unsigned i; int64_t program;
    if(!e)return;
    s=sample(e,base+7200*TICKS,0,1,100,0,0,RESOURCE_VALID_MEMORY);program=s->rows[0].program_id;monitor_snapshot_free(s);
    CHECK(sqlite3_open(dbpath,&db)==SQLITE_OK);review_days=15;review_offset=0;seed(db,program,1500,0,1);review_days=3;review_offset=1;
    /* Future rows must be filtered before each date's LIMIT 64. */
    sqlok(db,"INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) SELECT run_id,program_id,pid,creation,start_utc+108000000000,end_utc+108000000000,duration,version FROM aggregate_window WHERE end_utc>=134058240000000000");
    sqlok(db,"INSERT INTO aggregate_metric(window_id,metric,value,covered,valid_count,eligible,state,reason,n,days,minutes,streak) SELECT w.id,m.metric,999,60,20,1,1,'future review',0,0,0,0 FROM aggregate_window w JOIN aggregate_metric m ON m.window_id=(SELECT min(id) FROM aggregate_window) WHERE w.end_utc>=134058348000000000");
    for(i=1;i<=20;i++) {
        s=sample(e,base+7200*TICKS+(uint64_t)i*3*TICKS,3,1,100,0,0,valid);
        if(i==20){CHECK(s->rows[0].metric[0].count==896);CHECK(s->rows[0].metric[0].days==14);CHECK(s->rows[0].metric[0].minutes==896);REVIEW_NEAR(s->rows[0].metric[0].median,1);CHECK(s->rows[0].metric[0].state==MONITOR_NORMAL);}
        monitor_snapshot_free(s);
    }
    sqlite3_close(db);monitor_close(e);
}



static void database_stats(const char *file,const char *tag) {
    sqlite3 *db=NULL; sqlite3_stmt *q=NULL; WIN32_FILE_ATTRIBUTE_DATA info;
    CHECK(sqlite3_open_v2(file,&db,SQLITE_OPEN_READONLY,NULL)==SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM raw),(SELECT count(*) FROM aggregate_window),(SELECT count(*) FROM aggregate_metric),(SELECT count(*) FROM raw WHERE cpu IS NULL),(SELECT count(*) FROM raw WHERE open_error<>0),(SELECT count(*) FROM aggregate_metric WHERE state=1),(SELECT count(*) FROM raw WHERE cpu=0)",-1,&q,NULL)==SQLITE_OK);
    CHECK(sqlite3_step(q)==SQLITE_ROW);
    printf("%s db counts raw=%lld windows=%lld metrics=%lld cpu_NULL=%lld open_errors=%lld learning_metrics=%lld valid_cpu_zero=%lld\n",tag,(long long)sqlite3_column_int64(q,0),(long long)sqlite3_column_int64(q,1),(long long)sqlite3_column_int64(q,2),(long long)sqlite3_column_int64(q,3),(long long)sqlite3_column_int64(q,4),(long long)sqlite3_column_int64(q,5),(long long)sqlite3_column_int64(q,6));
    sqlite3_finalize(q);sqlite3_close(db);
    CHECK(GetFileAttributesExA(file,GetFileExInfoStandard,&info));printf("%s checkpointed_file_bytes=%llu\n",tag,((unsigned long long)info.nFileSizeHigh<<32)|info.nFileSizeLow);
}
static int live(void) {
    const char *file="build-review-monitor/live.sqlite"; MonitorEngine *e=NULL; ResourceCollector *collector=NULL; char error[256]; uint32_t os_error=0; unsigned i; size_t rows=0,windows=0; double collect_sum=0,collect_max=0,store_sum=0,store_max=0; LARGE_INTEGER freq;
    _unlink(file);_unlink("build-review-monitor/live.sqlite-wal");_unlink("build-review-monitor/live.sqlite-shm");
    printf("LIVE actual collector 22 rounds / nominal 3s; no long-term baseline claim\n");fflush(stdout);
    CHECK(monitor_open(file,0,&e,error,sizeof error));CHECK(resource_collector_create(NULL,&collector,&os_error)==RESOURCE_OK);CHECK(QueryPerformanceFrequency(&freq));
    if(!e||!collector)return 1;
    for(i=0;i<22;i++) {
        LARGE_INTEGER begin,end;ResourceBatch batch={0};MonitorSnapshot *s=NULL;double ms;
        QueryPerformanceCounter(&begin);CHECK(resource_collector_collect(collector,&batch,&os_error)==RESOURCE_OK);QueryPerformanceCounter(&end);
        ms=(double)(end.QuadPart-begin.QuadPart)*1000/(double)freq.QuadPart;collect_sum+=ms;if(ms>collect_max)collect_max=ms;
        CHECK(monitor_ingest(e,&batch,NULL,&s));CHECK(s!=NULL);
        if(s){size_t j;rows+=s->count;windows+=s->completed_windows;store_sum+=s->store_ms;if(s->store_ms>store_max)store_max=s->store_ms;
            for(j=0;j<s->count;j++){CHECK(s->rows[j].raw.process_name_utf8==s->rows[j].name);if(!s->rows[j].path)CHECK(s->rows[j].program_id==0);}
            printf("LIVE batch=%u rows=%zu windows=%zu collect_ms=%.3f store_ms=%.3f\n",i+1,s->count,s->completed_windows,ms,s->store_ms);fflush(stdout);
        }
        resource_batch_free(&batch);monitor_snapshot_free(s);if(i<21)Sleep(3000);
    }
    CHECK(windows>0);resource_collector_destroy(collector);monitor_close(e);
    printf("LIVE totals rows=%zu windows=%zu collect_mean_ms=%.3f collect_max_ms=%.3f store_mean_ms=%.3f store_max_ms=%.3f\n",rows,windows,collect_sum/22,collect_max,store_sum/22,store_max);database_stats(file,"LIVE");
    printf("LIVE %u checks, %u failures\n",checks,failures);return failures?1:0;
}
static int capacity(void) {
    MonitorEngine *e=open_fixture();MonitorSnapshot *s;sqlite3 *db=NULL;int64_t program;LARGE_INTEGER begin,end,freq;unsigned i;double close_ms=0;
    const unsigned valid=RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_OBSERVATION_INTERVAL;
    if(!e)return 1;s=sample(e,base+7200*TICKS,0,1,100,0,0,RESOURCE_VALID_MEMORY);program=s->rows[0].program_id;monitor_snapshot_free(s);
    CHECK(sqlite3_open(dbpath,&db)==SQLITE_OK);QueryPerformanceFrequency(&freq);QueryPerformanceCounter(&begin);review_days=14;review_offset=0;seed(db,program,100000,0,1);review_days=3;review_offset=1;QueryPerformanceCounter(&end);
    printf("CAPACITY SYNTHETIC direct seed windows=100000 metrics=400000 elapsed_ms=%.3f product_SQLite=%s\n",(double)(end.QuadPart-begin.QuadPart)*1000/(double)freq.QuadPart,sqlite3_libversion());
    for(i=1;i<=20;i++){s=sample(e,base+7200*TICKS+(uint64_t)i*3*TICKS,3,1,100,0,0,valid);if(i==20){close_ms=s->store_ms;CHECK(s->rows[0].metric[0].state==MONITOR_NORMAL);}monitor_snapshot_free(s);}
    sqlite3_close(db);monitor_close(e);printf("CAPACITY one-instance close transaction_ms=%.3f (4 metrics, up to 14 dates x 64 representative seeks; capped subset)\n",close_ms);database_stats(dbpath,"CAPACITY");printf("CAPACITY %u checks, %u failures\n",checks,failures);return failures?1:0;
}


static void same_cycle(void) {
    const unsigned valid=RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_OBSERVATION_INTERVAL;
    MonitorEngine *e=open_fixture();MonitorSnapshot *s=NULL;sqlite3 *db=NULL;ResourceData r[2];ResourceBatch b;const char *paths[2]={fixture,fixture};unsigned i,j;
    if(!e)return;memset(&b,0,sizeof b);memset(r,0,sizeof r);b.items=r;b.count=2;
    for(j=0;j<2;j++){r[j].pid=77+j;r[j].creation_time_utc_100ns=base-100*TICKS;r[j].process_name_utf8="weighted.exe";r[j].measured_at_utc_100ns=base;r[j].valid_fields=valid|RESOURCE_VALID_IDENTITY|RESOURCE_VALID_NAME;r[j].cpu_percent=j?30:1;r[j].working_set_bytes=100;r[j].observation_interval_seconds=r[j].cpu_interval_seconds=r[j].io_interval_seconds=3;}
    b.started_at_utc_100ns=b.finished_at_utc_100ns=base;CHECK(monitor_ingest(e,&b,paths,&s));CHECK(s!=NULL);CHECK(sqlite3_open(dbpath,&db)==SQLITE_OK);seed(db,s->rows[0].program_id,59,0,1);monitor_snapshot_free(s);
    for(i=1;i<=60;i++) {
        for(j=0;j<2;j++)r[j].measured_at_utc_100ns=base+(uint64_t)i*3*TICKS;
        b.started_at_utc_100ns=b.finished_at_utc_100ns=r[0].measured_at_utc_100ns;CHECK(monitor_ingest(e,&b,paths,&s));CHECK(s!=NULL);
        if(i==20||i==40){CHECK(s->completed_windows==2);for(j=0;j<2;j++){CHECK(s->rows[j].metric[0].count==59);CHECK(s->rows[j].metric[0].state==MONITOR_LEARNING);}}
        if(i==60){CHECK(s->completed_windows==2);for(j=0;j<2;j++)CHECK(s->rows[j].metric[0].count==60);CHECK(s->rows[0].metric[0].state==MONITOR_NORMAL);CHECK(s->rows[1].metric[0].state==MONITOR_ATTENTION);}
        monitor_snapshot_free(s);
    }
    sqlite3_close(db);monitor_close(e);
}

static int review_double_order(const void *a,const void *b){double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y);}
static int load(void) {
    const char *file="build-review-monitor/load.sqlite";MonitorEngine *e=NULL;char error[256];ResourceData *r=calloc(500,sizeof(*r));ResourceBatch b;const char *paths[500];char names[500][64];double times[210],total=0;size_t windows=0;unsigned i,j;
    _unlink(file);_unlink("build-review-monitor/load.sqlite-wal");_unlink("build-review-monitor/load.sqlite-shm");CHECK(r!=NULL);CHECK(monitor_open(file,1,&e,error,sizeof error));if(!r||!e)return 1;memset(&b,0,sizeof b);b.items=r;b.count=500;b.logical_processor_count=12;
    for(j=0;j<500;j++){snprintf(names[j],sizeof names[j],"C:\\review\\load_%03u.exe",j);paths[j]=names[j];r[j].pid=10000+j;r[j].creation_time_utc_100ns=base-100*TICKS;r[j].process_name_utf8="load.exe";r[j].valid_fields=RESOURCE_VALID_IDENTITY|RESOURCE_VALID_NAME|RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_OBSERVATION_INTERVAL;r[j].cpu_percent=(double)(1+j%10);r[j].working_set_bytes=67108864;r[j].observation_interval_seconds=r[j].cpu_interval_seconds=r[j].io_interval_seconds=3;}
    for(i=0;i<210;i++){MonitorSnapshot *s=NULL;for(j=0;j<500;j++)r[j].measured_at_utc_100ns=base+(uint64_t)i*3*TICKS;b.started_at_utc_100ns=b.finished_at_utc_100ns=r[0].measured_at_utc_100ns;CHECK(monitor_ingest(e,&b,paths,&s));CHECK(s!=NULL);if(s){times[i]=s->store_ms;total+=times[i];windows+=s->completed_windows;CHECK(s->count==500);if(s->completed_windows)printf("LOAD round=%u windows=%zu ingest_ms=%.3f\n",i,s->completed_windows,s->store_ms);}monitor_snapshot_free(s);}
    CHECK(windows==5000);free(r);monitor_close(e);qsort(times,210,sizeof times[0],review_double_order);printf("LOAD SYNTHETIC 500 distinct paths x 210 rounds / 3s represented intervals, no waits; windows=%zu mean_ms=%.3f median_ms=%.3f p95_ms=%.3f max_ms=%.3f\n",windows,total/210,times[105],times[199],times[209]);database_stats(file,"LOAD");printf("LOAD %u checks, %u failures\n",checks,failures);return failures?1:0;
}


static void minute_counts(sqlite3 *db,const char *label) {
    sqlite3_stmt *q=NULL;
    CHECK(sqlite3_prepare_v2(db,"SELECT count(*),count(DISTINCT w.end_utc/600000000),count(DISTINCT w.end_utc/864000000000) FROM aggregate_window w JOIN aggregate_metric m ON m.window_id=w.id WHERE m.metric=0 AND m.eligible=1",-1,&q,NULL)==SQLITE_OK);
    CHECK(sqlite3_step(q)==SQLITE_ROW);
    printf("MINUTE_SHADOW %s full_eligible_CPU_rows=%d distinct_end_minutes=%d dates=%d\n",label,sqlite3_column_int(q,0),sqlite3_column_int(q,1),sqlite3_column_int(q,2));sqlite3_finalize(q);
}
static void newer_duplicates(sqlite3 *db,int64_t program) {
    sqlite3_stmt *w=NULL,*v=NULL;unsigned day,i,m;
    sqlok(db,"BEGIN IMMEDIATE");
    CHECK(sqlite3_prepare_v2(db,"INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) VALUES((SELECT min(id) FROM collection_run),?,?,1,?,?,60,1)",-1,&w,NULL)==SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db,"INSERT INTO aggregate_metric(window_id,metric,value,covered,valid_count,eligible,state,reason,n,days,minutes,streak) VALUES(?,?,?,60,20,1,1,'shadow fixture',0,0,0,0)",-1,&v,NULL)==SQLITE_OK);
    for(day=1;day<=3;day++)for(i=0;i<96;i++){
        sqlite3_int64 id;uint64_t end=base-(uint64_t)day*86400*TICKS+20*60*TICKS;
        sqlite3_bind_int64(w,1,program);sqlite3_bind_int(w,2,(int)(1000+i));sqlite3_bind_int64(w,3,(sqlite3_int64)(end-60*TICKS));sqlite3_bind_int64(w,4,(sqlite3_int64)end);CHECK(sqlite3_step(w)==SQLITE_DONE);id=sqlite3_last_insert_rowid(db);sqlite3_reset(w);
        for(m=0;m<4;m++){sqlite3_bind_int64(v,1,id);sqlite3_bind_int(v,2,(int)m);sqlite3_bind_double(v,3,m==0?1:(m==1?100:0));CHECK(sqlite3_step(v)==SQLITE_DONE);sqlite3_reset(v);}
    }
    sqlite3_finalize(w);sqlite3_finalize(v);sqlok(db,"COMMIT");
}
static int minute_shadow(int expect_ready) {
    const unsigned valid=RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_OBSERVATION_INTERVAL;
    unsigned sparse,i;printf("MINUTE_SHADOW product_SQLite=%s expectation=%s\n",sqlite3_libversion(),expect_ready?"fixed ready for diverse history":"reproduce current false-learning");
    for(sparse=0;sparse<2;sparse++){
        MonitorEngine *e=open_fixture();MonitorSnapshot *s;sqlite3 *db=NULL;int64_t program;
        if(!e)return 1;s=sample(e,base,0,1,100,0,0,RESOURCE_VALID_MEMORY);program=s->rows[0].program_id;monitor_snapshot_free(s);CHECK(sqlite3_open(dbpath,&db)==SQLITE_OK);
        seed(db,program,60,sparse,1);if(!sparse)newer_duplicates(db,program);minute_counts(db,sparse?"sparse":"diverse+duplicates");
        for(i=1;i<=20;i++){
            s=sample(e,base+(uint64_t)i*3*TICKS,3,1,100,0,0,valid);
            if(i==20){MonitorMetricReport *r=&s->rows[0].metric[0];printf("MINUTE_SHADOW %s observed state=%s n=%u days=%u minutes=%u median=%.3f threshold=%.3f\n",sparse?"sparse":"diverse+duplicates",monitor_state_name(r->state),r->count,r->days,r->minutes,r->median,r->threshold);CHECK(r->days==3);if(sparse){CHECK(r->state==MONITOR_LEARNING);CHECK(r->minutes==3);}else if(expect_ready){CHECK(r->state==MONITOR_NORMAL);CHECK(r->count==63);CHECK(r->minutes==63);}else{CHECK(r->state==MONITOR_LEARNING);CHECK(r->count==192);CHECK(r->minutes==3);}}
            monitor_snapshot_free(s);
        }
        sqlite3_close(db);monitor_close(e);
    }
    printf("MINUTE_SHADOW %u checks, %u failures; %s\n",checks,failures,expect_ready?"regression expectation":"known deficiency reproduction only");return failures?1:0;
}


static void one_seed(sqlite3 *db,int64_t program,uint64_t end,unsigned pid,double value,int eligible,int null_value) {
    sqlite3_stmt *w=NULL,*v=NULL;sqlite3_int64 id;
    CHECK(sqlite3_prepare_v2(db,"INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) VALUES((SELECT min(id) FROM collection_run),?,?,1,?,?,60,1)",-1,&w,NULL)==SQLITE_OK);
    sqlite3_bind_int64(w,1,program);sqlite3_bind_int(w,2,(int)pid);sqlite3_bind_int64(w,3,(sqlite3_int64)(end-60*TICKS));sqlite3_bind_int64(w,4,(sqlite3_int64)end);CHECK(sqlite3_step(w)==SQLITE_DONE);id=sqlite3_last_insert_rowid(db);sqlite3_finalize(w);
    CHECK(sqlite3_prepare_v2(db,"INSERT INTO aggregate_metric(window_id,metric,value,covered,valid_count,eligible,state,reason,n,days,minutes,streak) VALUES(?,0,?,60,20,?,1,'minute representative fixture',0,0,0,0)",-1,&v,NULL)==SQLITE_OK);
    sqlite3_bind_int64(v,1,id);if(null_value)sqlite3_bind_null(v,2);else sqlite3_bind_double(v,2,value);sqlite3_bind_int(v,3,eligible);CHECK(sqlite3_step(v)==SQLITE_DONE);sqlite3_finalize(v);
}
static void selection_case(unsigned kind) {
    const unsigned valid=RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_OBSERVATION_INTERVAL;
    MonitorEngine *e=open_fixture();MonitorSnapshot *s;sqlite3 *db=NULL;int64_t program;unsigned day,min,pid,i;uint64_t anchor=base;double current=1;
    if(!e)return;if(kind==0)anchor=base+12*3600*TICKS;if(kind==1)current=13;if(kind==2)current=0;if(kind==3)anchor=base+30*TICKS;
    s=sample(e,anchor,0,current,100,0,0,RESOURCE_VALID_MEMORY);program=s->rows[0].program_id;monitor_snapshot_free(s);CHECK(sqlite3_open(dbpath,&db)==SQLITE_OK);sqlok(db,"BEGIN IMMEDIATE");
    if(kind==0){for(day=0;day<14;day++)for(min=1;min<=20;min++)for(pid=0;pid<32;pid++)one_seed(db,program,base-(uint64_t)day*86400*TICKS+(uint64_t)min*60*TICKS,1000+pid,1,1,0);}
    else if(kind==1||kind==2){for(day=1;day<=3;day++)for(min=1;min<=20;min++){uint64_t end=base-(uint64_t)day*86400*TICKS+(uint64_t)min*60*TICKS;one_seed(db,program,end,1000,7,1,0);one_seed(db,program,end+10*TICKS,1001,kind==1?11:0,1,0);if(kind==1)one_seed(db,program,end+10*TICKS,1002,13,1,0);one_seed(db,program,end+20*TICKS,1003,999,kind==2?1:0,kind==2);if(kind==2){one_seed(db,program,end+30*TICKS,1004,-1,1,0);one_seed(db,program,end+40*TICKS,1005,HUGE_VAL,1,0);}}}
    else {sqlok(db,"COMMIT");seed(db,program,59,0,1);sqlok(db,"BEGIN IMMEDIATE");one_seed(db,program,base+10*TICKS,2000,1,1,0);one_seed(db,program,base+40*TICKS,2001,999,1,0);}
    sqlok(db,"COMMIT");minute_counts(db,kind==0?"14-day dense":kind==1?"latest eligible tie":"zero/future filter");
    for(i=1;i<=20;i++){s=sample(e,anchor+(uint64_t)i*3*TICKS,3,current,100,0,0,valid);if(i==20){MonitorMetricReport *r=&s->rows[0].metric[0];printf("MINUTE_SELECTION kind=%u state=%s n=%u days=%u minutes=%u median=%.3f threshold=%.3f store_ms=%.3f\n",kind,monitor_state_name(r->state),r->count,r->days,r->minutes,r->median,r->threshold,s->store_ms);CHECK(r->state==MONITOR_NORMAL);CHECK(r->count==(kind==0?280u:60u));CHECK(r->minutes==(kind==0?280u:60u));CHECK(r->days==(kind==0?14u:(kind==3?4u:3u)));REVIEW_NEAR(r->median,current);if(kind==1)REVIEW_NEAR(r->threshold,19.5);if(kind==2)REVIEW_NEAR(r->threshold,5);}
        monitor_snapshot_free(s);}
    sqlite3_close(db);monitor_close(e);
}
static void cutoff_case(void) {
    const unsigned valid=RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_OBSERVATION_INTERVAL;
    MonitorEngine *e=open_fixture();MonitorSnapshot *s=NULL;sqlite3 *db=NULL;ResourceData r[2];ResourceBatch b;const char *paths[2]={fixture,fixture};unsigned i,j;
    if(!e)return;memset(&b,0,sizeof b);memset(r,0,sizeof r);b.items=r;b.count=2;
    for(j=0;j<2;j++){r[j].pid=77+j;r[j].creation_time_utc_100ns=base-1000*TICKS;r[j].process_name_utf8="weighted.exe";r[j].measured_at_utc_100ns=base-(j?0:120*TICKS);r[j].valid_fields=valid|RESOURCE_VALID_IDENTITY|RESOURCE_VALID_NAME;r[j].cpu_percent=1;r[j].working_set_bytes=100;r[j].observation_interval_seconds=r[j].cpu_interval_seconds=r[j].io_interval_seconds=3;}
    b.started_at_utc_100ns=b.finished_at_utc_100ns=base;CHECK(monitor_ingest(e,&b,paths,&s));CHECK(s!=NULL);CHECK(sqlite3_open(dbpath,&db)==SQLITE_OK);seed(db,s->rows[0].program_id,59,0,1);monitor_snapshot_free(s);
    for(i=1;i<=20;i++){for(j=0;j<2;j++)r[j].measured_at_utc_100ns=base-(j?0:120*TICKS)+(uint64_t)i*3*TICKS;b.started_at_utc_100ns=b.finished_at_utc_100ns=r[1].measured_at_utc_100ns;CHECK(monitor_ingest(e,&b,paths,&s));CHECK(s!=NULL);if(i==20){CHECK(s->completed_windows==2);CHECK(s->rows[0].metric[0].window_end_utc_100ns< s->rows[1].metric[0].window_start_utc_100ns);CHECK(s->rows[1].metric[0].count==59);CHECK(s->rows[1].metric[0].state==MONITOR_LEARNING);printf("MINUTE_SELECTION cutoff-isolated Aend=%llu Bstart=%llu Bn=%u Bstate=%s\n",(unsigned long long)s->rows[0].metric[0].window_end_utc_100ns,(unsigned long long)s->rows[1].metric[0].window_start_utc_100ns,s->rows[1].metric[0].count,monitor_state_name(s->rows[1].metric[0].state));}monitor_snapshot_free(s);}
    sqlite3_close(db);monitor_close(e);
}
static int minute_selection(void) {unsigned i;for(i=0;i<4;i++)selection_case(i);cutoff_case();printf("MINUTE_SELECTION %u checks, %u failures\n",checks,failures);return failures?1:0;}
