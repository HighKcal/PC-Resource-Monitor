/* Copied from build-perf-20261103/retained-analysis/retained_probe.c.
 * Phase 1 adds optional uninstrumented measurement; seed/oracle stays unchanged.
 * Test-only TU: calls product trim/gap/store through experiment_fixture.h.
 * The build adapter links matching product objects or a historical monolith.
 * All writes target new copies. Source must be a SYNTHETIC evidence database.
 */
#include "../support/experiment_fixture.h"
#include <psapi.h>

#define R_SESSION UINT64_C(900000000)
#define R_TRACE_MAX 128
/* The frozen presplit source exposes trim; the final coordinator delegates to
 * the private retention API. Both calls use the same caller-owned transaction. */
static int r_trim(Experiment *e, uint64_t utc)
{
    return experiment_fixture_trim(e, utc);
}
typedef struct RSample {
    char *sql;
    sqlite3_int64 ns;
    int vm, fullscan, sorts;
} RSample;
typedef struct RTrace { RSample rows[R_TRACE_MAX]; unsigned count; int overflow; } RTrace;
static const char *r_tables[] = {"operation", "window_evidence", "metric_evidence", "decision", "episode", "reduction", "evaluation", "retention_state"};

static void r_die(sqlite3 *db, const char *where)
{ fprintf(stderr, "FAIL %s: %s\n", where, db ? sqlite3_errmsg(db) : "no connection"); exit(1); }
static void r_sql(sqlite3 *db, const char *sql)
{ if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) r_die(db, sql); }
static sqlite3_int64 r_scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL; sqlite3_int64 v;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK || sqlite3_step(s) != SQLITE_ROW) r_die(db, sql);
    v = sqlite3_column_int64(s, 0); sqlite3_finalize(s); return v;
}
static double r_ms(LARGE_INTEGER a, LARGE_INTEGER b)
{ LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)(b.QuadPart-a.QuadPart)*1000/(double)f.QuadPart; }
static uint64_t r_hash(uint64_t h, const void *p, size_t n)
{ const unsigned char *b = p; size_t i; for (i=0;i<n;++i) { h ^= b[i]; h *= UINT64_C(1099511628211); } return h; }
static uint64_t r_query_hash(sqlite3 *db, const char *sql, sqlite3_int64 *count)
{
    sqlite3_stmt *s=NULL; int rc, i; uint64_t h=UINT64_C(1469598103934665603); *count=0;
    if (sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK) r_die(db,sql);
    while ((rc=sqlite3_step(s))==SQLITE_ROW) {
        ++*count;
        for(i=0;i<sqlite3_column_count(s);++i) {
            int t=sqlite3_column_type(s,i), bytes; h=r_hash(h,&t,sizeof(t));
            if(t==SQLITE_INTEGER) { sqlite3_int64 v=sqlite3_column_int64(s,i); h=r_hash(h,&v,sizeof(v)); }
            else if(t==SQLITE_FLOAT) { double v=sqlite3_column_double(s,i); h=r_hash(h,&v,sizeof(v)); }
            else if(t!=SQLITE_NULL) { const void *p=sqlite3_column_blob(s,i); bytes=sqlite3_column_bytes(s,i); h=r_hash(h,&bytes,sizeof(bytes)); h=r_hash(h,p,(size_t)bytes); }
        }
    }
    if(rc!=SQLITE_DONE) r_die(db,sql); sqlite3_finalize(s); return h;
}
static uint64_t r_inventory(sqlite3 *db, const char *phase)
{
    unsigned i; uint64_t all=UINT64_C(1469598103934665603);
    for(i=0;i<sizeof(r_tables)/sizeof(r_tables[0]);++i) {
        char sql[256]; sqlite3_int64 n; uint64_t h;
        const char *order=!strcmp(r_tables[i],"metric_evidence") ? "1,2" : !strcmp(r_tables[i],"reduction") ? "1,2,3,4,5" : "1";
        snprintf(sql,sizeof(sql),"SELECT * FROM %s ORDER BY %s",r_tables[i],order);
        h=r_query_hash(db,sql,&n); all=r_hash(all,&h,sizeof(h));
        printf("TABLE phase=%s name=%s rows=%lld hash=%016llx\n",phase,r_tables[i],(long long)n,(unsigned long long)h);
    }
    return all;
}
static void r_check(sqlite3 *db)
{
    sqlite3_stmt *s=NULL; int rc;
    if(sqlite3_prepare_v2(db,"PRAGMA integrity_check",-1,&s,NULL)!=SQLITE_OK || sqlite3_step(s)!=SQLITE_ROW || strcmp(experiment_fixture_column_text(s,0),"ok")) r_die(db,"integrity_check");
    sqlite3_finalize(s); s=NULL;
    if(sqlite3_prepare_v2(db,"PRAGMA foreign_key_check",-1,&s,NULL)!=SQLITE_OK) r_die(db,"foreign_key_check");
    rc=sqlite3_step(s); sqlite3_finalize(s); if(rc!=SQLITE_DONE) r_die(db,"foreign_key_check rows");
    printf("CHECK integrity=ok foreign_key_errors=0 dangling_evaluation=%lld\n",(long long)r_scalar(db,
        "SELECT count(*) FROM evaluation v WHERE (v.episode IS NOT NULL AND NOT EXISTS(SELECT 1 FROM episode e WHERE e.id=v.episode)) OR (v.decision IS NOT NULL AND NOT EXISTS(SELECT 1 FROM decision d WHERE d.id=v.decision))"));
}
static uint64_t r_file_bytes(const char *p)
{
    wchar_t wide[32768]; WIN32_FILE_ATTRIBUTE_DATA a;
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,p,-1,wide,32768) || !GetFileAttributesExW(wide,GetFileExInfoStandard,&a)) return 0;
    return ((uint64_t)a.nFileSizeHigh<<32)|a.nFileSizeLow;
}
static void r_sizes(sqlite3 *db,const char *path,const char *phase)
{
    char wal[32768],shm[32768]; PROCESS_MEMORY_COUNTERS_EX m; int valid;
    snprintf(wal,sizeof(wal),"%s-wal",path); snprintf(shm,sizeof(shm),"%s-shm",path);
    memset(&m,0,sizeof(m)); m.cb=sizeof(m); valid=GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS *)&m,sizeof(m))!=0;
    printf("SIZE phase=%s db=%llu wal=%llu shm=%llu pages=%lld free_pages=%lld memory_valid=%d ws=%llu private=%llu\n",phase,
        (unsigned long long)r_file_bytes(path),(unsigned long long)r_file_bytes(wal),(unsigned long long)r_file_bytes(shm),
        db?(long long)r_scalar(db,"PRAGMA page_count"):0,db?(long long)r_scalar(db,"PRAGMA freelist_count"):0,valid,
        valid?(unsigned long long)m.WorkingSetSize:0,valid?(unsigned long long)m.PrivateUsage:0);
}
static void r_backup_new(const char *source,const char *dest)
{
    sqlite3 *from=NULL,*to=NULL; sqlite3_backup *b; wchar_t wide[32768]; HANDLE file; int rc;
    if(!experiment_fixture_file_absent(dest) || !experiment_fixture_local_path(dest,wide)) r_die(NULL,"destination or WAL/SHM already exists / invalid path");
    if(sqlite3_open_v2(source,&from,SQLITE_OPEN_READONLY,NULL)!=SQLITE_OK) r_die(from,"source readonly open");
    if(r_scalar(from,"PRAGMA application_id")!=EXPERIMENT_FIXTURE_APP_ID || r_scalar(from,"SELECT synthetic FROM experiment")!=1) r_die(from,"source must be synthetic evidence");
    file=CreateFileW(wide,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) r_die(NULL,"atomic new destination"); CloseHandle(file);
    if(sqlite3_open_v2(dest,&to,SQLITE_OPEN_READWRITE,NULL)!=SQLITE_OK) r_die(to,"new destination open");
    b=sqlite3_backup_init(to,"main",from,"main"); if(!b) r_die(to,"backup init");
    rc=sqlite3_backup_step(b,-1); if(rc!=SQLITE_DONE) r_die(to,"backup step");
    if(sqlite3_backup_finish(b)!=SQLITE_OK) r_die(to,"backup finish");
    if(sqlite3_close(to)!=SQLITE_OK || sqlite3_close(from)!=SQLITE_OK) r_die(NULL,"backup close");
}
static Experiment *r_open(const char *path)
{
    Experiment *e=NULL; char error[512];
    if(!experiment_open_review(path,&e,error,sizeof(error))) { fprintf(stderr,"FAIL open: %s\n",error); exit(1); }
    if(!experiment_fixture_synthetic(e) || strcmp(sqlite3_libversion(),"3.54.1")) r_die(experiment_fixture_db(e),"expected synthetic SQLite 3.54.1");
    return e;
}
static void r_seed(const char *source,const char *dest,unsigned windows)
{
    Experiment *e; unsigned g,j,m,groups=windows/4; uint64_t now,base_creation; char sql[1024]; LARGE_INTEGER a,b;
    if(windows<4 || windows>60000 || windows%4) r_die(NULL,"windows must be multiple of 4 in [4,60000]");
    r_backup_new(source,dest); e=r_open(dest); QueryPerformanceCounter(&a);
    now=(uint64_t)r_scalar(experiment_fixture_db(e),"SELECT max(utc) FROM operation")+DAY;
    if(now<30*DAY) r_die(experiment_fixture_db(e),"seed timestamp range"); base_creation=now-29*DAY;
    r_sql(experiment_fixture_db(e),"PRAGMA synchronous=FULL; BEGIN IMMEDIATE; UPDATE experiment SET source=''; DELETE FROM reduction;");
    if(!experiment_fixture_set_source(e,"")) r_die(experiment_fixture_db(e),"source label allocation");
    for(g=0;g<groups;++g) {
        uint64_t start=now-27*DAY+(uint64_t)g*(26*DAY)/(uint64_t)groups;
        uint64_t creation=base_creation-(uint64_t)g*TICKS; uint32_t pid=50000+g%500;
        int active=(g%10==0 && g+500>=groups);
        for(j=0;j<4;++j) {
            MonitorMetricReport reports[4]; memset(reports,0,sizeof(reports));
            for(m=0;m<4;++m) {
                MonitorMetricReport *r=&reports[m]; r->state=MONITOR_NORMAL; r->baseline_eligible=1;
                r->window_start_utc_100ns=start+(uint64_t)j*60*TICKS; r->window_end_utc_100ns=r->window_start_utc_100ns+60*TICKS;
                r->window_duration_seconds=60; r->covered_seconds=60; r->valid_samples=20; r->count=896; r->days=14; r->minutes=896;
                r->current=m==0?((j==3&&!active)?0:50):(m==1?67108864:0);
                r->median=m==0?50:(m==1?67108864:0); r->mad=0;
                r->threshold=m==0?75:(m==1?201326592:1048576);
                snprintf(r->reason,sizeof(r->reason),"synthetic cardinality fixture; not observed baseline training");
            }
            if(!experiment_fixture_store_window(e,0,0,0,R_SESSION,pid,creation,"SYNTHETIC retained fixture.exe","SYNTHETIC:\\retained-fixture.exe",reports,0,0,0)) {
                fprintf(stderr,"FAIL store_window: %s\n",experiment_error(e)); exit(1);
            }
        }
    }
    /* Synthetic ratings are explicitly UNRATED/unknown/NULL time, not human outcomes.
     * Keep only latest 500 instance reductions; history episodes/evidence remain. */
    snprintf(sql,sizeof(sql),"DELETE FROM reduction WHERE session=%llu AND creation>%llu;"
        "INSERT INTO evaluation(episode,decision,trial,ordering,judgment,understood,target_correct,time_correct,metric_correct,next_check,elapsed,note,created) "
        "SELECT id,last_decision,'synthetic-cardinality-fixture','fixture-order',0,-1,-1,-1,-1,-1,NULL,'synthetic load record, not user evaluation',max(last,ended) FROM episode WHERE session=%llu;"
        "CREATE TABLE benchmark_fixture(now_utc INTEGER NOT NULL,added_windows INTEGER NOT NULL); INSERT INTO benchmark_fixture VALUES(%llu,%u); COMMIT;",
        (unsigned long long)R_SESSION,(unsigned long long)(base_creation-(uint64_t)(groups>500?groups-500:0)*TICKS),
        (unsigned long long)R_SESSION,(unsigned long long)now,windows);
    r_sql(experiment_fixture_db(e),sql); QueryPerformanceCounter(&b);
    printf("SEED added_windows=%u added_decisions=%u added_episodes=%u elapsed_ms=%.3f source_refs=0 source_unavailable=1\n",windows,windows*8,groups,r_ms(a,b));
    r_check(experiment_fixture_db(e));
    if(r_scalar(experiment_fixture_db(e),"SELECT count(*) FROM evaluation v WHERE (v.episode IS NOT NULL AND NOT EXISTS(SELECT 1 FROM episode e WHERE e.id=v.episode)) OR (v.decision IS NOT NULL AND NOT EXISTS(SELECT 1 FROM decision d WHERE d.id=v.decision))")) r_die(experiment_fixture_db(e),"seed dangling evaluation");
    (void)r_inventory(experiment_fixture_db(e),"seed");
    r_sql(experiment_fixture_db(e),"PRAGMA wal_checkpoint(TRUNCATE)"); r_sizes(experiment_fixture_db(e),dest,"seed-open"); experiment_close(e); r_sizes(NULL,dest,"seed-closed");
}
static int r_trace(unsigned type,void *context,void *statement,void *elapsed)
{
    RTrace *t=context; sqlite3_stmt *s=statement; RSample *r;
    if(type!=SQLITE_TRACE_PROFILE) return 0;
    if(t->count==R_TRACE_MAX) { t->overflow=1; return 0; }
    r=&t->rows[t->count++]; r->sql=sqlite3_expanded_sql(s); r->ns=*(sqlite3_int64 *)elapsed;
    r->vm=sqlite3_stmt_status(s,SQLITE_STMTSTATUS_VM_STEP,0);
    r->fullscan=sqlite3_stmt_status(s,SQLITE_STMTSTATUS_FULLSCAN_STEP,0); r->sorts=sqlite3_stmt_status(s,SQLITE_STMTSTATUS_SORT,0); return 0;
}
static void r_dump(sqlite3 *db,RTrace *t)
{
    unsigned i;
    if(t->overflow) r_die(db,"trace overflow");
    for(i=0;i<t->count;++i) {
        RSample *r=&t->rows[i]; char *plan; sqlite3_stmt *s=NULL; int rc;
        if(!r->sql) r_die(db,"expanded SQL allocation");
        printf("SQL index=%u profile_ns=%lld vm=%d fullscan=%d sorts=%d text=%s\n",i,(long long)r->ns,r->vm,r->fullscan,r->sorts,r->sql);
        plan=sqlite3_mprintf("EXPLAIN QUERY PLAN %s",r->sql); if(!plan) r_die(db,"plan allocation");
        if(sqlite3_prepare_v2(db,plan,-1,&s,NULL)!=SQLITE_OK) r_die(db,"plan prepare");
        while((rc=sqlite3_step(s))==SQLITE_ROW) printf("PLAN index=%u node=%d parent=%d detail=%s\n",i,sqlite3_column_int(s,0),sqlite3_column_int(s,1),experiment_fixture_column_text(s,3));
        if(rc!=SQLITE_DONE) r_die(db,"plan step"); sqlite3_finalize(s); sqlite3_free(plan); sqlite3_free(r->sql);
    }
    memset(t,0,sizeof(*t));
}
static void r_ids_before(sqlite3 *db)
{
    r_sql(db,"CREATE TEMP TABLE before_operation AS SELECT id FROM operation; CREATE TEMP TABLE before_decision AS SELECT id FROM decision;"
        "CREATE TEMP TABLE before_episode AS SELECT id,state,ended,reason FROM episode; CREATE TEMP TABLE before_evaluation AS SELECT id FROM evaluation;");
}
static void r_deltas(sqlite3 *db)
{
    const char *tables[]={"operation","decision","episode","evaluation"}; unsigned i;
    for(i=0;i<4;++i) {
        char sql[256]; sqlite3_stmt *s=NULL; int rc;
        snprintf(sql,sizeof(sql),"SELECT id FROM before_%s EXCEPT SELECT id FROM %s ORDER BY id",tables[i],tables[i]);
        if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK) r_die(db,"delta prepare");
        while((rc=sqlite3_step(s))==SQLITE_ROW) printf("REMOVED table=%s id=%lld\n",tables[i],(long long)sqlite3_column_int64(s,0));
        if(rc!=SQLITE_DONE) r_die(db,"delta step"); sqlite3_finalize(s);
    }
    {
        sqlite3_stmt *s=NULL; int rc;
        if(sqlite3_prepare_v2(db,"SELECT e.id,e.state,e.ended,e.reason FROM episode e JOIN before_episode b ON b.id=e.id WHERE e.state<>b.state OR e.ended<>b.ended OR e.reason<>b.reason ORDER BY e.id",-1,&s,NULL)!=SQLITE_OK) r_die(db,"changed episodes");
        while((rc=sqlite3_step(s))==SQLITE_ROW) printf("CHANGED episode=%lld state=%d ended=%lld reason=%s\n",(long long)sqlite3_column_int64(s,0),sqlite3_column_int(s,1),(long long)sqlite3_column_int64(s,2),experiment_fixture_column_text(s,3));
        if(rc!=SQLITE_DONE) r_die(db,"changed step"); sqlite3_finalize(s);
    }
}
static void r_refresh(Experiment *e,unsigned offset)
{
    ExperimentStats stats; ExperimentEpisode *ep=NULL; ExperimentDecision *p=NULL,*f=NULL;
    size_t ne=0,np=0,nf=0,i; LARGE_INTEGER a,b;
    QueryPerformanceCounter(&a);
    if(!experiment_stats(e,&stats) || !experiment_episodes(e,-1,200,offset,&ep,&ne) ||
        !experiment_decisions(e,EXPERIMENT_PERSONALIZED,200,offset,&p,&np) || !experiment_decisions(e,EXPERIMENT_FIXED,200,offset,&f,&nf)) r_die(experiment_fixture_db(e),experiment_error(e));
    QueryPerformanceCounter(&b);
    printf("REFRESH offset=%u wall_ms=%.3f windows=%llu decisions=%llu episodes=%llu active=%llu recovered=%llu unknown=%llu ratings=%llu gaps=%llu page_events=%zu personal=%zu fixed=%zu stats_hash=%016llx\n",
        offset,r_ms(a,b),(unsigned long long)stats.windows,(unsigned long long)stats.decisions,(unsigned long long)stats.episodes,
        (unsigned long long)stats.active,(unsigned long long)stats.recovered,(unsigned long long)stats.unknown,(unsigned long long)stats.evaluations,(unsigned long long)stats.gaps,
        ne,np,nf,(unsigned long long)r_hash(UINT64_C(1469598103934665603),&stats,sizeof(stats)));
    printf("PAGE_IDS offset=%u kind=episodes ids=",offset); for(i=0;i<ne;++i) printf("%s%lld",i?",":"",(long long)ep[i].id); putchar('\n');
    printf("PAGE_IDS offset=%u kind=personal ids=",offset); for(i=0;i<np;++i) printf("%s%lld",i?",":"",(long long)p[i].id); putchar('\n');
    printf("PAGE_IDS offset=%u kind=fixed ids=",offset); for(i=0;i<nf;++i) printf("%s%lld",i?",":"",(long long)f[i].id); putchar('\n');
    experiment_episodes_free(ep,ne); experiment_decisions_free(p,np); experiment_decisions_free(f,nf);
}
static void r_measure(const char *source,const char *dest,const char *mode)
{
    Experiment *e; uint64_t now,before,after; RTrace trace={0}; LARGE_INTEGER a,b; sqlite3_int64 changes; int ok=1;
    int profiled=getenv("PHASE1_NO_PROFILE")==NULL;
    r_backup_new(source,dest); e=r_open(dest); now=(uint64_t)r_scalar(experiment_fixture_db(e),"SELECT now_utc FROM benchmark_fixture");
    r_check(experiment_fixture_db(e)); before=r_inventory(experiment_fixture_db(e),"before"); r_ids_before(experiment_fixture_db(e)); r_sizes(experiment_fixture_db(e),dest,"before");
    printf("MEASURE mode=%s cache=warm-after-integrity-fingerprint-and-id-snapshot profile=%s source=standalone-synthetic\n",mode,profiled?"on":"off");
    changes=sqlite3_total_changes64(experiment_fixture_db(e));
    if(profiled) sqlite3_trace_v2(experiment_fixture_db(e),SQLITE_TRACE_PROFILE,r_trace,&trace);
    QueryPerformanceCounter(&a);
    if(!strcmp(mode,"stats")) { r_refresh(e,0); r_refresh(e,0); r_refresh(e,20000); }
    else {
        ok=experiment_fixture_execute(e,"BEGIN IMMEDIATE");
        if(ok && !strcmp(mode,"trim-retained")) ok=r_trim(e,now);
        else if(ok && !strcmp(mode,"trim-expired")) ok=r_trim(e,now+7*DAY);
        else if(ok && !strcmp(mode,"gap-all")) ok=experiment_fixture_gap(e,now,0,0,0,ALL_METRICS,"synthetic perf full reset");
        else if(ok && !strcmp(mode,"gap-one")) {
            sqlite3_stmt *s=NULL; uint64_t session,creation; uint32_t pid;
            if(!experiment_fixture_prepare(e,"SELECT session,pid,creation FROM episode WHERE state=0 ORDER BY id DESC LIMIT 1",&s) || sqlite3_step(s)!=SQLITE_ROW) r_die(experiment_fixture_db(e),"active gap target");
            session=(uint64_t)sqlite3_column_int64(s,0); pid=(uint32_t)sqlite3_column_int64(s,1); creation=(uint64_t)sqlite3_column_int64(s,2); sqlite3_finalize(s);
            ok=experiment_fixture_gap(e,now,session,pid,creation,1u,"synthetic perf metric gap");
        } else if(ok) r_die(experiment_fixture_db(e),"unknown mode");
        if(ok) ok=experiment_fixture_execute(e,"COMMIT");
    }
    QueryPerformanceCounter(&b); sqlite3_trace_v2(experiment_fixture_db(e),0,NULL,NULL);
    if(!ok) { fprintf(stderr,"FAIL product operation: %s\n",experiment_error(e)); exit(1); }
    printf("TOTAL mode=%s wall_ms=%.3f changes=%lld\n",mode,r_ms(a,b),(long long)(sqlite3_total_changes64(experiment_fixture_db(e))-changes));
    if(profiled) r_dump(experiment_fixture_db(e),&trace);
    r_sizes(experiment_fixture_db(e),dest,"after-operation"); after=r_inventory(experiment_fixture_db(e),"after"); r_deltas(experiment_fixture_db(e)); r_check(experiment_fixture_db(e));
    if(!strcmp(mode,"stats") && before!=after) r_die(experiment_fixture_db(e),"read-only result mutated fixture");
    printf("RESULT mode=%s before_hash=%016llx after_hash=%016llx\n",mode,(unsigned long long)before,(unsigned long long)after);
    experiment_close(e); r_sizes(NULL,dest,"closed");
}
int main(int argc,char **argv)
{
    printf("VERSION SQLite=%s source_id=%s workload=synthetic-not-user-evaluation\n",sqlite3_libversion(),sqlite3_sourceid());
    if(argc==5 && !strcmp(argv[1],"seed")) {
        char *end=NULL; unsigned long n=strtoul(argv[4],&end,10); if(!end || *end || n>60000) r_die(NULL,"invalid windows");
        r_seed(argv[2],argv[3],(unsigned)n);
    } else if(argc==5 && !strcmp(argv[1],"measure")) r_measure(argv[2],argv[3],argv[4]);
    else { fprintf(stderr,"usage: retained_probe seed SOURCE NEW WINDOWS | measure FIXTURE NEW stats|trim-retained|trim-expired|gap-one|gap-all\n"); return 2; }
    return 0;
}
