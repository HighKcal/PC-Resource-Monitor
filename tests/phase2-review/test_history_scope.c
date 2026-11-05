/* Independent direct scope checks. Expected representatives are literal fixture
 * outcomes, not values calculated by product Baseline or its recursive SQL. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "monitor.h"
#include "monitor_store.h"
#include "baseline_history.h"
#include "monitor_fixture.h"
#include "sqlite3.h"

#define REVIEW_TICKS UINT64_C(10000000)
#define REVIEW_DAY (UINT64_C(86400) * REVIEW_TICKS)
#define REVIEW_MINUTE (UINT64_C(60) * REVIEW_TICKS)
static unsigned checks, failures, serial, allocations, oom_hits;
static int fail_allocation;
static const char *directory;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line=%d %s\n", __LINE__, #x); } } while (0)
void *review_history_calloc(size_t n, size_t size)
{
    ++allocations;
    if (fail_allocation) { fail_allocation = 0; ++oom_hits; return NULL; }
    return calloc(n, size);
}
typedef struct Fixture { MonitorEngine *engine; MonitorStore *store; sqlite3 *db; uint64_t day, start; } Fixture;
static void sql(Fixture *f, const char *query) { CHECK(sqlite3_exec(f->db, query, NULL, NULL, NULL) == SQLITE_OK); }
static void initialize(Fixture *f)
{
    ResourceBatch batch = {0};
    MonitorSnapshot *snapshot = NULL;
    char file[1024], error[256];
    memset(f, 0, sizeof(*f));
    snprintf(file, sizeof(file), "%s/history-%u.sqlite", directory, ++serial);
    CHECK(GetFileAttributesA(file) == INVALID_FILE_ATTRIBUTES);
    CHECK(monitor_open(file, 1, &f->engine, error, sizeof(error)));
    if (!f->engine) { printf("OPEN %s\n", error); exit(2); }
    f->store = monitor_fixture_store(f->engine); f->db = monitor_fixture_db(f->engine);
    f->day = UINT64_C(134058672000000000) / REVIEW_DAY * REVIEW_DAY;
    f->start = f->day + 30 * REVIEW_TICKS;
    batch.started_at_utc_100ns = batch.finished_at_utc_100ns = f->start;
    batch.logical_processor_count = 8;
    CHECK(monitor_ingest(f->engine, &batch, NULL, &snapshot)); monitor_snapshot_free(snapshot);
    sql(f, "INSERT INTO program(id,path,name,first_seen,last_seen) VALUES(1,'SYNTHETIC:\\history.exe','history.exe',1,1)");
}
static void seed(Fixture *f, uint64_t end, double value)
{
    sqlite3_stmt *s = NULL;
    int64_t id;
    CHECK(sqlite3_prepare_v2(f->db, "INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) VALUES((SELECT min(id) FROM collection_run),1,5555,1,?,?,60,1)", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, (sqlite3_int64)(end - 60 * REVIEW_TICKS)); sqlite3_bind_int64(s, 2, (sqlite3_int64)end);
    CHECK(sqlite3_step(s) == SQLITE_DONE); id = sqlite3_last_insert_rowid(f->db); sqlite3_finalize(s); s = NULL;
    CHECK(sqlite3_prepare_v2(f->db, "INSERT INTO aggregate_metric VALUES(?,0,?,60,20,1,2,'independent scope fixture',0,0,0,NULL,NULL,NULL,0)", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, id); sqlite3_bind_double(s, 2, value); CHECK(sqlite3_step(s) == SQLITE_DONE); sqlite3_finalize(s);
}
typedef struct Trace { unsigned queries, rows; int stop_after_row, armed, interrupted; } Trace;
static int trace(unsigned event, void *context, void *statement, void *detail)
{
    Trace *p = context;
    const char *query = sqlite3_sql((sqlite3_stmt *)statement);
    (void)detail;
    if (!query || !strstr(query, "WITH RECURSIVE picks")) return 0;
    if (event == SQLITE_TRACE_STMT) ++p->queries;
    if (event == SQLITE_TRACE_ROW) {
        ++p->rows;
        if (p->stop_after_row && !p->interrupted) p->armed = 1;
    }
    return 0;
}
static int progress(void *context)
{
    Trace *p = context;
    if (p->armed && !p->interrupted) { p->armed = 0; p->interrupted = 1; return 1; }
    return 0;
}
static void select_one(BaselineHistoryScope *scope, uint64_t lower, uint64_t upper, uint64_t start, double expected)
{
    double values[64]; unsigned count = 999;
    CHECK(baseline_history_select_day(scope, 1, MONITOR_CPU, lower, upper, start, values, &count));
    CHECK(count == 1); CHECK(values[0] == expected);
}
static void start_scope(Fixture *f, BaselineHistoryScope *scope, int64_t *cutoff)
{
    CHECK(monitor_store_begin(f->store, cutoff));
    baseline_history_begin(scope, f->store, *cutoff);
}
static void end_scope(Fixture *f, BaselineHistoryScope *scope)
{
    baseline_history_clear(scope);
    CHECK(monitor_store_commit(f->store));
    CHECK(sqlite3_get_autocommit(f->db));
    CHECK(!sqlite3_stmt_busy(monitor_fixture_baseline_statement(f->engine)));
}
static void exact_start_and_cutoff(void)
{
    Fixture f;
    BaselineHistoryScope scope = {0};
    Trace probe = {0};
    int64_t cutoff;
    unsigned before;
    initialize(&f);
    sql(&f, "BEGIN IMMEDIATE");
    seed(&f, f.start - 1, 20); seed(&f, f.start, 30); seed(&f, f.start + 1, 40);
    seed(&f, f.day - REVIEW_DAY + 12 * REVIEW_MINUTE, 10);
    seed(&f, f.day - REVIEW_DAY + 12 * REVIEW_MINUTE, 11); /* Same end: greatest eligible ID. */
    sql(&f, "COMMIT");
    CHECK(sqlite3_trace_v2(f.db, SQLITE_TRACE_STMT | SQLITE_TRACE_ROW, trace, &probe) == SQLITE_OK);
    start_scope(&f, &scope, &cutoff);
    seed(&f, f.start - 1, 999); /* Same transaction, newer than fixed cutoff. */
    select_one(&scope, f.day, f.day + REVIEW_DAY, f.start, 20);
    select_one(&scope, f.day, f.day + REVIEW_DAY, f.start + 1, 30);
    select_one(&scope, f.day, f.day + REVIEW_DAY, f.start + 2, 40);
    before = probe.queries;
    select_one(&scope, f.day, f.day + REVIEW_DAY, f.start, 20);
    CHECK(probe.queries == before); /* Exact same key reused. */
    select_one(&scope, f.day - REVIEW_DAY, f.day, f.start, 11);
    before = probe.queries;
    select_one(&scope, f.day - REVIEW_DAY, f.day, f.start + 2, 11);
    CHECK(probe.queries == before); /* Old full date shares min(upper,start). */
    end_scope(&f, &scope);
    start_scope(&f, &scope, &cutoff);
    select_one(&scope, f.day, f.day + REVIEW_DAY, f.start, 999);
    end_scope(&f, &scope);
    CHECK(sqlite3_trace_v2(f.db, 0, NULL, NULL) == SQLITE_OK);
    monitor_close(f.engine);
    printf("HISTORY_BOUNDARY exact_start=ok day_upper=ok cutoff_excludes_new_id=ok next_scope_refresh=ok\n");
}
static void partial_and_oom(void)
{
    Fixture f;
    BaselineHistoryScope scope = {0};
    Trace probe = {0};
    int64_t cutoff;
    double values[64];
    unsigned i, count, before_queries, before_allocations, before_oom, partial_rows;
    initialize(&f);
    sql(&f, "BEGIN IMMEDIATE");
    for (i = 1; i <= 70; ++i) seed(&f, f.day - REVIEW_DAY + i * REVIEW_MINUTE, (double)i);
    sql(&f, "COMMIT");
    start_scope(&f, &scope, &cutoff);
    CHECK(sqlite3_trace_v2(f.db, SQLITE_TRACE_STMT | SQLITE_TRACE_ROW, trace, &probe) == SQLITE_OK);
    sqlite3_progress_handler(f.db, 1, progress, &probe); probe.stop_after_row = 1;
    count = 999;
    for (i = 0; i < 64; ++i) values[i] = -777;
    CHECK(!baseline_history_select_day(&scope, 1, MONITOR_CPU, f.day - REVIEW_DAY, f.day, f.start, values, &count));
    CHECK(count == 0);
    for (i = 0; i < 64; ++i) CHECK(values[i] == -777);
    CHECK(probe.rows >= 1 && probe.interrupted);
    partial_rows = probe.rows;
    CHECK(!sqlite3_stmt_busy(monitor_fixture_baseline_statement(f.engine)));
    probe.stop_after_row = 0; sqlite3_progress_handler(f.db, 0, NULL, NULL);
    before_queries = probe.queries;
    CHECK(baseline_history_select_day(&scope, 1, MONITOR_CPU, f.day - REVIEW_DAY, f.day, f.start, values, &count));
    CHECK(probe.queries == before_queries + 1); CHECK(count == 64);
    for (i = 0; i < count; ++i) CHECK(values[i] == (double)(70 - i));
    before_queries = probe.queries;
    CHECK(baseline_history_select_day(&scope, 1, MONITOR_CPU, f.day - REVIEW_DAY, f.day, f.start, values, &count));
    CHECK(probe.queries == before_queries && count == 64);
    end_scope(&f, &scope);
    start_scope(&f, &scope, &cutoff);
    before_allocations = allocations; before_oom = oom_hits; fail_allocation = 1;
    for (i = 0; i < 3; ++i) {
        before_queries = probe.queries;
        CHECK(baseline_history_select_day(&scope, 1, MONITOR_CPU, f.day - REVIEW_DAY, f.day, f.start, values, &count));
        CHECK(count == 64 && values[0] == 70 && values[63] == 7);
        CHECK(probe.queries == before_queries + 1);
    }
    CHECK(allocations == before_allocations + 1 && oom_hits == before_oom + 1);
    end_scope(&f, &scope);
    CHECK(sqlite3_trace_v2(f.db, 0, NULL, NULL) == SQLITE_OK); monitor_close(f.engine);
    printf("HISTORY_FAILURE partial_rows=%u interrupted=1 retry_full64=1 cached_after_success=1 oom_once=1 fallback_queries=3\n", partial_rows);
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    directory = argv[1]; exact_start_and_cutoff(); partial_and_oom();
    printf("HISTORY_REVIEW checks=%u failures=%u allocations=%u oom_hits=%u\n", checks, failures, allocations, oom_hits);
    return failures ? 1 : 0;
}
