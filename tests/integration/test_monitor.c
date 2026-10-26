#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "monitor_internal.h"

static unsigned checks, failures, file_sequence;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
typedef struct Fixture {
    MonitorEngine *engine;
    ResourceData rows[2];
    ResourceBatch batch;
    const char *paths[2];
    MonitorSnapshot *snapshot;
    uint64_t now;
    char file[MAX_PATH];
} Fixture;
static int sql(MonitorEngine *e, const char *text)
{
    char *error = NULL;
    int rc = sqlite3_exec(e->db, text, NULL, NULL, &error);
    if (rc != SQLITE_OK) printf("fixture SQL error: %s\n", error);
    sqlite3_free(error); return rc == SQLITE_OK;
}
static int64_t scalar(MonitorEngine *e, const char *text)
{
    sqlite3_stmt *s = NULL;
    int64_t v = -1;
    CHECK(sqlite3_prepare_v2(e->db, text, -1, &s, NULL) == SQLITE_OK);
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    else CHECK(0);
    sqlite3_finalize(s); return v;
}
static void init(Fixture *f, unsigned count)
{
    char error[256], temp[MAX_PATH];
    unsigned i;
    memset(f, 0, sizeof(*f));
    GetTempPathA(MAX_PATH, temp);
    snprintf(f->file, sizeof(f->file), "%smonitor-fixture-%lu-%u.sqlite", temp,
        (unsigned long)GetCurrentProcessId(), ++file_sequence);
    CHECK(GetFileAttributesA(f->file) == INVALID_FILE_ATTRIBUTES);
    CHECK(monitor_open(f->file, 1, &f->engine, error, sizeof(error)));
    if (!f->engine) { printf("open failed: %s\n", error); exit(1); }
    f->now = (monitor_utc_now() / MONITOR_DAY_TICKS) * MONITOR_DAY_TICKS + 12 * 3600 * MONITOR_TICKS_PER_SECOND;
    f->batch.items = f->rows; f->batch.count = count; f->batch.logical_processor_count = 8;
    for (i = 0; i < count; ++i) {
        ResourceData *r = &f->rows[i];
        r->pid = 100 + i; r->creation_time_utc_100ns = f->now - 3600 * MONITOR_TICKS_PER_SECOND;
        r->process_name_utf8 = "same.exe"; f->paths[i] = "SYNTHETIC:\\A\\same.exe";
        r->valid_fields = RESOURCE_VALID_NAME | RESOURCE_VALID_IDENTITY | RESOURCE_VALID_CPU | RESOURCE_VALID_MEMORY |
            RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL;
        r->cpu_percent = 10; r->working_set_bytes = UINT64_C(67108864);
        r->observation_interval_seconds = r->cpu_interval_seconds = r->io_interval_seconds = 3;
        r->foreground = RESOURCE_FOREGROUND_UNKNOWN;
    }
}
static int tick(Fixture *f, double seconds)
{
    size_t i;
    f->now += (uint64_t)(seconds * (double)MONITOR_TICKS_PER_SECOND);
    for (i = 0; i < f->batch.count; ++i) f->rows[i].measured_at_utc_100ns = f->now;
    f->batch.started_at_utc_100ns = f->batch.finished_at_utc_100ns = f->now;
    monitor_snapshot_free(f->snapshot); f->snapshot = NULL;
    return monitor_ingest(f->engine, &f->batch, f->paths, &f->snapshot);
}
static void window(Fixture *f)
{
    unsigned i;
    for (i = 0; i < 20; ++i) CHECK(tick(f, 3));
    CHECK(f->snapshot && f->snapshot->completed_windows == f->batch.count);
}
static void cleanup(Fixture *f)
{
    monitor_snapshot_free(f->snapshot); monitor_close(f->engine);
    f->snapshot = NULL; f->engine = NULL;
    CHECK(DeleteFileA(f->file));
}
static void seed(Fixture *f, unsigned count, int same_minute, unsigned metric_mask, int day_offset)
{
    unsigned i, m;
    sqlite3_stmt *w = NULL, *metric = NULL;
    sqlite3_int64 run = scalar(f->engine, "SELECT max(id) FROM collection_run"), program = f->snapshot->rows[0].program_id;
    CHECK(sql(f->engine, "BEGIN IMMEDIATE"));
    CHECK(sqlite3_prepare_v2(f->engine->db,
        "INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) VALUES(?,?,1,1,?,?,60,1)", -1, &w, NULL) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(f->engine->db,
        "INSERT INTO aggregate_metric VALUES(?,?,?,60,20,1,1,'seed',0,0,0,NULL,NULL,NULL,0)", -1, &metric, NULL) == SQLITE_OK);
    for (i = 0; i < count; ++i) {
        uint64_t end = (f->now / MONITOR_DAY_TICKS - (uint64_t)(day_offset + (int)(i % 3))) * MONITOR_DAY_TICKS +
            (same_minute ? 60 : (uint64_t)(i / 3 + 1) * 60) * MONITOR_TICKS_PER_SECOND;
        sqlite3_int64 id;
        sqlite3_bind_int64(w, 1, run); sqlite3_bind_int64(w, 2, program);
        sqlite3_bind_int64(w, 3, (sqlite3_int64)(end - 60 * MONITOR_TICKS_PER_SECOND)); sqlite3_bind_int64(w, 4, (sqlite3_int64)end);
        CHECK(sqlite3_step(w) == SQLITE_DONE); sqlite3_reset(w); id = sqlite3_last_insert_rowid(f->engine->db);
        for (m = 0; m < 4; ++m) if (metric_mask & (1u << m)) {
            sqlite3_bind_int64(metric, 1, id); sqlite3_bind_int(metric, 2, (int)m);
            sqlite3_bind_double(metric, 3, m == 0 ? 10 : m == 1 ? 67108864 : 0);
            CHECK(sqlite3_step(metric) == SQLITE_DONE); sqlite3_reset(metric);
        }
    }
    sqlite3_finalize(w); sqlite3_finalize(metric); CHECK(sql(f->engine, "COMMIT"));
}
static void test_policy(void)
{
    Fixture f;
    int64_t program;
    unsigned m;
    init(&f, 1); CHECK(tick(&f, 0)); program = f.snapshot->rows[0].program_id;
    seed(&f, 60, 0, 15, 1); window(&f);
    for (m = 0; m < 4; ++m) {
        CHECK(f.snapshot->rows[0].metric[m].state == MONITOR_NORMAL);
        CHECK(f.snapshot->rows[0].metric[m].count == 60);
        CHECK(f.snapshot->rows[0].metric[m].days == 3);
        CHECK(f.snapshot->rows[0].metric[m].minutes == 60);
    }
    CHECK(f.snapshot->rows[0].metric[0].threshold == 15);
    f.rows[0].cpu_percent = 15; window(&f); CHECK(f.snapshot->rows[0].metric[0].state == MONITOR_NORMAL);
    f.rows[0].cpu_percent = 50; window(&f); CHECK(f.snapshot->rows[0].metric[0].state == MONITOR_ATTENTION);
    CHECK(f.snapshot->rows[0].metric[0].streak == 1);
    window(&f); CHECK(f.snapshot->rows[0].metric[0].streak == 2);
    window(&f); CHECK(f.snapshot->rows[0].metric[0].state == MONITOR_CANDIDATE);
    CHECK(f.snapshot->rows[0].metric[0].streak == 3 && !f.snapshot->rows[0].metric[0].baseline_eligible);
    CHECK(f.snapshot->rows[0].metric[1].state == MONITOR_NORMAL);
    CHECK(monitor_feedback(f.engine, program, MONITOR_FEEDBACK_EXPECTED, f.now));
    CHECK(tick(&f, 3)); CHECK(f.snapshot->rows[0].feedback == MONITOR_FEEDBACK_EXPECTED);
    CHECK(f.snapshot->rows[0].metric[0].state == MONITOR_CANDIDATE);
    CHECK(monitor_feedback(f.engine, program, MONITOR_FEEDBACK_CANCEL, f.now)); CHECK(tick(&f, 3));
    CHECK(f.snapshot->rows[0].feedback == MONITOR_FEEDBACK_CANCEL);
    f.rows[0].valid_fields &= ~(unsigned)RESOURCE_VALID_CPU; CHECK(tick(&f, 3));
    CHECK(f.snapshot->rows[0].metric[0].state == MONITOR_UNAVAILABLE && !f.snapshot->rows[0].metric[0].streak);
    f.rows[0].valid_fields |= RESOURCE_VALID_CPU;
    while (f.snapshot->completed_windows == 0) CHECK(tick(&f, 3));
    CHECK(f.snapshot->rows[0].metric[0].streak == 1); /* 95% valid window, but missing sample reset persistence. */
    f.rows[0].cpu_percent = 10; window(&f); CHECK(f.snapshot->rows[0].metric[0].streak == 0);
    cleanup(&f);
}
static void test_learning_identity(void)
{
    Fixture f;
    int64_t first;
    init(&f, 2); f.paths[1] = "SYNTHETIC:\\B\\same.exe"; CHECK(tick(&f, 0));
    first = f.snapshot->rows[0].program_id;
    CHECK(first != f.snapshot->rows[1].program_id); seed(&f, 60, 1, 15, 1); window(&f);
    CHECK(f.snapshot->rows[0].metric[0].count == 3 && f.snapshot->rows[0].metric[0].minutes == 3);
    CHECK(f.snapshot->rows[0].metric[0].state == MONITOR_LEARNING);
    printf("INSUFFICIENT_TIME available_windows=60 dates=3 distinct_minutes=3 selected_n=%u selected_minutes=%u state=%s\n",
        f.snapshot->rows[0].metric[0].count, f.snapshot->rows[0].metric[0].minutes,
        monitor_state_name(f.snapshot->rows[0].metric[0].state));
    CHECK(f.snapshot->rows[1].metric[0].count == 0 && f.snapshot->rows[1].metric[0].state == MONITOR_LEARNING);
    f.rows[0].valid_fields &= ~(unsigned)RESOURCE_VALID_IDENTITY; CHECK(tick(&f, 3));
    CHECK(f.snapshot->rows[0].program_id == 0 && f.snapshot->rows[0].metric[0].state == MONITOR_UNAVAILABLE);
    CHECK(scalar(f.engine, "SELECT count(*) FROM raw WHERE program_id IS NULL AND name='same.exe' AND creation IS NULL") == 1);
    f.rows[0].valid_fields |= RESOURCE_VALID_IDENTITY; f.rows[0].creation_time_utc_100ns += 1;
    CHECK(tick(&f, 3)); CHECK(f.snapshot->rows[0].metric[0].window_end_utc_100ns == 0);
    CHECK(f.snapshot->rows[0].program_id == first);
    f.rows[1].creation_time_utc_100ns += 1; f.paths[1] = NULL; CHECK(tick(&f, 3));
    CHECK(f.snapshot->rows[1].path == NULL && f.snapshot->rows[1].path_error != 0);
    cleanup(&f);
}
static void test_past_only(void)
{
    Fixture f;
    int64_t cutoff;
    char update[256];
    init(&f, 2); CHECK(tick(&f, 0)); seed(&f, 59, 0, 15, 1);
    cutoff = scalar(f.engine, "SELECT max(id) FROM aggregate_window");
    seed(&f, 60, 0, 15, -1); /* future/current date rows must not prepare baseline */
    snprintf(update, sizeof(update), "UPDATE aggregate_window SET start_utc=%llu,end_utc=%llu WHERE id>%lld",
        (unsigned long long)(f.now + MONITOR_DAY_TICKS), (unsigned long long)(f.now + MONITOR_DAY_TICKS + 60 * MONITOR_TICKS_PER_SECOND), (long long)cutoff);
    CHECK(sql(f.engine, update));
    window(&f);
    CHECK(f.snapshot->rows[0].metric[0].state == MONITOR_LEARNING);
    CHECK(f.snapshot->rows[0].metric[0].count == 59);
    CHECK(f.snapshot->rows[1].metric[0].count == 59); /* one cutoff excludes same batch's first close */
    window(&f); CHECK(f.snapshot->rows[0].metric[0].count == 59); /* preceding boundary has end==start */
    window(&f); CHECK(f.snapshot->rows[0].metric[0].state == MONITOR_NORMAL);
    CHECK(f.snapshot->rows[0].metric[0].count == 60);
    cleanup(&f);
}
static void test_weights_gap_restart(void)
{
    Fixture f;
    unsigned i;
    uint64_t before;
    char error[256];
    init(&f, 1); CHECK(tick(&f, 0));
    for (i = 0; i < 10; ++i) {
        f.rows[0].cpu_percent = 2; f.rows[0].cpu_interval_seconds = 1; CHECK(tick(&f, 3));
    }
    for (i = 0; i < 10; ++i) {
        f.rows[0].cpu_percent = 8; f.rows[0].cpu_interval_seconds = 5; CHECK(tick(&f, 3));
    }
    CHECK(fabs(f.snapshot->rows[0].metric[0].current - 7) < 1e-9);
    CHECK(f.snapshot->rows[0].metric[0].covered_seconds == 60);
    before = (uint64_t)scalar(f.engine, "SELECT count(*) FROM aggregate_window");
    f.rows[0].observation_interval_seconds = 16; CHECK(tick(&f, 16));
    CHECK(f.snapshot->rows[0].metric[0].window_end_utc_100ns == 0);
    f.rows[0].observation_interval_seconds = 3; CHECK(tick(&f, 3));
    CHECK((uint64_t)scalar(f.engine, "SELECT count(*) FROM aggregate_window") == before);
    CHECK(monitor_feedback(f.engine, f.snapshot->rows[0].program_id, MONITOR_FEEDBACK_IGNORE, f.now));
    monitor_close(f.engine); f.engine = NULL;
    CHECK(monitor_open(f.file, 1, &f.engine, error, sizeof(error))); CHECK(tick(&f, 3));
    CHECK(f.snapshot->rows[0].feedback == MONITOR_FEEDBACK_IGNORE);
    CHECK(f.snapshot->rows[0].metric[0].window_end_utc_100ns == 0);
    f.now += 3600 * MONITOR_TICKS_PER_SECOND; CHECK(tick(&f, 3));
    CHECK(f.snapshot->rows[0].feedback == MONITOR_FEEDBACK_CANCEL);
    cleanup(&f);
}
static void test_coverage_null_rollback(void)
{
    Fixture f;
    unsigned i;
    int64_t runs, raw;
    init(&f, 1); CHECK(tick(&f, 0));
    f.rows[0].cpu_percent = 0; f.rows[0].io_read_bytes_per_second = 0;
    CHECK(tick(&f, 3)); CHECK(scalar(f.engine, "SELECT count(*) FROM raw WHERE cpu=0 AND io_read=0") == 1);
    f.rows[0].valid_fields &= ~(unsigned)(RESOURCE_VALID_CPU | RESOURCE_VALID_IO_READ | RESOURCE_VALID_MEMORY);
    CHECK(tick(&f, 3)); CHECK(scalar(f.engine, "SELECT count(*) FROM raw WHERE cpu IS NULL AND io_read IS NULL AND ram IS NULL") == 1);
    for (i = 0; i < 18; ++i) CHECK(tick(&f, 3));
    CHECK(f.snapshot->rows[0].metric[0].state == MONITOR_UNAVAILABLE);
    CHECK(f.snapshot->rows[0].metric[1].state == MONITOR_UNAVAILABLE);
    CHECK(f.snapshot->rows[0].metric[3].state == MONITOR_LEARNING);
    runs = scalar(f.engine, "SELECT count(*) FROM collection_run"); raw = scalar(f.engine, "SELECT count(*) FROM raw");
    CHECK(sql(f.engine, "CREATE TRIGGER failraw BEFORE INSERT ON raw BEGIN SELECT RAISE(ABORT,'synthetic rollback'); END"));
    CHECK(!tick(&f, 3)); CHECK(f.snapshot == NULL && f.engine->count == 0);
    CHECK(scalar(f.engine, "SELECT count(*) FROM collection_run") == runs);
    CHECK(scalar(f.engine, "SELECT count(*) FROM raw") == raw);
    CHECK(sql(f.engine, "DROP TRIGGER failraw")); CHECK(tick(&f, 3));
    CHECK(f.snapshot->rows[0].metric[3].window_end_utc_100ns == 0);
    CHECK(monitor_record_failure(f.engine, f.now, 5)); CHECK(f.engine->count == 0);
    CHECK(scalar(f.engine, "SELECT count(*) FROM collection_run WHERE status=5 AND row_count=0") == 1);
    cleanup(&f);
}
static void test_caps_schema_busy(void)
{
    Fixture f;
    sqlite3 *other = NULL;
    MonitorEngine *rejected = NULL;
    char error[256];
    uint32_t os_error;
    char *path;
    init(&f, 1); CHECK(tick(&f, 0)); seed(&f, 300, 0, 1, 1); window(&f);
    CHECK(f.snapshot->rows[0].metric[0].count == 192);
    CHECK(f.snapshot->rows[0].metric[1].state == MONITOR_LEARNING && !f.snapshot->rows[0].metric[1].count);
    CHECK(sqlite3_open(f.file, &other) == SQLITE_OK);
    CHECK(sqlite3_exec(other, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!tick(&f, 3)); CHECK(f.engine->count == 0);
    CHECK(sqlite3_exec(other, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK); sqlite3_close(other);
    CHECK(tick(&f, 3));
    CHECK(!monitor_open(f.file, 0, &rejected, error, sizeof(error)) && !rejected);
    CHECK(sql(f.engine, "PRAGMA user_version=999"));
    CHECK(!monitor_open(f.file, 1, &rejected, error, sizeof(error)) && !rejected);
    CHECK(scalar(f.engine, "PRAGMA user_version") == 999);
    path = monitor_query_path(GetCurrentProcessId(), 1, &os_error);
    CHECK(path == NULL && os_error == ERROR_INVALID_DATA); free(path);
    cleanup(&f);
}
static void test_multi_instance_minute_depth(void)
{
    Fixture f;
    sqlite3_stmt *w = NULL, *metric = NULL;
    sqlite3_int64 run, program;
    unsigned day, minute, process;
    MonitorMetricReport *report;
    init(&f, 1); CHECK(tick(&f, 0));
    run = scalar(f.engine, "SELECT max(id) FROM collection_run");
    program = f.snapshot->rows[0].program_id;
    CHECK(sql(f.engine, "BEGIN IMMEDIATE"));
    CHECK(sqlite3_prepare_v2(f.engine->db,
        "INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) VALUES(?,?,?,?,?,?,60,1)",
        -1, &w, NULL) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(f.engine->db,
        "INSERT INTO aggregate_metric VALUES(?,0,10,60,20,1,1,'multi-instance seed',0,0,0,NULL,NULL,NULL,0)",
        -1, &metric, NULL) == SQLITE_OK);
    /* Same exact path: 32 PID+creation instances, 20 distinct completed minutes
     * on each of three prior UTC dates. Latest 64 rows/day cover only 2 minutes.
     * There is sufficient real represented time; concurrency cannot create it. */
    for (day = 1; day <= 3; ++day) for (minute = 1; minute <= 20; ++minute)
        for (process = 0; process < 32; ++process) {
            uint64_t end = (f.now / MONITOR_DAY_TICKS - day) * MONITOR_DAY_TICKS +
                ((uint64_t)minute * 60 + process) * MONITOR_TICKS_PER_SECOND;
            sqlite3_bind_int64(w, 1, run); sqlite3_bind_int64(w, 2, program);
            sqlite3_bind_int64(w, 3, 1000 + process);
            sqlite3_bind_int64(w, 4, (sqlite3_int64)(f.now - 10 * MONITOR_DAY_TICKS + process * MONITOR_TICKS_PER_SECOND));
            sqlite3_bind_int64(w, 5, (sqlite3_int64)(end - 60 * MONITOR_TICKS_PER_SECOND));
            sqlite3_bind_int64(w, 6, (sqlite3_int64)end);
            CHECK(sqlite3_step(w) == SQLITE_DONE); sqlite3_reset(w);
            sqlite3_bind_int64(metric, 1, sqlite3_last_insert_rowid(f.engine->db));
            CHECK(sqlite3_step(metric) == SQLITE_DONE); sqlite3_reset(metric);
        }
    sqlite3_finalize(w); sqlite3_finalize(metric); CHECK(sql(f.engine, "COMMIT"));
    CHECK(scalar(f.engine, "SELECT count(*) FROM aggregate_window") == 1920);
    CHECK(scalar(f.engine, "SELECT count(DISTINCT pid) FROM aggregate_window") == 32);
    CHECK(scalar(f.engine, "SELECT count(DISTINCT program_id) FROM aggregate_window") == 1);
    CHECK(scalar(f.engine, "SELECT count(DISTINCT end_utc/600000000) FROM aggregate_window") == 60);
    window(&f); report = &f.snapshot->rows[0].metric[MONITOR_CPU];
    printf("MULTI_INSTANCE_DEPTH available_windows=1920 paths=1 instances=32 dates=3 distinct_minutes=60 "
        "selected_n=%u selected_dates=%u selected_minutes=%u state=%s median=%.3f threshold=%.3f\n",
        report->count, report->days, report->minutes, monitor_state_name(report->state), report->median, report->threshold);
    CHECK(report->count >= 60 && report->count <= 896);
    CHECK(report->days == 3);
    CHECK(report->minutes == 60);
    CHECK(report->state == MONITOR_NORMAL);
    cleanup(&f);
}
static void insert_representative_variant(Fixture *f, sqlite3_int64 run, sqlite3_int64 program,
    uint64_t end, unsigned version, const double values[4], unsigned eligible, unsigned nulls)
{
    sqlite3_stmt *w = NULL, *metric = NULL;
    sqlite3_int64 id;
    unsigned m;
    CHECK(sqlite3_prepare_v2(f->engine->db,
        "INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) VALUES(?,?,2000,1,?,?,60,?)",
        -1, &w, NULL) == SQLITE_OK);
    sqlite3_bind_int64(w, 1, run); sqlite3_bind_int64(w, 2, program);
    sqlite3_bind_int64(w, 3, (sqlite3_int64)(end - 60 * MONITOR_TICKS_PER_SECOND)); sqlite3_bind_int64(w, 4, (sqlite3_int64)end);
    sqlite3_bind_int(w, 5, (int)version); CHECK(sqlite3_step(w) == SQLITE_DONE); sqlite3_finalize(w);
    id = sqlite3_last_insert_rowid(f->engine->db);
    CHECK(sqlite3_prepare_v2(f->engine->db,
        "INSERT INTO aggregate_metric VALUES(?,?,?,60,20,?,1,'representative seed',0,0,0,NULL,NULL,NULL,0)",
        -1, &metric, NULL) == SQLITE_OK);
    for (m = 0; m < 4; ++m) {
        sqlite3_bind_int64(metric, 1, id); sqlite3_bind_int(metric, 2, (int)m);
        if (nulls & (1u << m)) sqlite3_bind_null(metric, 3); else sqlite3_bind_double(metric, 3, values[m]);
        sqlite3_bind_int(metric, 4, !!(eligible & (1u << m)));
        CHECK(sqlite3_step(metric) == SQLITE_DONE); sqlite3_reset(metric);
    }
    sqlite3_finalize(metric);
}
static void test_minute_representatives(void)
{
    Fixture f;
    sqlite3_int64 run, program, other;
    unsigned day, minute, m;
    const double latest[4] = { 10, 67108864, 100, 300 };
    const double older[4] = { 900, 67108864, 900, 900 };
    const double tie[4] = { 0, 0, 900, HUGE_VAL };
    const double invalid[4] = { HUGE_VAL, 0, -1, HUGE_VAL };
    const double excluded[4] = { 900, 900, 900, 900 };
    init(&f, 1); f.rows[0].cpu_percent = 0; CHECK(tick(&f, 0));
    run = scalar(f.engine, "SELECT max(id) FROM collection_run"); program = f.snapshot->rows[0].program_id;
    CHECK(sql(f.engine, "INSERT INTO program(path,name,first_seen,last_seen) VALUES('SYNTHETIC:\\other\\same.exe','same.exe',1,1)"));
    other = sqlite3_last_insert_rowid(f.engine->db);
    CHECK(sql(f.engine, "BEGIN IMMEDIATE"));
    for (day = 1; day <= 3; ++day) for (minute = 1; minute <= 20; ++minute) {
        uint64_t base = (f.now / MONITOR_DAY_TICKS - day) * MONITOR_DAY_TICKS +
            (uint64_t)minute * 60 * MONITOR_TICKS_PER_SECOND;
        insert_representative_variant(&f, run, program, base + 20 * MONITOR_TICKS_PER_SECOND, 1, latest, 15, 0);
        insert_representative_variant(&f, run, program, base + 10 * MONITOR_TICKS_PER_SECOND, 1, older, 15, 0);
        /* Higher id at equal time wins CPU=0. NULL RAM and infinite write must
         * not hide older finite values; ineligible read must not hide read=100. */
        insert_representative_variant(&f, run, program, base + 20 * MONITOR_TICKS_PER_SECOND, 1, tie, 11, 2);
        insert_representative_variant(&f, run, program, base + 30 * MONITOR_TICKS_PER_SECOND, 1, invalid, 15, 2);
        insert_representative_variant(&f, run, program, base + 40 * MONITOR_TICKS_PER_SECOND, 1, excluded, 0, 0);
        insert_representative_variant(&f, run, program, base + 50 * MONITOR_TICKS_PER_SECOND, 2, excluded, 15, 0);
        insert_representative_variant(&f, run, other, base + 55 * MONITOR_TICKS_PER_SECOND, 1, excluded, 15, 0);
    }
    CHECK(sql(f.engine, "COMMIT")); window(&f);
    for (m = 0; m < 4; ++m) {
        CHECK(f.snapshot->rows[0].metric[m].count == 60);
        CHECK(f.snapshot->rows[0].metric[m].days == 3 && f.snapshot->rows[0].metric[m].minutes == 60);
        CHECK(f.snapshot->rows[0].metric[m].state == MONITOR_NORMAL);
    }
    CHECK(f.snapshot->rows[0].metric[0].median == 0 && f.snapshot->rows[0].metric[0].threshold == 5);
    CHECK(f.snapshot->rows[0].metric[1].median == 67108864);
    CHECK(f.snapshot->rows[0].metric[2].median == 100);
    CHECK(f.snapshot->rows[0].metric[3].median == 300);
    printf("REPRESENTATIVES n=%u dates=%u minutes=%u CPU_median=%.0f RAM_median=%.0f read_median=%.0f write_median=%.0f\n",
        f.snapshot->rows[0].metric[0].count, f.snapshot->rows[0].metric[0].days, f.snapshot->rows[0].metric[0].minutes,
        f.snapshot->rows[0].metric[0].median, f.snapshot->rows[0].metric[1].median,
        f.snapshot->rows[0].metric[2].median, f.snapshot->rows[0].metric[3].median);
    cleanup(&f);
}
static void benchmark_minute_selection(unsigned density)
{
    Fixture f;
    sqlite3_int64 run, program;
    unsigned day, minute, process, repetition, i;
    LARGE_INTEGER frequency, begin, end;
    double total = 0, maximum = 0, minimum = HUGE_VAL;
    const double values[4] = { 10, 67108864, 0, 0 };
    sqlite3_stmt *plan = NULL;
    char explain[2048];
    init(&f, 1); CHECK(tick(&f, 0));
    run = scalar(f.engine, "SELECT max(id) FROM collection_run"); program = f.snapshot->rows[0].program_id;
    CHECK(sql(f.engine, "BEGIN IMMEDIATE"));
    for (day = 1; day <= 3; ++day) for (minute = 1; minute <= 64; ++minute)
        for (process = 0; process < density; ++process) {
            uint64_t finish = (f.now / MONITOR_DAY_TICKS - day) * MONITOR_DAY_TICKS +
                ((uint64_t)minute * 60 + process) * MONITOR_TICKS_PER_SECOND;
            insert_representative_variant(&f, run, program, finish, 1, values, 15, 0);
        }
    CHECK(sql(f.engine, "COMMIT"));
    snprintf(explain, sizeof(explain), "EXPLAIN QUERY PLAN %s", sqlite3_sql(f.engine->baseline_query));
    CHECK(sqlite3_prepare_v2(f.engine->db, explain, -1, &plan, NULL) == SQLITE_OK);
    while (sqlite3_step(plan) == SQLITE_ROW) printf("BENCH_PLAN density=%u %s\n", density, sqlite3_column_text(plan, 3));
    sqlite3_finalize(plan); QueryPerformanceFrequency(&frequency);
    for (repetition = 0; repetition < 5; ++repetition) {
        double elapsed;
        for (i = 0; i < 19; ++i) CHECK(tick(&f, 3));
        sqlite3_stmt_status(f.engine->baseline_query, SQLITE_STMTSTATUS_VM_STEP, 1);
        sqlite3_stmt_status(f.engine->baseline_query, SQLITE_STMTSTATUS_FULLSCAN_STEP, 1);
        sqlite3_stmt_status(f.engine->baseline_query, SQLITE_STMTSTATUS_SORT, 1);
        QueryPerformanceCounter(&begin); CHECK(tick(&f, 3)); QueryPerformanceCounter(&end);
        elapsed = (double)(end.QuadPart - begin.QuadPart) * 1000 / (double)frequency.QuadPart;
        total += elapsed; if (elapsed > maximum) maximum = elapsed; if (elapsed < minimum) minimum = elapsed;
        printf("BENCH_CLOSE density=%u iteration=%u ms=%.3f n=%u days=%u minutes=%u state=%s vm_steps=%d fullscan_steps=%d sorts=%d\n",
            density, repetition + 1, elapsed, f.snapshot->rows[0].metric[0].count, f.snapshot->rows[0].metric[0].days,
            f.snapshot->rows[0].metric[0].minutes, monitor_state_name(f.snapshot->rows[0].metric[0].state),
            sqlite3_stmt_status(f.engine->baseline_query, SQLITE_STMTSTATUS_VM_STEP, 0),
            sqlite3_stmt_status(f.engine->baseline_query, SQLITE_STMTSTATUS_FULLSCAN_STEP, 0),
            sqlite3_stmt_status(f.engine->baseline_query, SQLITE_STMTSTATUS_SORT, 0));
    }
    printf("BENCH_SUMMARY SYNTHETIC density=%u history_windows=%u closes=5 min_ms=%.3f mean_ms=%.3f max_ms=%.3f\n",
        density, 3 * 64 * density, minimum, total / 5, maximum);
    cleanup(&f);
}
static void test_retention(void)
{
    Fixture f;
    char query[1024];
    uint64_t old;
    init(&f, 1); CHECK(tick(&f, 0)); seed(&f, 6, 0, 15, 29);
    old = f.now - 2 * 3600 * MONITOR_TICKS_PER_SECOND;
    snprintf(query, sizeof(query), "UPDATE raw SET utc=%llu; UPDATE collection_run SET finished=%llu; INSERT INTO feedback(program_id,kind,created,expires) SELECT id,1,1,1 FROM program",
        (unsigned long long)old, (unsigned long long)(f.now - 30 * MONITOR_DAY_TICKS));
    CHECK(sql(f.engine, query)); CHECK(tick(&f, 3));
    CHECK(scalar(f.engine, "SELECT count(*) FROM raw") == 1);
    CHECK(scalar(f.engine, "SELECT count(*) FROM aggregate_window") == 0);
    CHECK(scalar(f.engine, "SELECT count(*) FROM aggregate_metric") == 0);
    CHECK(scalar(f.engine, "SELECT count(*) FROM feedback") == 0);
    CHECK(scalar(f.engine, "SELECT count(*) FROM collection_run") == 1);
    CHECK(scalar(f.engine, "SELECT count(*) FROM pragma_foreign_key_check") == 0);
    cleanup(&f);
}
int main(int argc, char **argv)
{
    printf("SYNTHETIC deterministic monitor tests; SQLite %s source=%s\n", sqlite3_libversion(), sqlite3_sourceid());
    if (argc == 2 && !strcmp(argv[1], "--baseline-benchmark")) {
        benchmark_minute_selection(1); benchmark_minute_selection(32);
        printf("monitor benchmark: %u checks, %u failures\n", checks, failures); return failures ? 1 : 0;
    }
    test_policy(); test_learning_identity(); test_past_only(); test_weights_gap_restart();
    test_coverage_null_rollback(); test_caps_schema_busy(); test_retention();
    test_multi_instance_minute_depth(); test_minute_representatives();
    printf("monitor: %u checks, %u failures\n", checks, failures); return failures ? 1 : 0;
}
