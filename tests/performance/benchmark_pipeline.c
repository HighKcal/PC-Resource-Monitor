/* Test-only fixture access to the wrapper's two SQLite connections. Compile this file
 * against the SAME source root as monitor.lib (see build-benchmarks.cmd).
 * No production API or schema is changed by this diagnostic executable. */
#include "../support/experiment_fixture.h"
#include <psapi.h>
#include "../support/monitor_fixture.h" /* Core-only benchmark diagnostics. */

#define BENCH_HASH_INIT UINT64_C(1469598103934665603)
enum { BENCH_MAX_TICKS = 101, BENCH_CATEGORIES = 9 };
static unsigned bench_checks, bench_failures;
#define BENCH_CHECK(x) do { ++bench_checks; if (!(x)) { ++bench_failures; printf("FAIL line=%d %s\n", __LINE__, #x); } } while (0)
static const char *const bench_categories[BENCH_CATEGORIES] = {
    "baseline", "raw", "trim", "feedback", "summary", "program", "run", "transaction", "other"
};
typedef struct BenchSql {
    uint64_t calls[BENCH_CATEGORIES], rows[BENCH_CATEGORIES], vm[BENCH_CATEGORIES];
    uint64_t scans[BENCH_CATEGORIES], sorts[BENCH_CATEGORIES], nanoseconds[BENCH_CATEGORIES];
    uint64_t representative_digest;
} BenchSql;
typedef struct Bench {
    MonitorEngine *core;
    Experiment *experiment;
    ResourceData *rows;
    const char **paths;
    char (*path_text)[128];
    sqlite3_int64 *programs;
    MonitorSnapshot *snapshot;
    ResourceCollector *probe;
    ResourceBatch batch;
    unsigned instances, groups, days, density, spread, profile, wrapper, ticks;
    uint64_t utc, snapshot_digest;
    char source[MAX_PATH], side[MAX_PATH];
    double ordinary[BENCH_MAX_TICKS], closes[BENCH_MAX_TICKS];
    size_t ordinary_count, close_count, completed;
    BenchSql source_sql[2], side_sql[2];
} Bench;
static uint64_t bench_hash(uint64_t h, const void *data, size_t size)
{
    const unsigned char *p = data;
    size_t i;
    for (i = 0; i < size; ++i) h = (h ^ p[i]) * UINT64_C(1099511628211);
    return h;
}
static uint64_t bench_integer(uint64_t h, uint64_t v) { return bench_hash(h, &v, sizeof(v)); }
static uint64_t bench_real(uint64_t h, double v)
{
    uint64_t bits;
    if (isnan(v)) bits = UINT64_C(0x7ff8000000000000);
    else { if (v == 0) v = 0; memcpy(&bits, &v, sizeof(bits)); }
    return bench_integer(h, bits);
}
static uint64_t bench_text(uint64_t h, const char *p)
{
    if (!p) return bench_integer(h, UINT64_MAX);
    h = bench_integer(h, strlen(p)); return bench_hash(h, p, strlen(p));
}
static double bench_qpc(void)
{
    LARGE_INTEGER n, f;
    QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f);
    return (double)n.QuadPart * 1000 / (double)f.QuadPart;
}
static unsigned bench_category(const char *sql)
{
    if (!sql) return 8;
    if (strstr(sql, "WITH RECURSIVE picks")) return 0;
    if (strstr(sql, "INSERT INTO raw")) return 1;
    if (strstr(sql, "DELETE FROM")) return 2;
    if (strstr(sql, "FROM feedback") || strstr(sql, "INTO feedback")) return 3;
    if (strstr(sql, "aggregate_") || strstr(sql, "window_evidence") || strstr(sql, "metric_evidence") ||
        strstr(sql, "decision") || strstr(sql, "episode") || strstr(sql, "reduction")) return 4;
    if (strstr(sql, "program")) return 5;
    if (strstr(sql, "collection_run") || strstr(sql, "operation")) return 6;
    if (strstr(sql, "BEGIN") || strstr(sql, "COMMIT")) return 7;
    return 8;
}
static int bench_trace(unsigned event, void *context, void *statement, void *detail)
{
    BenchSql *p = context;
    sqlite3_stmt *s = statement;
    unsigned c = bench_category(sqlite3_sql(s));
    if (event == SQLITE_TRACE_ROW) {
        ++p->rows[c];
        if (!c) {
            p->representative_digest = bench_real(p->representative_digest, sqlite3_column_double(s, 0));
            p->representative_digest = bench_integer(p->representative_digest, (uint64_t)sqlite3_column_int64(s, 1));
        }
    } else if (event == SQLITE_TRACE_PROFILE) {
        ++p->calls[c]; p->nanoseconds[c] += *(sqlite3_uint64 *)detail;
        p->vm[c] += (unsigned)sqlite3_stmt_status(s, SQLITE_STMTSTATUS_VM_STEP, 1);
        p->scans[c] += (unsigned)sqlite3_stmt_status(s, SQLITE_STMTSTATUS_FULLSCAN_STEP, 1);
        p->sorts[c] += (unsigned)sqlite3_stmt_status(s, SQLITE_STMTSTATUS_SORT, 1);
    }
    return 0;
}
static void bench_reset_statement_counters(sqlite3 *db)
{
    sqlite3_stmt *s = NULL;
    while ((s = sqlite3_next_stmt(db, s)) != NULL) {
        sqlite3_stmt_status(s, SQLITE_STMTSTATUS_VM_STEP, 1);
        sqlite3_stmt_status(s, SQLITE_STMTSTATUS_FULLSCAN_STEP, 1);
        sqlite3_stmt_status(s, SQLITE_STMTSTATUS_SORT, 1);
    }
}
static void bench_sql_add(BenchSql *out, const BenchSql *in)
{
    unsigned c;
    for (c = 0; c < BENCH_CATEGORIES; ++c) {
        out->calls[c] += in->calls[c]; out->rows[c] += in->rows[c]; out->vm[c] += in->vm[c];
        out->scans[c] += in->scans[c]; out->sorts[c] += in->sorts[c]; out->nanoseconds[c] += in->nanoseconds[c];
    }
    out->representative_digest = bench_integer(out->representative_digest, in->representative_digest);
}
static void bench_sql_print(const char *db, const char *phase, const BenchSql *p)
{
    unsigned c;
    for (c = 0; c < BENCH_CATEGORIES; ++c) if (p->calls[c])
        printf("SQL db=%s phase=%s category=%s calls=%llu rows=%llu vm=%llu fullscan=%llu sorts=%llu sqlite_profile_ms=%.3f\n",
            db, phase, bench_categories[c], (unsigned long long)p->calls[c], (unsigned long long)p->rows[c],
            (unsigned long long)p->vm[c], (unsigned long long)p->scans[c], (unsigned long long)p->sorts[c],
            (double)p->nanoseconds[c] / 1000000);
    printf("SQL_REPRESENTATIVES db=%s phase=%s value_end_digest=%016llx (diagnostic row sequence; IDs checked separately)\n",
        db, phase, (unsigned long long)p->representative_digest);
}
static uint64_t bench_file_bytes(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}
static void bench_memory(const char *phase)
{
    PROCESS_MEMORY_COUNTERS_EX p;
    DWORD handles = 0;
    memset(&p, 0, sizeof(p)); p.cb = sizeof(p);
    BENCH_CHECK(GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&p, sizeof(p)));
    BENCH_CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles));
    printf("MEMORY phase=%s working_set=%zu private_bytes=%zu peak_working_set=%zu handles=%lu sqlite_current=%lld sqlite_highwater=%lld\n",
        phase, (size_t)p.WorkingSetSize, (size_t)p.PrivateUsage, (size_t)p.PeakWorkingSetSize,
        (unsigned long)handles, (long long)sqlite3_memory_used(), (long long)sqlite3_memory_highwater(0));
}
static void bench_sizes(const char *phase, const char *db)
{
    char wal[MAX_PATH];
    snprintf(wal, sizeof(wal), "%s-wal", db);
    printf("FILE phase=%s db=%llu wal=%llu path=%s\n", phase,
        (unsigned long long)bench_file_bytes(db), (unsigned long long)bench_file_bytes(wal), db);
}
static int bench_execute(sqlite3 *db, const char *sql)
{
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) printf("SQL_ERROR %s\n", sqlite3_errmsg(db));
    BENCH_CHECK(rc == SQLITE_OK); return rc == SQLITE_OK;
}
static uint64_t bench_db_digest(sqlite3 *db, const char *label, const char *const *queries, unsigned count)
{
    uint64_t h = BENCH_HASH_INIT, rows = 0;
    unsigned q;
    for (q = 0; q < count; ++q) {
        sqlite3_stmt *s = NULL;
        int rc, c;
        h = bench_integer(h, q);
        BENCH_CHECK(sqlite3_prepare_v2(db, queries[q], -1, &s, NULL) == SQLITE_OK);
        if (!s) return 0;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            ++rows; h = bench_integer(h, UINT64_C(0x726f77));
            for (c = 0; c < sqlite3_column_count(s); ++c) {
                int type = sqlite3_column_type(s, c);
                h = bench_integer(h, (unsigned)type);
                if (type == SQLITE_INTEGER) h = bench_integer(h, (uint64_t)sqlite3_column_int64(s, c));
                else if (type == SQLITE_FLOAT) h = bench_real(h, sqlite3_column_double(s, c));
                else if (type == SQLITE_TEXT) h = bench_text(h, (const char *)sqlite3_column_text(s, c));
            }
        }
        BENCH_CHECK(rc == SQLITE_DONE); sqlite3_finalize(s);
    }
    printf("DIGEST label=%s rows=%llu fnv64=%016llx\n", label, (unsigned long long)rows, (unsigned long long)h);
    return h;
}
static void bench_snapshot_digest(Bench *b)
{
    size_t i;
    unsigned m;
    uint64_t h = b->snapshot_digest;
    h = bench_integer(h, b->snapshot->count); h = bench_integer(h, b->snapshot->sequence);
    h = bench_integer(h, b->snapshot->measured_utc_100ns); h = bench_integer(h, b->snapshot->completed_windows);
    for (i = 0; i < b->snapshot->count; ++i) {
        const MonitorRow *row = &b->snapshot->rows[i];
        h = bench_integer(h, row->raw.pid); h = bench_integer(h, row->raw.creation_time_utc_100ns);
        h = bench_integer(h, row->program_id); h = bench_integer(h, row->path_error);
        h = bench_text(h, row->name); h = bench_text(h, row->path);
        h = bench_integer(h, row->feedback); h = bench_integer(h, row->feedback_expires_utc_100ns);
        for (m = 0; m < 4; ++m) {
            const MonitorMetricReport *r = &row->metric[m];
            h = bench_integer(h, r->state); h = bench_real(h, r->current);
            h = bench_real(h, r->median); h = bench_real(h, r->mad); h = bench_real(h, r->threshold);
            h = bench_real(h, r->covered_seconds); h = bench_integer(h, r->count); h = bench_integer(h, r->days);
            h = bench_integer(h, r->minutes); h = bench_integer(h, r->streak); h = bench_integer(h, r->valid_samples);
            h = bench_integer(h, r->window_start_utc_100ns); h = bench_integer(h, r->window_end_utc_100ns);
            h = bench_real(h, r->window_duration_seconds); h = bench_integer(h, (unsigned)r->baseline_eligible);
            h = bench_text(h, r->reason);
        }
    }
    b->snapshot_digest = h;
}
static int bench_ingest(Bench *b)
{
    monitor_snapshot_free(b->snapshot); b->snapshot = NULL;
    if (!(b->wrapper ? experiment_ingest(b->experiment, &b->batch, b->paths, &b->snapshot) :
        monitor_ingest(b->core, &b->batch, b->paths, &b->snapshot))) {
        printf("INGEST_ERROR %s\n", b->wrapper ? experiment_error(b->experiment) : monitor_error(b->core));
        BENCH_CHECK(0); return 0;
    }
    return 1;
}
static void bench_batch(Bench *b, unsigned tick)
{
    unsigned i;
    size_t n = 0;
    for (i = 0; i < b->instances; ++i) {
        ResourceData *r;
        unsigned group = i % b->groups;
        if (b->spread && i % 20 > tick) continue;
        r = &b->rows[n]; memset(r, 0, sizeof(*r));
        r->pid = 10000 + i; r->creation_time_utc_100ns = b->utc - 20 * DAY + i;
        r->measured_at_utc_100ns = b->utc + (uint64_t)tick * 3 * TICKS + (uint64_t)i * 200;
        r->process_name_utf8 = "SYNTHETIC benchmark.exe";
        r->valid_fields = RESOURCE_VALID_NAME | RESOURCE_VALID_IDENTITY | RESOURCE_VALID_CPU | RESOURCE_VALID_MEMORY |
            RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL;
        r->cpu_percent = tick < 40 ? 50 : 0;
        r->working_set_bytes = UINT64_C(67108864) + (uint64_t)group * UINT64_C(1048576);
        r->observation_interval_seconds = r->cpu_interval_seconds = r->io_interval_seconds = 3;
        r->cpu_status = r->io_read_status = r->io_write_status = RESOURCE_DELTA_VALID;
        r->foreground = RESOURCE_FOREGROUND_UNKNOWN;
        b->paths[n] = b->path_text[group]; ++n;
    }
    b->batch.items = b->rows; b->batch.count = n; b->batch.logical_processor_count = 12;
    b->batch.started_at_utc_100ns = b->utc + (uint64_t)tick * 3 * TICKS;
    b->batch.finished_at_utc_100ns = b->batch.started_at_utc_100ns + (uint64_t)b->instances * 200;
}
static int bench_seed(Bench *b)
{
    sqlite3_stmt *window = NULL, *metric = NULL;
    unsigned group, day, minute, process, m;
    uint64_t windows = 0;
    double before = bench_qpc();
    if (!b->days) return 1;
    if (!bench_execute(monitor_fixture_db(b->core), "BEGIN IMMEDIATE")) return 0;
    BENCH_CHECK(sqlite3_prepare_v2(monitor_fixture_db(b->core),
        "INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) VALUES(?,?,?,?,?,?,60,1)",
        -1, &window, NULL) == SQLITE_OK);
    BENCH_CHECK(sqlite3_prepare_v2(monitor_fixture_db(b->core),
        "INSERT INTO aggregate_metric VALUES(?,?,?,60,20,1,2,'SYNTHETIC benchmark seed',896,14,896,0,0,0,0)",
        -1, &metric, NULL) == SQLITE_OK);
    if (!window || !metric) return 0;
    for (group = 0; group < b->groups; ++group) for (day = 0; day < b->days; ++day)
        for (minute = 1; minute <= 64; ++minute) for (process = 0; process < b->density; ++process) {
            uint64_t end = (b->utc / DAY - day) * DAY + ((uint64_t)minute * 60 + process) * TICKS;
            sqlite3_int64 id;
            sqlite3_bind_int64(window, 1, 1); sqlite3_bind_int64(window, 2, b->programs[group]);
            sqlite3_bind_int64(window, 3, 50000 + process); sqlite3_bind_int64(window, 4, (sqlite3_int64)(b->utc - 20 * DAY + process));
            sqlite3_bind_int64(window, 5, (sqlite3_int64)(end - 60 * TICKS)); sqlite3_bind_int64(window, 6, (sqlite3_int64)end);
            BENCH_CHECK(sqlite3_step(window) == SQLITE_DONE); sqlite3_reset(window); id = sqlite3_last_insert_rowid(monitor_fixture_db(b->core));
            for (m = 0; m < 4; ++m) {
                double value = m == MONITOR_CPU ? 1 + group % 5 : m == MONITOR_RAM ? 67108864.0 + group * 1048576.0 : 0;
                sqlite3_bind_int64(metric, 1, id); sqlite3_bind_int(metric, 2, (int)m); sqlite3_bind_double(metric, 3, value);
                BENCH_CHECK(sqlite3_step(metric) == SQLITE_DONE); sqlite3_reset(metric);
            }
            ++windows;
        }
    sqlite3_finalize(window); sqlite3_finalize(metric);
    if (!bench_execute(monitor_fixture_db(b->core), "COMMIT")) return 0;
    printf("SEED windows=%llu metric_rows=%llu elapsed_ms=%.3f (excluded from timed ingestion)\n",
        (unsigned long long)windows, (unsigned long long)(windows * 4), bench_qpc() - before);
    fflush(stdout); return 1;
}
static int bench_double_compare(const void *a, const void *b)
{ double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static void bench_timing(const char *phase, double *samples, size_t n)
{
    size_t i;
    if (!n) return;
    if (n < 20) { printf("TIMING_SAMPLES phase=%s n=%zu ms=", phase, n); for (i = 0; i < n; ++i) printf("%s%.6f", i ? "," : "", samples[i]); printf("\n"); }
    qsort(samples, n, sizeof(*samples), bench_double_compare);
    printf("TIMING phase=%s n=%zu min_ms=%.6f median_ms=%.6f max_ms=%.6f", phase, n, samples[0], samples[(n-1)/2], samples[n-1]);
    if (n >= 20) printf(" p95_ms=%.6f", samples[(n*95+99)/100-1]);
    printf("\n");
}
/* Isolated diagnostic replay of the original C reduction over selected points.
 * It attributes CPU work without adding timers inside production inner loops.
 * It is NOT the observed phase duration of the measured ingestion call. */
static void bench_reduction(Bench *b)
{
    double values[896], work[896], deviations[896], elapsed[3], result = 0;
    uint64_t minute_ids[896], selected_minutes[896];
    unsigned age, n = 0, repeat, i, j, distinct = 0;
    sqlite3_int64 cutoff;
    sqlite3_stmt *s = monitor_fixture_baseline_statement(b->core);
    uint64_t start = monitor_fixture_instance_start(b->core, 0), date = start / DAY;
    {
        sqlite3_stmt *maximum = NULL;
        cutoff = -1;
        if (sqlite3_prepare_v2(monitor_fixture_db(b->core), "SELECT max(id) FROM aggregate_window", -1, &maximum, NULL) == SQLITE_OK &&
            sqlite3_step(maximum) == SQLITE_ROW)
            cutoff = sqlite3_column_int64(maximum, 0);
        sqlite3_finalize(maximum);
    }
    BENCH_CHECK(cutoff >= 0);
    for (age = 0; age < 14; ++age) {
        int rc;
        sqlite3_bind_int(s, 1, MONITOR_RAM); sqlite3_bind_int64(s, 2, monitor_fixture_instance_program(b->core, 0));
        sqlite3_bind_int64(s, 3, (sqlite3_int64)((date-age)*DAY)); sqlite3_bind_int64(s, 4, (sqlite3_int64)((date-age+1)*DAY));
        sqlite3_bind_int64(s, 5, (sqlite3_int64)start); sqlite3_bind_int64(s, 6, cutoff);
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) if (n < 896) {
            values[n] = sqlite3_column_double(s, 0); selected_minutes[n] = (uint64_t)sqlite3_column_int64(s, 1)/(60*TICKS); ++n;
        }
        BENCH_CHECK(rc == SQLITE_DONE); sqlite3_reset(s); sqlite3_clear_bindings(s);
    }
    for (repeat = 0; repeat < 3; ++repeat) {
        double before = bench_qpc();
        unsigned trial;
        for (trial = 0; trial < 2000; ++trial) {
            distinct = 0;
            for (i = 0; i < n; ++i) {
                for (j = 0; j < distinct && minute_ids[j] != selected_minutes[i]; ++j) { }
                if (j == distinct) minute_ids[distinct++] = selected_minutes[i];
            }
            result += distinct;
        }
        elapsed[repeat] = (bench_qpc()-before)/2000;
    }
    printf("REDUCTION_REPLAY phase=quadratic_minute_dedup points=%u distinct=%u trials_per_sample=2000 ms=%.6f,%.6f,%.6f\n", n, distinct, elapsed[0], elapsed[1], elapsed[2]);
    BENCH_CHECK(n == distinct);
    for (repeat = 0; repeat < 3; ++repeat) {
        double before = bench_qpc();
        unsigned trial;
        for (trial = 0; trial < 2000; ++trial) {
            double med;
            memcpy(work, values, n*sizeof(*work));
            qsort(work, n, sizeof(*work), bench_double_compare);
            med = n ? (n%2 ? work[n/2] : work[n/2-1]/2 + work[n/2]/2) : 0;
            for (i = 0; i < n; ++i) deviations[i] = fabs(values[i]-med);
            qsort(deviations, n, sizeof(*deviations), bench_double_compare);
            result += med + (n ? deviations[n/2] : 0);
        }
        elapsed[repeat] = (bench_qpc()-before)/2000;
    }
    printf("REDUCTION_REPLAY phase=median_MAD points=%u trials_per_sample=2000 ms=%.6f,%.6f,%.6f checksum=%.1f (isolated CPU replay, not in-ingest stage timing)\n", n, elapsed[0], elapsed[1], elapsed[2], result);
}
static void bench_digests(Bench *b)
{
    static const char *const source_queries[] = {
        "SELECT * FROM program ORDER BY id", "SELECT * FROM collection_run ORDER BY id", "SELECT * FROM raw ORDER BY id",
        "SELECT * FROM aggregate_window ORDER BY id", "SELECT * FROM aggregate_metric ORDER BY window_id,metric", "SELECT * FROM feedback ORDER BY id"
    };
    static const char *const side_queries[] = {
        "SELECT * FROM operation ORDER BY id", "SELECT * FROM window_evidence ORDER BY id", "SELECT * FROM metric_evidence ORDER BY window_id,metric",
        "SELECT * FROM decision ORDER BY id", "SELECT * FROM episode ORDER BY id", "SELECT * FROM reduction ORDER BY session,pid,creation,metric,method",
        "SELECT * FROM evaluation ORDER BY id", "SELECT * FROM retention_state", "SELECT * FROM replay_info", "SELECT * FROM sqlite_sequence ORDER BY name"
    };
    printf("DIGEST label=snapshots deterministic_ticks=%u fnv64=%016llx (timing fields excluded)\n", b->ticks, (unsigned long long)b->snapshot_digest);
    bench_db_digest(monitor_fixture_db(b->core), "source", source_queries, sizeof(source_queries)/sizeof(source_queries[0]));
    if (b->wrapper) bench_db_digest(experiment_fixture_db(b->experiment), "sidecar", side_queries, sizeof(side_queries)/sizeof(side_queries[0]));
}
static double bench_followup(Bench *b, double last_begin)
{
    ResourceBatch observation = {0};
    uint32_t error = 0;
    double before, interval;
    size_t i;
    Sleep(3000); before = bench_qpc(); interval = (before-last_begin)/1000;
    BENCH_CHECK(resource_collector_collect(b->probe, &observation, &error) == RESOURCE_OK);
    for (i = 0; i < observation.count; ++i) if (observation.items[i].pid == GetCurrentProcessId()) {
        const ResourceData *r = &observation.items[i];
        printf("ACTUAL_SELF_PROBE observation_s=%.6f CPU_interval_s=%.6f CPU_status=%s read_status=%s valid=0x%x (real collector observations; synthetic history is separate)\n",
            r->observation_interval_seconds, r->cpu_interval_seconds, resource_delta_status_string(r->cpu_status), resource_delta_status_string(r->io_read_status), r->valid_fields);
        break;
    }
    BENCH_CHECK(i < observation.count); resource_batch_free(&observation);
    printf("ACTUAL_NEXT_INTERVAL seconds=%.6f gap_over_15=%d (final ingest plus real 3-second caller wait; before digest/diagnostics)\n", interval, interval > 15);
    return interval;
}
static void bench_coupled_ingest(Bench *b, double interval)
{
    size_t i;
    for (i = 0; i < b->batch.count; ++i) {
        ResourceData *r = &b->rows[i];
        r->measured_at_utc_100ns += (uint64_t)(interval*TICKS);
        r->observation_interval_seconds = r->cpu_interval_seconds = r->io_interval_seconds = interval;
        if (interval > 15) {
            r->valid_fields &= ~(unsigned)(RESOURCE_VALID_CPU | RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL);
            r->cpu_status = r->io_read_status = r->io_write_status = RESOURCE_DELTA_LONG_GAP;
        }
    }
    b->batch.started_at_utc_100ns += (uint64_t)(interval*TICKS); b->batch.finished_at_utc_100ns += (uint64_t)(interval*TICKS);
    BENCH_CHECK(bench_ingest(b));
    printf("COUPLED_FOLLOWUP next_interval_s=%.6f gap_over_15=%d completed_windows=%zu CPU_state=%s CPU_streak=%u (excluded from deterministic digests)\n",
        interval, interval > 15, b->snapshot->completed_windows, monitor_state_name(b->snapshot->rows[0].metric[0].state), b->snapshot->rows[0].metric[0].streak);
}
int main(int argc, char **argv)
{
    Bench b;
    const char *kind = "mixed", *out = NULL;
    char error[512];
    unsigned i, tick;
    double last_begin = 0, coupled_interval;
    uint32_t os_error = 0;
    memset(&b, 0, sizeof(b)); b.instances = 500; b.days = 14; b.snapshot_digest = BENCH_HASH_INIT;
    b.utc = UINT64_C(134381808000000000); /* Fixed UTC fixture; never wall-clock input. */
    for (i = 1; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--out") && i+1 < (unsigned)argc) out = argv[++i];
        else if (!strcmp(argv[i], "--case") && i+1 < (unsigned)argc) kind = argv[++i];
        else if (!strcmp(argv[i], "--instances") && i+1 < (unsigned)argc) b.instances = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--days") && i+1 < (unsigned)argc) b.days = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--utc") && i+1 < (unsigned)argc) b.utc = _strtoui64(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--mode") && i+1 < (unsigned)argc) {
            const char *mode = argv[++i]; if (strcmp(mode, "core") && strcmp(mode, "experiment")) return 2; b.wrapper = !strcmp(mode, "experiment");
        } else if (!strcmp(argv[i], "--schedule") && i+1 < (unsigned)argc) {
            const char *schedule = argv[++i]; if (strcmp(schedule, "sync") && strcmp(schedule, "spread")) return 2; b.spread = !strcmp(schedule, "spread");
        } else if (!strcmp(argv[i], "--profile")) b.profile = 1;
        else return 2;
    }
    if (!out || strlen(out) > MAX_PATH-32 || !b.instances || b.instances > 500 || (b.days && b.days != 14) ||
        b.utc < 30*DAY || (strcmp(kind,"same") && strcmp(kind,"mixed") && strcmp(kind,"distinct"))) return 2;
    b.groups = !strcmp(kind,"same") ? 1 : !strcmp(kind,"mixed") ? b.instances/10 : b.instances;
    if (!b.groups) b.groups = 1;
    b.density = !strcmp(kind,"same") ? 32 : !strcmp(kind,"mixed") ? 8 : 1;
    b.ticks = b.spread ? 80 : 61;
    snprintf(b.source, sizeof(b.source), "%s-source.sqlite", out); snprintf(b.side, sizeof(b.side), "%s-evidence.sqlite", out);
    if (GetFileAttributesA(b.source) != INVALID_FILE_ATTRIBUTES || GetFileAttributesA(b.side) != INVALID_FILE_ATTRIBUTES) {
        printf("Refusing existing benchmark output\n"); return 2;
    }
    printf("FIXTURE mode=%s instances=%u paths=%u case=%s days=%u density=%u schedule=%s ticks=%u UTC=%llu row_UTC_step=200 profile=%u SQLite=%s OS_cache=not_flushed\n",
        b.wrapper?"experiment":"core", b.instances, b.groups, kind, b.days, b.density, b.spread?"spread":"sync", b.ticks, (unsigned long long)b.utc, b.profile, sqlite3_libversion());
    if (b.wrapper) {
        if (!experiment_create(b.source, b.side, 1, &b.experiment, error, sizeof(error))) { printf("OPEN_ERROR %s\n", error); return 1; }
        b.core = experiment_fixture_core(b.experiment);
    } else if (!monitor_open(b.source, 1, &b.core, error, sizeof(error))) { printf("OPEN_ERROR %s\n", error); return 1; }
    b.rows = calloc(b.instances, sizeof(*b.rows)); b.paths = calloc(b.instances, sizeof(*b.paths));
    b.path_text = calloc(b.groups, sizeof(*b.path_text)); b.programs = calloc(b.groups, sizeof(*b.programs));
    if (!b.rows || !b.paths || !b.path_text || !b.programs) return 1;
    for (i = 0; i < b.groups; ++i) snprintf(b.path_text[i], sizeof(b.path_text[i]), "SYNTHETIC:\\pipeline-%s\\program-%u.exe", kind, i);
    { unsigned spread = b.spread; b.spread = 0; bench_batch(&b, 0); b.spread = spread; }
    if (!bench_ingest(&b)) return 1;
    for (i = 0; i < b.groups; ++i) b.programs[i] = b.snapshot->rows[i].program_id;
    if (b.wrapper) BENCH_CHECK(experiment_reset(b.experiment, b.utc, "benchmark deterministic registration reset"));
    else monitor_reset(b.core);
    if (!bench_seed(&b)) return 1;
    BENCH_CHECK(resource_collector_create(NULL, &b.probe, &os_error) == RESOURCE_OK);
    bench_memory("before");
    if (b.profile) { bench_reset_statement_counters(monitor_fixture_db(b.core)); if (b.wrapper) bench_reset_statement_counters(experiment_fixture_db(b.experiment)); }
    for (tick = 0; tick < b.ticks; ++tick) {
        BenchSql core_profile = {0}, side_profile = {0};
        double before, elapsed;
        int close;
        core_profile.representative_digest = side_profile.representative_digest = BENCH_HASH_INIT;
        bench_batch(&b, tick);
        if (b.profile) {
            sqlite3_trace_v2(monitor_fixture_db(b.core), SQLITE_TRACE_PROFILE|SQLITE_TRACE_ROW, bench_trace, &core_profile);
            if (b.wrapper) sqlite3_trace_v2(experiment_fixture_db(b.experiment), SQLITE_TRACE_PROFILE|SQLITE_TRACE_ROW, bench_trace, &side_profile);
        }
        if (tick == b.ticks-1) {
            ResourceBatch probe_batch = {0};
            BENCH_CHECK(resource_collector_collect(b.probe, &probe_batch, &os_error) == RESOURCE_OK); resource_batch_free(&probe_batch);
        }
        before = bench_qpc();
        if (!bench_ingest(&b)) return 1;
        elapsed = bench_qpc()-before;
        if (tick == b.ticks-1) last_begin = before;
        if (b.profile) { sqlite3_trace_v2(monitor_fixture_db(b.core), 0, NULL, NULL); if (b.wrapper) sqlite3_trace_v2(experiment_fixture_db(b.experiment), 0, NULL, NULL); }
        close = b.snapshot->completed_windows != 0;
        if (close) b.closes[b.close_count++] = elapsed; else b.ordinary[b.ordinary_count++] = elapsed;
        b.completed += b.snapshot->completed_windows;
        bench_sql_add(&b.source_sql[close], &core_profile); bench_sql_add(&b.side_sql[close], &side_profile);
        bench_snapshot_digest(&b);
        if (close) printf("CLOSE tick=%u completed=%zu total_ms=%.6f core_store_ms=%.6f outside_core_ms=%.6f CPU_state=%s CPU_count=%u CPU_days=%u CPU_minutes=%u\n",
            tick, b.snapshot->completed_windows, elapsed, b.snapshot->store_ms, elapsed-b.snapshot->store_ms,
            monitor_state_name(b.snapshot->rows[0].metric[0].state), b.snapshot->rows[0].metric[0].count,
            b.snapshot->rows[0].metric[0].days, b.snapshot->rows[0].metric[0].minutes);
        fflush(stdout);
    }
    BENCH_CHECK(b.completed == (size_t)b.instances*3);
    /* Coupled observation must precede DB hashing and CPU diagnostics. */
    coupled_interval = bench_followup(&b, last_begin);
    bench_timing("ordinary", b.ordinary, b.ordinary_count); bench_timing("close", b.closes, b.close_count);
    bench_memory("after"); bench_sizes("open", b.source); if (b.wrapper) bench_sizes("open", b.side);
    if (b.profile) {
        bench_sql_print("source", "ordinary", &b.source_sql[0]); bench_sql_print("source", "close", &b.source_sql[1]);
        if (b.wrapper) { bench_sql_print("sidecar", "ordinary", &b.side_sql[0]); bench_sql_print("sidecar", "close", &b.side_sql[1]); }
        bench_reduction(&b);
    }
    bench_digests(&b);
    bench_coupled_ingest(&b, coupled_interval);
    monitor_snapshot_free(b.snapshot); resource_collector_destroy(b.probe);
    if (b.wrapper) experiment_close(b.experiment); else monitor_close(b.core);
    bench_sizes("closed", b.source); if (b.wrapper) bench_sizes("closed", b.side);
    free(b.rows); free(b.paths); free(b.path_text); free(b.programs);
    printf("RESULT checks=%u failures=%u\n", bench_checks, bench_failures);
    return bench_failures ? 1 : 0;
}
