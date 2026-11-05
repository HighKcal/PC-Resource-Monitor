#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <direct.h>
#include "../support/monitor_fixture.h"

/* Independent result oracle: read the complete joined history and perform the
 * contract's filters and representative selection in C, without recursive SQL
 * or any product cache. This is a correctness test, not a timing benchmark. */
#define TICKS UINT64_C(10000000)
#define DAY (UINT64_C(86400) * TICKS)
#define MINUTE (UINT64_C(60) * TICKS)
#define ROWS 5
static unsigned checks, failures;
static const char *artifacts;
static uint64_t digest = UINT64_C(1469598103934665603);
static unsigned injected_allocations;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line=%d %s\n", __LINE__, #x); } } while (0)

/* Only detection/baseline.c is compiled with these substitutions in the
 * optional injected build. Failing cache allocation must use uncached SQL. */
static int inject_oom(void)
{ char value[2]; return GetEnvironmentVariableA("REVIEW_BASELINE_OOM", value, sizeof(value)) > 0; }
void *review_baseline_calloc(size_t n, size_t size)
{ if (inject_oom()) { ++injected_allocations; return NULL; } return calloc(n, size); }
void *review_baseline_malloc(size_t size)
{ if (inject_oom()) { ++injected_allocations; return NULL; } return malloc(size); }
void *review_baseline_realloc(void *p, size_t size)
{ if (inject_oom()) { ++injected_allocations; return NULL; } return realloc(p, size); }

