#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "monitor_internal.h"

/* Read-only SQL candidate experiment. Not product code. Both algorithms select
 * bounded per-minute representatives with the same six predicates/parameters. */
static const char original_sql[] =
    "SELECT m.value,w.end_utc,w.id FROM aggregate_window w JOIN aggregate_metric m ON m.window_id=w.id AND m.metric=?1 AND m.eligible=1 AND m.value IS NOT NULL AND m.value>=0 AND m.value<=1.7976931348623157e308 WHERE w.program_id=?2 AND w.version=1 AND w.end_utc>=?3 AND w.end_utc<?4 AND w.end_utc<?5 AND w.id<=?6 ORDER BY w.end_utc DESC,w.id DESC LIMIT 1";
/* ?4 is a minute-aligned UTC date upper bound; it is not an arbitrary partial
 * minute endpoint. ?5 retains the exact 100ns start boundary independently. */
static const char candidate_sql[] =
    "WITH RECURSIVE picks(id,end_utc,value,n) AS ("
    "SELECT NULL,?4,NULL,0 UNION ALL "
    "SELECT w.id,w.end_utc,(SELECT value FROM aggregate_metric WHERE window_id=w.id AND metric=?1),p.n+1 FROM picks p JOIN aggregate_window w ON w.id=("
    "SELECT x.id FROM aggregate_window x JOIN aggregate_metric m ON m.window_id=x.id AND m.metric=?1 AND m.eligible=1 AND m.value IS NOT NULL AND m.value>=0 AND m.value<=1.7976931348623157e308 "
    "WHERE x.program_id=?2 AND x.version=1 AND x.end_utc>=?3 AND x.end_utc<min((p.end_utc/600000000)*600000000,?5) AND x.id<=?6 ORDER BY x.end_utc DESC,x.id DESC LIMIT 1) WHERE p.n<64) "
    "SELECT value,end_utc,id FROM picks WHERE n>0";
