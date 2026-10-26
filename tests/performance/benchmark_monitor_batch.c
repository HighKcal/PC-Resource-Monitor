#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "monitor_internal.h"

/* Synthetic mature-history workload. Real waits/QPC and optional self-process
 * collector observations are explicitly separate from represented history. */
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line=%d %s\n", __LINE__, #x); fflush(stdout); } } while (0)
typedef struct Workload {
    MonitorEngine *engine;
    ResourceCollector *probe;
    ResourceData *rows;
    const char **paths;
    char (*path_text)[128];
    sqlite3_int64 *programs;
    ResourceBatch batch;
    MonitorSnapshot *snapshot;
    uint64_t utc;
    unsigned instances, groups, density, days;
    char filename[MAX_PATH], kind[16];
} Workload;
static double milliseconds(LARGE_INTEGER a, LARGE_INTEGER b)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f); return (double)(b.QuadPart-a.QuadPart)*1000/(double)f.QuadPart;
}
static int execute(Workload *w, const char *sql)
{
    char *error = NULL;
    int rc = sqlite3_exec(w->engine->db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) printf("SQL_ERROR %s\n", error ? error : "unknown");
    sqlite3_free(error); CHECK(rc == SQLITE_OK); return rc == SQLITE_OK;
}
static void data_values(ResourceData *r, unsigned group)
{
    r->valid_fields = RESOURCE_VALID_NAME | RESOURCE_VALID_IDENTITY | RESOURCE_VALID_CPU |
        RESOURCE_VALID_MEMORY | RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE |
        RESOURCE_VALID_OBSERVATION_INTERVAL;
    r->cpu_percent = 50; /* One attention window; tests reset of nonzero streak. */
    r->working_set_bytes = UINT64_C(67108864) + (uint64_t)group * UINT64_C(1048576);
    r->io_read_bytes_per_second = r->io_write_bytes_per_second = 0;
    r->cpu_status = r->io_read_status = r->io_write_status = RESOURCE_DELTA_VALID;
    r->foreground = RESOURCE_FOREGROUND_UNKNOWN;
}
static int ingest(Workload *w, double interval, int gap)
{
    size_t i;
    w->utc += (uint64_t)(interval * (double)MONITOR_TICKS_PER_SECOND);
    for (i = 0; i < w->batch.count; ++i) {
        ResourceData *r = &w->rows[i];
        data_values(r, (unsigned)i % w->groups);
        /* ~10ms spread at 500 rows: exact p.start differs, no rounding. */
        r->measured_at_utc_100ns = w->utc + (uint64_t)i * 200;
        r->observation_interval_seconds = r->cpu_interval_seconds = r->io_interval_seconds = interval;
        if (gap) {
            r->valid_fields &= ~(unsigned)(RESOURCE_VALID_CPU | RESOURCE_VALID_IO_READ |
                RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL);
            r->cpu_status = r->io_read_status = r->io_write_status = RESOURCE_DELTA_LONG_GAP;
        }
    }
    w->batch.started_at_utc_100ns = w->utc;
    w->batch.finished_at_utc_100ns = w->utc + (uint64_t)w->batch.count * 200;
    monitor_snapshot_free(w->snapshot); w->snapshot = NULL;
    if (!monitor_ingest(w->engine, &w->batch, w->paths, &w->snapshot)) {
        printf("INGEST_ERROR %s\n", monitor_error(w->engine)); CHECK(0); return 0;
    }
    return 1;
}
static void probe(Workload *w, const char *stage, int expected_gap)
{
    ResourceBatch batch = {0};
    uint32_t error = 0;
    size_t i;
    CHECK(resource_collector_collect(w->probe, &batch, &error) == RESOURCE_OK);
    for (i = 0; i < batch.count; ++i) if (batch.items[i].pid == GetCurrentProcessId()) {
        ResourceData *r = &batch.items[i];
        printf("REAL_SELF_PROBE case=%s stage=%s valid=0x%x observation_s=%.6f CPU_interval_s=%.6f CPU_status=%s read_status=%s RAM_valid=%d\n",
            w->kind, stage, r->valid_fields, r->observation_interval_seconds, r->cpu_interval_seconds,
            resource_delta_status_string(r->cpu_status), resource_delta_status_string(r->io_read_status),
            !!(r->valid_fields & RESOURCE_VALID_MEMORY));
        if (expected_gap >= 0) {
            CHECK((r->cpu_status == RESOURCE_DELTA_LONG_GAP) == expected_gap);
            CHECK((r->io_read_status == RESOURCE_DELTA_LONG_GAP) == expected_gap);
            CHECK(!!(r->valid_fields & RESOURCE_VALID_OBSERVATION_INTERVAL) == !expected_gap);
        }
        break;
    }
    CHECK(i < batch.count); resource_batch_free(&batch); fflush(stdout);
}
static int initialize(Workload *w, const char *kind, unsigned count, unsigned days, const char *phase)
{
    unsigned i;
    char error[256];
    uint32_t os_error = 0;
    memset(w, 0, sizeof(*w)); snprintf(w->kind, sizeof(w->kind), "%s", kind);
    w->instances = count; w->days = days;
    w->groups = !strcmp(kind, "same") ? 1 : !strcmp(kind, "mixed") ? count / 10 : count;
    if (!w->groups) w->groups = 1;
    w->density = !strcmp(kind, "same") ? 32 : !strcmp(kind, "mixed") ? 8 : 1;
    snprintf(w->filename, sizeof(w->filename), "build-monitor/mature-%s-%s-%lu.sqlite", phase, kind, (unsigned long)GetCurrentProcessId());
    CHECK(GetFileAttributesA(w->filename) == INVALID_FILE_ATTRIBUTES);
    if (!monitor_open(w->filename, 1, &w->engine, error, sizeof(error))) {
        printf("OPEN_ERROR %s\n", error); return 0;
    }
    w->rows = calloc(count, sizeof(*w->rows)); w->paths = calloc(count, sizeof(*w->paths));
    w->path_text = calloc(w->groups, sizeof(*w->path_text)); w->programs = calloc(w->groups, sizeof(*w->programs));
    if (!w->rows || !w->paths || !w->path_text || !w->programs) return 0;
    w->utc = monitor_utc_now() / MONITOR_DAY_TICKS * MONITOR_DAY_TICKS + 12 * 3600 * MONITOR_TICKS_PER_SECOND;
    w->batch.items = w->rows; w->batch.count = count; w->batch.logical_processor_count = 12;
    for (i = 0; i < w->groups; ++i) snprintf(w->path_text[i], sizeof(w->path_text[i]), "SYNTHETIC:\\mature-%s\\program-%u.exe", kind, i);
    for (i = 0; i < count; ++i) {
        w->paths[i] = w->path_text[i % w->groups]; w->rows[i].pid = 10000 + i;
        w->rows[i].creation_time_utc_100ns = w->utc - 20 * MONITOR_DAY_TICKS + i;
        w->rows[i].process_name_utf8 = "SYNTHETIC mature.exe";
    }
    if (!ingest(w, 0, 0)) return 0;
    for (i = 0; i < w->groups; ++i) w->programs[i] = w->snapshot->rows[i].program_id;
    CHECK(resource_collector_create(NULL, &w->probe, &os_error) == RESOURCE_OK);
    printf("CASE_SETUP SYNTHETIC case=%s instances=%u paths=%u history_days=%u minutes_per_date=64 historical_instances_per_path_minute=%u UTC_base=%llu row_UTC_step_ticks=200 file=%s\n",
        kind, count, w->groups, days, w->density, (unsigned long long)w->utc, w->filename);
    {
        sqlite3_stmt *configuration = NULL;
        CHECK(sqlite3_prepare_v2(w->engine->db,"PRAGMA main.cache_size",-1,&configuration,NULL)==SQLITE_OK);
        CHECK(sqlite3_step(configuration)==SQLITE_ROW);
        printf("PAGE_CACHE_CONFIG case=%s main_cache_size=%d (negative value is approximate KiB target, not process memory limit)\n",
            kind,sqlite3_column_int(configuration,0));
        sqlite3_finalize(configuration);
    }
    fflush(stdout); return 1;
}
static int seed(Workload *w)
{
    sqlite3_stmt *window = NULL, *metric = NULL;
    unsigned group, day, minute, process, m;
    LARGE_INTEGER a, b;
    sqlite3_int64 count = 0;
    QueryPerformanceCounter(&a);
    if (!execute(w, "BEGIN IMMEDIATE")) return 0;
    CHECK(sqlite3_prepare_v2(w->engine->db,
        "INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) VALUES(?,?,?,?,?,?,60,1)",
        -1, &window, NULL) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(w->engine->db,
        "INSERT INTO aggregate_metric VALUES(?,?,?,60,20,1,2,'SYNTHETIC mature seed',896,14,896,0,0,0,0)",
        -1, &metric, NULL) == SQLITE_OK);
    for (group = 0; group < w->groups; ++group) for (day = 0; day < w->days; ++day)
        for (minute = 1; minute <= 64; ++minute) for (process = 0; process < w->density; ++process) {
            uint64_t end = (w->utc / MONITOR_DAY_TICKS - day) * MONITOR_DAY_TICKS +
                ((uint64_t)minute * 60 + process) * MONITOR_TICKS_PER_SECOND;
            sqlite3_int64 id;
            sqlite3_bind_int64(window, 1, 1); sqlite3_bind_int64(window, 2, w->programs[group]);
            sqlite3_bind_int64(window, 3, 50000 + process);
            sqlite3_bind_int64(window, 4, (sqlite3_int64)(w->utc - 20 * MONITOR_DAY_TICKS + process));
            sqlite3_bind_int64(window, 5, (sqlite3_int64)(end - 60 * MONITOR_TICKS_PER_SECOND));
            sqlite3_bind_int64(window, 6, (sqlite3_int64)end);
            CHECK(sqlite3_step(window) == SQLITE_DONE); sqlite3_reset(window); id = sqlite3_last_insert_rowid(w->engine->db);
            for (m = 0; m < 4; ++m) {
                double value = m == MONITOR_CPU ? 1 + group % 5 : m == MONITOR_RAM ? 67108864.0 + group * 1048576.0 : 0;
                sqlite3_bind_int64(metric, 1, id); sqlite3_bind_int(metric, 2, (int)m); sqlite3_bind_double(metric, 3, value);
                CHECK(sqlite3_step(metric) == SQLITE_DONE); sqlite3_reset(metric);
            }
            ++count;
        }
    sqlite3_finalize(window); sqlite3_finalize(metric);
    if (!execute(w, "COMMIT")) return 0;
    QueryPerformanceCounter(&b);
    printf("SEED case=%s windows=%lld metric_rows=%lld real_ms=%.3f\n", w->kind, (long long)count, (long long)(count * 4), milliseconds(a,b));
    fflush(stdout); return 1;
}
static void profile_reset(Workload *w)
{
    int current,highwater;
    sqlite3_stmt_status(w->engine->baseline_query, SQLITE_STMTSTATUS_RUN, 1);
    sqlite3_stmt_status(w->engine->baseline_query, SQLITE_STMTSTATUS_VM_STEP, 1);
    sqlite3_stmt_status(w->engine->baseline_query, SQLITE_STMTSTATUS_FULLSCAN_STEP, 1);
    sqlite3_stmt_status(w->engine->baseline_query, SQLITE_STMTSTATUS_SORT, 1);
    CHECK(sqlite3_db_status(w->engine->db,SQLITE_DBSTATUS_CACHE_HIT,&current,&highwater,1)==SQLITE_OK);
    CHECK(sqlite3_db_status(w->engine->db,SQLITE_DBSTATUS_CACHE_MISS,&current,&highwater,1)==SQLITE_OK);
}
static void inspect_ready(Workload *w, size_t count, unsigned streak)
{
    size_t i;
    unsigned m;
    CHECK(w->snapshot->count == count && w->snapshot->completed_windows == count);
    for (i = 0; i < count; ++i) for (m = 0; m < 4; ++m) {
        const MonitorMetricReport *r = &w->snapshot->rows[i].metric[m];
        CHECK(r->count == w->days * 64 && r->days == w->days && r->minutes == w->days * 64);
        CHECK(r->state == (m == MONITOR_CPU ? MONITOR_ATTENTION : MONITOR_NORMAL));
        CHECK(r->streak == (m == MONITOR_CPU ? streak : 0));
        CHECK(r->baseline_eligible == (m != MONITOR_CPU));
    }
    CHECK(w->snapshot->rows[0].metric[MONITOR_CPU].median == 1);
}
static void run_case(const char *kind, unsigned count, unsigned days, const char *phase)
{
    Workload w;
    LARGE_INTEGER before, after, coupled_end;
    double close_ms, coupled;
    unsigned i, recovery_ticks = 0;
    int gap;
    int cache_hits,cache_misses,cache_used,highwater;
    MonitorStorageStats stats;
    if (!initialize(&w, kind, count, days, phase) || !seed(&w)) { CHECK(0); return; }
    for (i = 0; i < 19; ++i) CHECK(ingest(&w, 3, 0));
    probe(&w, "before-mature-close", -1); profile_reset(&w);
    printf("MEASURE_BEGIN case=%s whole_batch_instances=%u\n", kind, count); fflush(stdout);
    QueryPerformanceCounter(&before); CHECK(ingest(&w, 3, 0)); QueryPerformanceCounter(&after);
    close_ms = milliseconds(before, after); inspect_ready(&w, count, 1);
    CHECK(sqlite3_db_status(w.engine->db,SQLITE_DBSTATUS_CACHE_HIT,&cache_hits,&highwater,0)==SQLITE_OK);
    CHECK(sqlite3_db_status(w.engine->db,SQLITE_DBSTATUS_CACHE_MISS,&cache_misses,&highwater,0)==SQLITE_OK);
    CHECK(sqlite3_db_status(w.engine->db,SQLITE_DBSTATUS_CACHE_USED,&cache_used,&highwater,0)==SQLITE_OK);
    printf("MATURE_CLOSE case=%s instances=%u paths=%u whole_ingest_ms=%.3f snapshot_store_ms=%.3f completed_windows=%zu n=%u dates=%u minutes=%u CPU_state=%s CPU_streak=%u RAM_samples=%u query_runs=%d VM_steps=%d fullscan_steps=%d sorts=%d exact_start_first=%llu exact_start_last=%llu\n",
        kind, count, w.groups, close_ms, w.snapshot->store_ms, w.snapshot->completed_windows,
        w.snapshot->rows[0].metric[0].count, w.snapshot->rows[0].metric[0].days, w.snapshot->rows[0].metric[0].minutes,
        monitor_state_name(w.snapshot->rows[0].metric[0].state), w.snapshot->rows[0].metric[0].streak,
        w.snapshot->rows[0].metric[1].valid_samples,
        sqlite3_stmt_status(w.engine->baseline_query, SQLITE_STMTSTATUS_RUN, 0),
        sqlite3_stmt_status(w.engine->baseline_query, SQLITE_STMTSTATUS_VM_STEP, 0),
        sqlite3_stmt_status(w.engine->baseline_query, SQLITE_STMTSTATUS_FULLSCAN_STEP, 0),
        sqlite3_stmt_status(w.engine->baseline_query, SQLITE_STMTSTATUS_SORT, 0),
        (unsigned long long)w.snapshot->rows[0].metric[0].window_start_utc_100ns,
        (unsigned long long)w.snapshot->rows[count-1].metric[0].window_start_utc_100ns); fflush(stdout);
    printf("PAGE_CACHE_CLOSE case=%s connection_hits=%d connection_misses=%d cache_used_bytes=%d (whole connection during mature close)\n",
        kind,cache_hits,cache_misses,cache_used); fflush(stdout);
    Sleep(3000); QueryPerformanceCounter(&coupled_end);
    coupled = milliseconds(before, coupled_end) / 1000; gap = coupled > 15;
    probe(&w, "after-close-plus-real-3s", gap);
    CHECK(ingest(&w, coupled, gap)); CHECK(w.snapshot->completed_windows == 0);
    for (i = 0; i < count; ++i) {
        const MonitorMetricReport *r = &w.snapshot->rows[i].metric[0];
        CHECK(r->streak == (gap ? 0u : 1u));
        CHECK((r->window_end_utc_100ns == 0) == gap);
        CHECK(r->state == (gap ? MONITOR_LEARNING : MONITOR_ATTENTION));
    }
    printf("COUPLED_FOLLOWUP SYNTHETIC case=%s real_close_ms=%.3f real_wait_ms=3000 measured_next_interval_s=%.6f gap_over_15=%d completed_windows=%zu state=%s streak=%u last_window_end=%llu RAM_count=%u reason=%s\n",
        kind, close_ms, coupled, gap, w.snapshot->completed_windows, monitor_state_name(w.snapshot->rows[0].metric[0].state),
        w.snapshot->rows[0].metric[0].streak, (unsigned long long)w.snapshot->rows[0].metric[0].window_end_utc_100ns,
        w.snapshot->rows[0].metric[1].valid_samples, w.snapshot->rows[0].metric[0].reason); fflush(stdout);
    Sleep(3000); probe(&w, "recovery-after-next-real-3s", 0);
    /* Recovery quality on one representative instance; other states are pruned
     * intentionally. Do not repeat another heavy 500-instance close here. */
    w.batch.count = 1;
    while (!w.snapshot->completed_windows && recovery_ticks < 21) {
        CHECK(ingest(&w, 3, 0)); ++recovery_ticks;
        if (gap && recovery_ticks < 20) CHECK(w.snapshot->completed_windows == 0);
    }
    inspect_ready(&w, 1, gap ? 1 : 2);
    CHECK(w.snapshot->rows[0].metric[1].valid_samples >= 15);
    if (gap) CHECK(w.snapshot->rows[0].metric[1].valid_samples == 20);
    printf("RECOVERY SYNTHETIC case=%s retained_instances=1 represented_3s_ticks=%u completed_windows=%zu CPU_state=%s CPU_streak=%u RAM_samples=%u duration_s=%.6f\n",
        kind, recovery_ticks, w.snapshot->completed_windows, monitor_state_name(w.snapshot->rows[0].metric[0].state),
        w.snapshot->rows[0].metric[0].streak, w.snapshot->rows[0].metric[1].valid_samples,
        w.snapshot->rows[0].metric[1].window_duration_seconds); fflush(stdout);
    CHECK(monitor_storage_stats(w.engine, &stats));
    printf("DB_STATS case=%s raw=%llu windows=%llu metric_rows=%llu page_bytes=%llu raw_page_bytes=%llu aggregate_page_bytes=%llu\n",
        kind, (unsigned long long)stats.raw_rows, (unsigned long long)stats.windows, (unsigned long long)stats.metric_rows,
        (unsigned long long)stats.database_page_bytes, (unsigned long long)stats.raw_page_bytes, (unsigned long long)stats.aggregate_page_bytes);
    monitor_snapshot_free(w.snapshot); resource_collector_destroy(w.probe); monitor_close(w.engine);
    free(w.rows); free(w.paths); free(w.path_text); free(w.programs);
    printf("CASE_END case=%s preserved_synthetic_db=%s\n", kind, w.filename); fflush(stdout);
}
int main(int argc, char **argv)
{
    const char *kind = "all", *phase = "after-cache";
    unsigned count = 500, days = 14;
    int i;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--case") && i+1<argc) kind = argv[++i];
        else if (!strcmp(argv[i], "--phase") && i+1<argc) phase = argv[++i];
        else if (!strcmp(argv[i], "--instances") && i+1<argc) count = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--days") && i+1<argc) days = (unsigned)atoi(argv[++i]);
        else return 2;
    }
    if (!count || count>500 || days<3 || days>14 ||
        (strcmp(kind,"all") && strcmp(kind,"same") && strcmp(kind,"mixed") && strcmp(kind,"distinct")) ||
        (strcmp(phase,"before") && strcmp(phase,"after") && strcmp(phase,"after-cache"))) return 2;
    printf("SYNTHETIC mature whole-batch benchmark SQLite=%s instances=%u days=%u phase=%s; actual self-process collector probes are observations only, not synthetic history\n",
        monitor_storage_version(), count, days, phase); fflush(stdout);
    if (!strcmp(kind,"all") || !strcmp(kind,"same")) run_case("same",count,days,phase);
    if (!strcmp(kind,"all") || !strcmp(kind,"mixed")) run_case("mixed",count,days,phase);
    if (!strcmp(kind,"all") || !strcmp(kind,"distinct")) run_case("distinct",count,days,phase);
    printf("MATURE_BENCH checks=%u failures=%u (assertions, not scenario count or accuracy)\n", checks, failures);
    return failures ? 1 : 0;
}