typedef struct Fixture {
    MonitorEngine *e;
    ResourceData rows[ROWS];
    ResourceBatch batch;
    const char *paths[ROWS];
    unsigned streak[ROWS][4];
    uint64_t anchor;
    char file[MAX_PATH];
} Fixture;
typedef struct InterruptProbe { unsigned calls, rows; int armed, interrupted; } InterruptProbe;
static int interrupt_trace(unsigned event, void *context, void *statement, void *detail)
{
    InterruptProbe *p = context;
    const char *text = sqlite3_sql((sqlite3_stmt *)statement);
    (void)detail;
    if (text && strstr(text, "WITH RECURSIVE picks")) {
        if (event == SQLITE_TRACE_STMT) ++p->calls;
        if (event == SQLITE_TRACE_ROW && p->calls == 3 && !p->interrupted) { ++p->rows; p->armed = 1; }
    }
    return 0;
}
static int interrupt_progress(void *context)
{
    InterruptProbe *p = context;
    if (p->armed && !p->interrupted) { p->armed = 0; p->interrupted = 1; return 1; }
    return 0; /* The single interruption must not prevent ROLLBACK. */
}
static void fatal(const char *what) { printf("FATAL %s\n", what); exit(2); }
static void sql(sqlite3 *db, const char *text)
{
    if (sqlite3_exec(db, text, NULL, NULL, NULL) != SQLITE_OK) fatal(sqlite3_errmsg(db));
}
static int64_t scalar(sqlite3 *db, const char *text)
{
    sqlite3_stmt *s = NULL;
    int64_t v;
    if (sqlite3_prepare_v2(db, text, -1, &s, NULL) != SQLITE_OK || sqlite3_step(s) != SQLITE_ROW) fatal(sqlite3_errmsg(db));
    v = sqlite3_column_int64(s, 0); sqlite3_finalize(s); return v;
}
static void hash(const void *data, size_t size)
{
    const unsigned char *p = data;
    size_t i;
    for (i = 0; i < size; ++i) digest = (digest ^ p[i]) * UINT64_C(1099511628211);
}
static void hash_real(double value)
{
    uint64_t bits;
    if (isnan(value)) bits = UINT64_C(0x7ff8000000000000);
    else { if (value == 0) value = 0; memcpy(&bits, &value, sizeof(bits)); }
    hash(&bits, sizeof(bits));
}
static int compare_double(const void *a, const void *b)
{ double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static double middle(double *v, unsigned n)
{
    qsort(v, n, sizeof(*v), compare_double);
    return n % 2 ? v[n / 2] : v[n / 2 - 1] / 2 + v[n / 2] / 2;
}
static void check_near(double a, double b) { CHECK((isnan(a) && isnan(b)) || fabs(a-b) <= fmax(1, fabs(b)) * 1e-12); }

static MonitorMetricReport reference(sqlite3 *db, int64_t program, unsigned metric,
    uint64_t start, int64_t cutoff, double current, unsigned prior)
{
    static const double floor_value[] = {5, 134217728, 1048576, 1048576};
    sqlite3_stmt *s = NULL;
    MonitorMetricReport r;
    double values[896], deviations[896];
    unsigned date_counts[14] = {0}, i, n = 0;
    uint64_t date = start / DAY, last_minute = UINT64_MAX;
    int rc;
    memset(&r, 0, sizeof(r)); r.current = current;
    r.median = r.mad = r.threshold = NAN;
    if (sqlite3_prepare_v2(db,
        "SELECT w.id,w.end_utc,w.version,m.value,m.eligible FROM aggregate_window w "
        "JOIN aggregate_metric m ON m.window_id=w.id WHERE w.program_id=? AND m.metric=? "
        "ORDER BY w.end_utc DESC,w.id DESC", -1, &s, NULL) != SQLITE_OK) fatal(sqlite3_errmsg(db));
    sqlite3_bind_int64(s, 1, program); sqlite3_bind_int(s, 2, (int)metric);
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        uint64_t end = (uint64_t)sqlite3_column_int64(s, 1), age, minute;
        double v = sqlite3_column_double(s, 3);
        if (sqlite3_column_int64(s, 0) > cutoff || end >= start || end / DAY > date ||
            sqlite3_column_int(s, 2) != 1 || sqlite3_column_int(s, 4) != 1 ||
            sqlite3_column_type(s, 3) == SQLITE_NULL || !isfinite(v) || v < 0) continue;
        age = date - end / DAY; minute = end / MINUTE;
        if (age >= 14 || date_counts[age] == 64 || minute == last_minute) continue;
        CHECK(n < 896); if (n == 896) fatal("oracle capacity");
        values[n++] = v; ++date_counts[age]; last_minute = minute;
    }
    CHECK(rc == SQLITE_DONE); sqlite3_finalize(s);
    r.count = r.minutes = n;
    for (i = 0; i < 14; ++i) if (date_counts[i]) ++r.days;
    if (n) {
        r.median = middle(values, n);
        for (i = 0; i < n; ++i) deviations[i] = fabs(values[i] - r.median);
        r.mad = middle(deviations, n);
        r.threshold = r.median + fmax(fmax(3 * 1.4826 * r.mad, .5 * r.median), floor_value[metric]);
    }
    if (n < 60 || r.days < 3 || r.minutes < 60) { r.state = MONITOR_LEARNING; r.baseline_eligible = 1; }
    else if (current > r.threshold) { r.streak = prior + (prior != UINT_MAX); r.state = r.streak >= 3 ? MONITOR_CANDIDATE : MONITOR_ATTENTION; }
    else { r.state = MONITOR_NORMAL; r.baseline_eligible = 1; }
    return r;
}
static void check_report(const MonitorMetricReport *actual, const MonitorMetricReport *expected)
{
    CHECK(actual->count == expected->count); CHECK(actual->days == expected->days);
    CHECK(actual->minutes == expected->minutes); CHECK(actual->state == expected->state);
    CHECK(actual->streak == expected->streak); CHECK(actual->baseline_eligible == expected->baseline_eligible);
    check_near(actual->median, expected->median); check_near(actual->mad, expected->mad); check_near(actual->threshold, expected->threshold);
    hash(&actual->state, sizeof(actual->state)); hash(&actual->count, sizeof(actual->count));
    hash(&actual->days, sizeof(actual->days)); hash(&actual->minutes, sizeof(actual->minutes));
    hash(&actual->streak, sizeof(actual->streak)); hash(&actual->baseline_eligible, sizeof(actual->baseline_eligible));
    hash_real(actual->current); hash_real(actual->median); hash_real(actual->mad); hash_real(actual->threshold);
}
static int try_tick(Fixture *f, unsigned step, MonitorSnapshot **s)
{
    static const int64_t offsets[ROWS] = {-1200000000, 0, 1, 200, 3000};
    unsigned i;
    for (i = 0; i < ROWS; ++i) f->rows[i].measured_at_utc_100ns = f->anchor + (uint64_t)((int64_t)step * 3 * (int64_t)TICKS + offsets[i]);
    f->batch.started_at_utc_100ns = f->rows[0].measured_at_utc_100ns;
    f->batch.finished_at_utc_100ns = f->rows[ROWS-1].measured_at_utc_100ns;
    return monitor_ingest(f->e, &f->batch, f->paths, s);
}
static MonitorSnapshot *tick(Fixture *f, unsigned step)
{
    MonitorSnapshot *s = NULL;
    if (!try_tick(f, step, &s)) fatal(monitor_error(f->e));
    return s;
}
static void seed_window(Fixture *f, int64_t program, uint64_t end, unsigned duplicate, unsigned serial, unsigned version)
{
    sqlite3_stmt *w = NULL, *m = NULL;
    int64_t id;
    unsigned metric;
    if (sqlite3_prepare_v2(monitor_fixture_db(f->e),
        "INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) "
        "VALUES((SELECT min(id) FROM collection_run),?,9876,1,?,?,60,?)", -1, &w, NULL) != SQLITE_OK) fatal(sqlite3_errmsg(monitor_fixture_db(f->e)));
    sqlite3_bind_int64(w, 1, program); sqlite3_bind_int64(w, 2, (int64_t)(end - MINUTE));
    sqlite3_bind_int64(w, 3, (int64_t)end); sqlite3_bind_int(w, 4, (int)version);
    if (sqlite3_step(w) != SQLITE_DONE) fatal(sqlite3_errmsg(monitor_fixture_db(f->e)));
    id = sqlite3_last_insert_rowid(monitor_fixture_db(f->e)); sqlite3_finalize(w);
    if (sqlite3_prepare_v2(monitor_fixture_db(f->e),
        "INSERT INTO aggregate_metric(window_id,metric,value,covered,valid_count,eligible,state,reason,n,days,minutes,streak) "
        "VALUES(?,?,?,60,20,?,1,'independent fixture',0,0,0,0)", -1, &m, NULL) != SQLITE_OK) fatal(sqlite3_errmsg(monitor_fixture_db(f->e)));
    for (metric = 0; metric < 4; ++metric) {
        unsigned mode = duplicate == 3 ? (metric + serial) % 6 : 0;
        double v = metric == 0 ? 1 + serial % 9 + duplicate : metric == 1 ? (1 + serial % 7 + duplicate) * 1048576.0 : metric == 2 ? 0 : (serial % 11 + duplicate) * 256.0;
        sqlite3_bind_int64(m, 1, id); sqlite3_bind_int(m, 2, (int)metric);
        if (mode == 2) v = -1;
        if (mode == 4) v = HUGE_VAL;
        if (mode == 5) v = NAN; /* SQLite stores a bound NaN as SQL NULL. */
        if (mode == 3) sqlite3_bind_null(m, 3); else sqlite3_bind_double(m, 3, v);
        sqlite3_bind_int(m, 4, mode != 1);
        if (sqlite3_step(m) != SQLITE_DONE) fatal(sqlite3_errmsg(monitor_fixture_db(f->e)));
        sqlite3_reset(m); sqlite3_clear_bindings(m);
    }
    sqlite3_finalize(m);
}
static void run_case(unsigned kind)
{
    Fixture f;
    MonitorSnapshot *s;
    char error[256];
    unsigned i, day, minute, duplicate, step;
    int64_t program;
    memset(&f, 0, sizeof(f));
    snprintf(f.file, sizeof(f.file), "%s/oracle-%lu-%u.sqlite", artifacts, (unsigned long)GetCurrentProcessId(), kind);
    CHECK(GetFileAttributesA(f.file) == INVALID_FILE_ATTRIBUTES);
    if (!monitor_open(f.file, 1, &f.e, error, sizeof(error))) fatal(error);
    f.anchor = UINT64_C(134058240000000000) / DAY * DAY + (kind == 0 ? 43200 : kind == 1 ? 30 : 0) * TICKS;
    f.batch.items = f.rows; f.batch.count = ROWS; f.batch.logical_processor_count = 8;
    for (i = 0; i < ROWS; ++i) {
        ResourceData *r = &f.rows[i];
        r->pid = 4400 + i; r->creation_time_utc_100ns = f.anchor - DAY + i;
        r->process_name_utf8 = "oracle.exe"; f.paths[i] = i == ROWS-1 ? "SYNTHETIC:\\oracle\\other.exe" : "SYNTHETIC:\\oracle\\same.exe";
        r->valid_fields = RESOURCE_VALID_NAME | RESOURCE_VALID_IDENTITY | RESOURCE_VALID_CPU | RESOURCE_VALID_MEMORY |
            RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL;
        r->observation_interval_seconds = r->cpu_interval_seconds = r->io_interval_seconds = 3;
        r->cpu_percent = 40; r->working_set_bytes = UINT64_C(536870912);
        r->io_read_bytes_per_second = r->io_write_bytes_per_second = 20971520;
    }
    s = tick(&f, 0); program = s->rows[1].program_id; monitor_snapshot_free(s);
    sql(monitor_fixture_db(f.e), "BEGIN IMMEDIATE");
    if (kind == 0 || kind == 2) {
        for (day = 0; day < 15; ++day) for (minute = 1; minute <= 70; ++minute) for (duplicate = 0; duplicate < 4; ++duplicate) {
            uint64_t end = (f.anchor / DAY - day) * DAY + minute * MINUTE + (duplicate ? duplicate == 3 ? 20 : 10 : 0) * TICKS;
            seed_window(&f, program, end, duplicate, minute + day, 1);
        }
        seed_window(&f, program, f.anchor-1, 0, 1999, 2);
    } else {
        /* 59 representatives makes exact-start and same-cycle leakage change
         * the learning gate. A 29-low/30-high distribution also makes each
         * boundary representative change the median, exposing wrong reuse
         * even when the representative count remains unchanged. */
        for (i = 0; i < 59; ++i) {
            char update[256];
            seed_window(&f, program, (f.anchor / DAY - 1 - i % 3) * DAY + (i / 3 + 1) * MINUTE, 0, i, 1);
            snprintf(update, sizeof(update), "UPDATE aggregate_metric SET value=%u * CASE metric WHEN 0 THEN 1 ELSE 1048576 END WHERE window_id=%lld",
                i < 29 ? 1u : 100u, (long long)sqlite3_last_insert_rowid(monitor_fixture_db(f.e)));
            sql(monitor_fixture_db(f.e), update);
        }
    }
    seed_window(&f, program, f.anchor - 1, 0, 100, 1);
    seed_window(&f, program, f.anchor, 1, 101, 1);
    seed_window(&f, program, f.anchor + 1, 2, 102, 1);
    seed_window(&f, program, f.anchor + 100, 3, 103, 1);
    seed_window(&f, program, f.anchor + MINUTE, 0, 104, 1);
    sql(monitor_fixture_db(f.e), "COMMIT");
    for (step = 1; step <= 80; ++step) {
        int64_t cutoff = scalar(monitor_fixture_db(f.e), "SELECT coalesce(max(id),0) FROM aggregate_window");
        if ((kind == 0 || kind == 2) && step == 40) {
            char trigger[400];
            InterruptProbe probe;
            int64_t raws = scalar(monitor_fixture_db(f.e), "SELECT count(*) FROM raw");
            int64_t runs = scalar(monitor_fixture_db(f.e), "SELECT count(*) FROM collection_run");
            memset(&probe, 0, sizeof(probe));
            if (kind == 0) {
                snprintf(trigger, sizeof(trigger), "CREATE TRIGGER fail_third_window BEFORE INSERT ON aggregate_metric "
                    "WHEN (SELECT max(id) FROM aggregate_window)>%lld BEGIN SELECT RAISE(ABORT,'review rollback after cached selections'); END", (long long)(cutoff + 2));
                sql(monitor_fixture_db(f.e), trigger);
            } else {
                CHECK(sqlite3_trace_v2(monitor_fixture_db(f.e), SQLITE_TRACE_STMT | SQLITE_TRACE_ROW, interrupt_trace, &probe) == SQLITE_OK);
                sqlite3_progress_handler(monitor_fixture_db(f.e), 1, interrupt_progress, &probe);
            }
            s = NULL;
            CHECK(!try_tick(&f, step, &s)); CHECK(s == NULL); CHECK(monitor_fixture_instance_count(f.e) == 0);
            if (kind == 2) {
                sqlite3_progress_handler(monitor_fixture_db(f.e), 0, NULL, NULL);
                CHECK(sqlite3_trace_v2(monitor_fixture_db(f.e), 0, NULL, NULL) == SQLITE_OK);
                CHECK(probe.calls == 3); CHECK(probe.rows > 0); CHECK(probe.interrupted);
                CHECK(strstr(monitor_error(f.e), "baseline query") != NULL);
                printf("SELECT_INTERRUPT completed_day_queries=2 partial_day_rows=%u one_shot=%d\n", probe.rows, probe.interrupted);
            }
            CHECK(scalar(monitor_fixture_db(f.e), "SELECT coalesce(max(id),0) FROM aggregate_window") == cutoff);
            CHECK(scalar(monitor_fixture_db(f.e), "SELECT count(*) FROM raw") == raws);
            CHECK(scalar(monitor_fixture_db(f.e), "SELECT count(*) FROM collection_run") == runs);
            if (kind == 0) sql(monitor_fixture_db(f.e), "DROP TRIGGER fail_third_window");
            memset(f.streak, 0, sizeof(f.streak)); s = tick(&f, step);
            CHECK(s->completed_windows == 0); monitor_snapshot_free(s); continue;
        }
        s = tick(&f, step);
        if (step % 20 == 0) {
            CHECK(s->completed_windows == ROWS);
            if (kind == 1 && step == 20) {
                CHECK(s->rows[0].metric[0].count == 59);
                CHECK(s->rows[1].metric[0].count == 60);
                CHECK(s->rows[1].metric[0].median != s->rows[2].metric[0].median);
                CHECK(s->rows[2].metric[0].median != s->rows[3].metric[0].median);
            }
            for (i = 0; i < ROWS; ++i) {
                unsigned metric;
                for (metric = 0; metric < 4; ++metric) {
                    const MonitorMetricReport *actual = &s->rows[i].metric[metric];
                    MonitorMetricReport expected = reference(monitor_fixture_db(f.e), s->rows[i].program_id, metric,
                        actual->window_start_utc_100ns, cutoff, actual->current, f.streak[i][metric]);
                    check_report(actual, &expected); f.streak[i][metric] = expected.streak;
                }
            }
            printf("ORACLE case=%u close=%u same-n=%u/%u/%u/%u other-n=%u\n", kind, step / 20,
                s->rows[0].metric[0].count, s->rows[1].metric[0].count,
                s->rows[2].metric[0].count, s->rows[3].metric[0].count, s->rows[4].metric[0].count);
        }
        monitor_snapshot_free(s);
    }
    /* End a populated epoch, expire all history through ordinary ingest trim,
     * then close a new window. Old cached values must not survive reset/trim. */
    monitor_reset(f.e); memset(f.streak, 0, sizeof(f.streak)); f.anchor += 29 * DAY;
    s = tick(&f, 0); CHECK(s->completed_windows == 0); monitor_snapshot_free(s);
    for (step = 1; step <= 20; ++step) {
        int64_t cutoff = scalar(monitor_fixture_db(f.e), "SELECT coalesce(max(id),0) FROM aggregate_window");
        s = tick(&f, step);
        if (step == 20) {
            CHECK(s->completed_windows == ROWS);
            for (i = 0; i < ROWS; ++i) {
                unsigned metric;
                for (metric = 0; metric < 4; ++metric) {
                    const MonitorMetricReport *actual = &s->rows[i].metric[metric];
                    MonitorMetricReport expected = reference(monitor_fixture_db(f.e), s->rows[i].program_id, metric,
                        actual->window_start_utc_100ns, cutoff, actual->current, 0);
                    check_report(actual, &expected); CHECK(actual->count == 0);
                }
            }
            CHECK(scalar(monitor_fixture_db(f.e), "SELECT count(*) FROM aggregate_window") == ROWS);
        }
        monitor_snapshot_free(s);
    }
    CHECK(scalar(monitor_fixture_db(f.e), "SELECT count(*) FROM pragma_foreign_key_check") == 0);
    monitor_close(f.e); f.e = NULL;
    if (!monitor_open(f.file, 1, &f.e, error, sizeof(error))) fatal(error);
    CHECK(monitor_fixture_instance_count(f.e) == 0); monitor_close(f.e);
    printf("ARTIFACT %s\n", f.file);
}
static void cache_pressure(void)
{
    enum { PROGRAMS = 81, INSTANCES = PROGRAMS + 1 };
    Fixture f;
    ResourceData rows[INSTANCES];
    ResourceBatch batch;
    const char *paths[INSTANCES];
    char path_text[INSTANCES][96], error[256];
    int64_t programs[INSTANCES], cutoff = 0;
    MonitorSnapshot *s = NULL;
    unsigned i, day, step;
    memset(&f, 0, sizeof(f)); memset(rows, 0, sizeof(rows)); memset(&batch, 0, sizeof(batch));
    snprintf(f.file, sizeof(f.file), "%s/pressure-%lu.sqlite", artifacts, (unsigned long)GetCurrentProcessId());
    CHECK(GetFileAttributesA(f.file) == INVALID_FILE_ATTRIBUTES);
    if (!monitor_open(f.file, 1, &f.e, error, sizeof(error))) fatal(error);
    f.anchor = UINT64_C(134058240000000000) / DAY * DAY + 43200 * TICKS;
    batch.items = rows; batch.count = INSTANCES; batch.logical_processor_count = 8;
    for (i = 0; i < INSTANCES; ++i) {
        ResourceData *r = &rows[i];
        snprintf(path_text[i], sizeof(path_text[i]), "SYNTHETIC:\\pressure\\program-%u.exe", i == PROGRAMS ? 0 : i);
        paths[i] = path_text[i]; r->pid = 8800 + i; r->creation_time_utc_100ns = f.anchor - DAY + i;
        r->process_name_utf8 = "pressure.exe";
        r->valid_fields = RESOURCE_VALID_NAME | RESOURCE_VALID_IDENTITY | RESOURCE_VALID_CPU | RESOURCE_VALID_MEMORY |
            RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL;
        r->observation_interval_seconds = r->cpu_interval_seconds = r->io_interval_seconds = 3;
        r->cpu_percent = 40; r->working_set_bytes = UINT64_C(536870912);
        r->io_read_bytes_per_second = r->io_write_bytes_per_second = 20971520;
        r->measured_at_utc_100ns = f.anchor;
    }
    batch.started_at_utc_100ns = batch.finished_at_utc_100ns = f.anchor;
    if (!monitor_ingest(f.e, &batch, paths, &s)) fatal(monitor_error(f.e));
    for (i = 0; i < INSTANCES; ++i) programs[i] = s->rows[i].program_id;
    CHECK(programs[PROGRAMS] == programs[0]); monitor_snapshot_free(s); s = NULL;
    sql(monitor_fixture_db(f.e), "BEGIN IMMEDIATE");
    for (i = 0; i < PROGRAMS; ++i) for (day = 0; day < 14; ++day)
        seed_window(&f, programs[i], (f.anchor / DAY - day) * DAY + MINUTE, 0, i + day, 1);
    sql(monitor_fixture_db(f.e), "COMMIT");
    for (step = 1; step <= 20; ++step) {
        cutoff = scalar(monitor_fixture_db(f.e), "SELECT coalesce(max(id),0) FROM aggregate_window");
        for (i = 0; i < INSTANCES; ++i) rows[i].measured_at_utc_100ns = f.anchor + step * 3 * TICKS;
        batch.started_at_utc_100ns = batch.finished_at_utc_100ns = rows[0].measured_at_utc_100ns;
        if (!monitor_ingest(f.e, &batch, paths, &s)) fatal(monitor_error(f.e));
        if (step == 20) {
            CHECK(s->completed_windows == INSTANCES);
            for (i = 0; i < INSTANCES; ++i) {
                unsigned metric;
                for (metric = 0; metric < 4; ++metric) {
                    const MonitorMetricReport *actual = &s->rows[i].metric[metric];
                    MonitorMetricReport expected = reference(monitor_fixture_db(f.e), programs[i], metric,
                        actual->window_start_utc_100ns, cutoff, actual->current, 0);
                    check_report(actual, &expected); CHECK(actual->count == 14);
                }
            }
        }
        monitor_snapshot_free(s); s = NULL;
    }
    CHECK(scalar(monitor_fixture_db(f.e), "SELECT count(*) FROM pragma_foreign_key_check") == 0);
    monitor_close(f.e);
    printf("PRESSURE distinct_programs=%u day_keys=%u configured_cache_entries=4096 trailing_first_program_repeat=1\n", PROGRAMS, PROGRAMS * 4 * 14);
    printf("ARTIFACT %s\n", f.file);
}
int main(int argc, char **argv)
{
    unsigned i;
    artifacts = argc > 1 ? argv[1] : "build-review-performance";
    (void)_mkdir(artifacts);
    for (i = 0; i < 3; ++i) run_case(i);
    cache_pressure();
    if (inject_oom()) CHECK(injected_allocations > 0);
    printf("RESULT checks=%u failures=%u digest=%016llx injected_allocations=%u SQLite=%s\n", checks, failures,
        (unsigned long long)digest, injected_allocations, sqlite3_libversion());
    return failures ? 1 : 0;
}