typedef struct Point { double value; sqlite3_int64 end, id; unsigned metric; } Point;
static int compare(const void *a, const void *b)
{
    const Point *x=a, *y=b;
    if (x->metric != y->metric) return (x->metric>y->metric)-(x->metric<y->metric);
    if (x->end != y->end) return (x->end<y->end)-(x->end>y->end);
    return (x->id<y->id)-(x->id>y->id);
}
static unsigned collect(sqlite3_stmt *s, int recursive, sqlite3_int64 program,
    sqlite3_int64 start, sqlite3_int64 cutoff, Point out[3584])
{
    unsigned age, metric, n=0;
    sqlite3_int64 date=start/(sqlite3_int64)MONITOR_DAY_TICKS;
    for (metric=0;metric<4;++metric) for (age=0;age<14;++age) {
        sqlite3_int64 lower=(date-age)*(sqlite3_int64)MONITOR_DAY_TICKS, upper=lower+(sqlite3_int64)MONITOR_DAY_TICKS;
        unsigned day_count=0;
        do {
            int rc;
            sqlite3_bind_int(s,1,(int)metric); sqlite3_bind_int64(s,2,program);
            sqlite3_bind_int64(s,3,lower); sqlite3_bind_int64(s,4,upper);
            sqlite3_bind_int64(s,5,start); sqlite3_bind_int64(s,6,cutoff);
            while ((rc=sqlite3_step(s))==SQLITE_ROW) {
                if (n>=3584) { fprintf(stderr,"point overflow\n"); exit(1); }
                out[n].value=sqlite3_column_double(s,0); out[n].end=sqlite3_column_int64(s,1); out[n].id=sqlite3_column_int64(s,2); out[n].metric=metric;
                upper=out[n].end/600000000*600000000; ++n; ++day_count;
                if (!recursive) break;
            }
            if (rc!=SQLITE_DONE && rc!=SQLITE_ROW) { fprintf(stderr,"step error %d\n",rc); exit(1); }
            sqlite3_reset(s); sqlite3_clear_bindings(s);
            if (recursive || rc==SQLITE_DONE) break;
        } while(day_count<64 && upper>lower);
    }
    /* Sort each metric block independently; recursive output order is not a
     * contract. Point sets/ids/values must be identical, including ties. */
    return n;
}
static sqlite3_int64 scalar(sqlite3 *db,const char *sql)
{
    sqlite3_stmt *s=NULL;
    sqlite3_int64 value;
    if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK || sqlite3_step(s)!=SQLITE_ROW) {
        fprintf(stderr,"scalar error %s\n",sqlite3_errmsg(db)); exit(1);
    }
    value=sqlite3_column_int64(s,0); sqlite3_finalize(s); return value;
}
static int cache_status(sqlite3 *db,int op,int reset)
{
    int current=0,highwater=0;
    if(sqlite3_db_status(db,op,&current,&highwater,reset)!=SQLITE_OK) exit(1);
    return current;
}
static uint64_t signature(Point *points,unsigned n)
{
    uint64_t h=UINT64_C(1469598103934665603);
    unsigned i;
    qsort(points,n,sizeof(*points),compare);
    for(i=0;i<n;++i) {
        uint64_t bits;
        memcpy(&bits,&points[i].value,sizeof(bits));
        h=(h^points[i].metric)*UINT64_C(1099511628211);
        h=(h^(uint64_t)points[i].id)*UINT64_C(1099511628211);
        h=(h^(uint64_t)points[i].end)*UINT64_C(1099511628211);
        h=(h^bits)*UINT64_C(1099511628211);
    }
    return h;
}
static int cache_benchmark(const char *file,sqlite3_int64 start)
{
    static const int budgets[5]={0,-16384,0,-32768,0};
    uint64_t expected[100]={0};
    unsigned configuration;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    printf("CACHE_TRIAL read-only CTE, fresh SQLite connection per configuration; one read transaction; order default/16MiB/default/32MiB/default; OS cache not flushed\n");
    for(configuration=0;configuration<5;++configuration) {
        sqlite3 *db=NULL;
        sqlite3_stmt *s=NULL,*programs=NULL;
        sqlite3_int64 ids[50],cutoff;
        unsigned path_count=0,trace_count,i;
        double total=0,steady=0,minimum=1e100,maximum=0;
        sqlite3_int64 hits=0,misses=0;
        int used_max=0;
        char pragma[64];
        if(sqlite3_open_v2(file,&db,SQLITE_OPEN_READONLY,NULL)!=SQLITE_OK) return 1;
        if(scalar(db,"SELECT synthetic FROM metadata")!=1) return 2;
        if(budgets[configuration]) {
            snprintf(pragma,sizeof(pragma),"PRAGMA cache_size=%d",budgets[configuration]);
            if(sqlite3_exec(db,pragma,NULL,NULL,NULL)!=SQLITE_OK) return 1;
        }
        printf("CACHE_CONFIG sequence=%u requested=%d actual_cache_size=%lld page_size=%lld\n",configuration,
            budgets[configuration],(long long)scalar(db,"PRAGMA cache_size"),(long long)scalar(db,"PRAGMA page_size"));
        cutoff=scalar(db,"SELECT max(id) FROM aggregate_window");
        if(sqlite3_prepare_v2(db,"SELECT id FROM program ORDER BY id LIMIT 50",-1,&programs,NULL)!=SQLITE_OK) return 1;
        while(sqlite3_step(programs)==SQLITE_ROW) ids[path_count++]=sqlite3_column_int64(programs,0);
        sqlite3_finalize(programs);
        if(!path_count || sqlite3_prepare_v2(db,candidate_sql,-1,&s,NULL)!=SQLITE_OK) return 1;
        trace_count=path_count==1?20:path_count*2;
        if(sqlite3_exec(db,"BEGIN",NULL,NULL,NULL)!=SQLITE_OK) return 1;
        for(i=0;i<trace_count;++i) {
            Point points[3584];
            unsigned n;
            LARGE_INTEGER a,b;
            double elapsed;
            int hit,miss,used;
            uint64_t hash;
            cache_status(db,SQLITE_DBSTATUS_CACHE_HIT,1); cache_status(db,SQLITE_DBSTATUS_CACHE_MISS,1);
            QueryPerformanceCounter(&a); n=collect(s,1,ids[i%path_count],start,cutoff,points); QueryPerformanceCounter(&b);
            elapsed=(double)(b.QuadPart-a.QuadPart)*1000/(double)frequency.QuadPart;
            hit=cache_status(db,SQLITE_DBSTATUS_CACHE_HIT,0); miss=cache_status(db,SQLITE_DBSTATUS_CACHE_MISS,0);
            used=cache_status(db,SQLITE_DBSTATUS_CACHE_USED,0);
            if(used>used_max) used_max=used;
            total+=elapsed; hits+=hit; misses+=miss;
            if(i) { steady+=elapsed; if(elapsed<minimum) minimum=elapsed; if(elapsed>maximum) maximum=elapsed; }
            hash=signature(points,n);
            if(n!=3584) return 1;
            if(!configuration) expected[i]=hash;
            else if(expected[i]!=hash) { fprintf(stderr,"cache changed point set at %u\n",i); return 1; }
            if(i==0 || i==trace_count-1) printf("CACHE_SAMPLE sequence=%u index=%u program=%lld points=%u ms=%.3f cache_hits=%d cache_misses=%d cache_used_bytes=%d\n",
                configuration,i,(long long)ids[i%path_count],n,elapsed,hit,miss,used);
        }
        printf("CACHE_SUMMARY sequence=%u requested=%d paths=%u calls=%u mean_ms=%.3f later_calls_mean_ms=%.3f later_min_ms=%.3f later_max_ms=%.3f total_hits=%lld total_misses=%lld max_cache_used_bytes=%d exact_point_hashes_match=1\n",
            configuration,budgets[configuration],path_count,trace_count,total/trace_count,steady/(trace_count-1),minimum,maximum,
            (long long)hits,(long long)misses,used_max);
        sqlite3_finalize(s); sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL); sqlite3_close(db);
    }
    return 0;
}
int main(int argc,char **argv)
{
    sqlite3 *db=NULL;
    sqlite3_stmt *old=NULL,*candidate=NULL,*meta=NULL,*plan=NULL;
    Point expected[3584],actual[3584];
    unsigned nold=0,nnew,i;
    int run;
    sqlite3_int64 start,program,cutoff;
    LARGE_INTEGER a,b,f;
    char explain[4096];
    if(argc!=3 && argc!=4) { printf("benchmark_monitor_query OWN_SYNTHETIC_DB EXACT_START_TICKS [--cache-benchmark]\n"); return 2; }
    start=(sqlite3_int64)_strtoui64(argv[2],NULL,10);
    if(argc==4) return !strcmp(argv[3],"--cache-benchmark") ? cache_benchmark(argv[1],start) : 2;
    if(sqlite3_open_v2(argv[1],&db,SQLITE_OPEN_READONLY,NULL)!=SQLITE_OK) return 1;
    if(sqlite3_prepare_v2(db,"SELECT synthetic FROM metadata",-1,&meta,NULL)!=SQLITE_OK ||
        sqlite3_step(meta)!=SQLITE_ROW || sqlite3_column_int(meta,0)!=1) return 2;
    sqlite3_finalize(meta);
    sqlite3_prepare_v2(db,"SELECT min(id) FROM program",-1,&meta,NULL); sqlite3_step(meta);
    program=sqlite3_column_int64(meta,0); sqlite3_finalize(meta);
    sqlite3_prepare_v2(db,"SELECT max(id) FROM aggregate_window",-1,&meta,NULL); sqlite3_step(meta);
    cutoff=sqlite3_column_int64(meta,0); sqlite3_finalize(meta);
    if(sqlite3_prepare_v2(db,original_sql,-1,&old,NULL)!=SQLITE_OK ||
        sqlite3_prepare_v2(db,candidate_sql,-1,&candidate,NULL)!=SQLITE_OK) {
        fprintf(stderr,"prepare error %s\n",sqlite3_errmsg(db)); return 1;
    }
    snprintf(explain,sizeof(explain),"EXPLAIN QUERY PLAN %s",candidate_sql);
    if(sqlite3_prepare_v2(db,explain,-1,&plan,NULL)!=SQLITE_OK) return 1;
    while(sqlite3_step(plan)==SQLITE_ROW) printf("QUERY_PLAN %s\n",sqlite3_column_text(plan,3));
    sqlite3_finalize(plan);
    if(sqlite3_exec(db,"BEGIN",NULL,NULL,NULL)!=SQLITE_OK) return 1;
    QueryPerformanceFrequency(&f);
    for(run=0;run<4;++run) {
        int recursive=run%2;
        sqlite3_stmt *s=recursive ? candidate : old;
        unsigned n;
        sqlite3_stmt_status(s,SQLITE_STMTSTATUS_RUN,1); sqlite3_stmt_status(s,SQLITE_STMTSTATUS_VM_STEP,1);
        sqlite3_stmt_status(s,SQLITE_STMTSTATUS_FULLSCAN_STEP,1); sqlite3_stmt_status(s,SQLITE_STMTSTATUS_SORT,1);
        QueryPerformanceCounter(&a); n=collect(s,recursive,program,start,cutoff,recursive?actual:expected); QueryPerformanceCounter(&b);
        printf("QUERY_CANDIDATE run=%d algorithm=%s points=%u ms=%.3f query_runs=%d VM_steps=%d fullscan=%d sorts=%d\n",run,
            recursive?"bounded-recursive-CTE":"original-C-loop",n,(double)(b.QuadPart-a.QuadPart)*1000/(double)f.QuadPart,
            sqlite3_stmt_status(s,SQLITE_STMTSTATUS_RUN,0),sqlite3_stmt_status(s,SQLITE_STMTSTATUS_VM_STEP,0),
            sqlite3_stmt_status(s,SQLITE_STMTSTATUS_FULLSCAN_STEP,0),sqlite3_stmt_status(s,SQLITE_STMTSTATUS_SORT,0));
        if(!recursive) {
            nold=n; qsort(expected,n,sizeof(*expected),compare);
        } else {
            nnew=n; qsort(actual,n,sizeof(*actual),compare);
            if(nnew!=nold) return 1;
            for(i=0;i<nold;++i) if(expected[i].metric!=actual[i].metric || expected[i].id!=actual[i].id || expected[i].end!=actual[i].end || expected[i].value!=actual[i].value) return 1;
        }
    }
    printf("QUERY_CANDIDATE_EQUALITY exact metric/IDs/end/value sets preserved; experiment does not modify product source; DB read-only; program=%lld start=%lld cutoff=%lld\n",
        (long long)program,(long long)start,(long long)cutoff);
    sqlite3_finalize(old); sqlite3_finalize(candidate); sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL); sqlite3_close(db); return 0;
}
