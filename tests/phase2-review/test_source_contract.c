/* Independent C-stage contract tests. No engine/store layouts or product .c
 * textual includes. Synthetic DB access is confined to the test-only fixture.
 * Live original snapshots are retained; freed/tampered pointers are not inputs.
 * Build the real storage/monitor_source.c with this file's allocator wrappers. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "experiment.h"
#include "monitor_source.h"
#include "monitor_fixture.h"
#include "experiment_fixture.h"
#include "sqlite3.h"

#define REVIEW_TICKS UINT64_C(10000000)
static unsigned checks, failures, serial, source_oom_hits;
static int fail_source_allocation;
static void *source_owned[64];
static size_t source_owned_count;
static const char *directory;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line=%d %s\n", __LINE__, #x); } } while (0)

static void *source_allocate(size_t n, size_t size, int zero)
{
    void *p;
    if (fail_source_allocation) { fail_source_allocation = 0; ++source_oom_hits; return NULL; }
    p = zero ? calloc(n, size) : malloc(size);
    if (p) {
        if (source_owned_count == sizeof(source_owned) / sizeof(source_owned[0])) exit(3);
        source_owned[source_owned_count++] = p;
    }
    return p;
}
void *review_source_malloc(size_t size) { return source_allocate(1, size, 0); }
void *review_source_calloc(size_t n, size_t size) { return source_allocate(n, size, 1); }
void review_source_free(void *p)
{
    size_t i;
    if (p) for (i = 0; i < source_owned_count; ++i) if (source_owned[i] == p) {
        source_owned[i] = source_owned[--source_owned_count]; break;
    }
    free(p);
}

typedef struct Fixture {
    MonitorEngine *engine;
    Experiment *experiment;
    ResourceData rows[2];
    ResourceBatch batch;
    const char *paths[2];
    char source[1024], sidecar[1024];
    uint64_t anchor;
} Fixture;

static int64_t scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL;
    int64_t value = -1;
    CHECK(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
    if (s && sqlite3_step(s) == SQLITE_ROW) value = sqlite3_column_int64(s, 0); else CHECK(0);
    sqlite3_finalize(s); return value;
}
static void sql(sqlite3 *db, const char *text)
{ CHECK(sqlite3_exec(db, text, NULL, NULL, NULL) == SQLITE_OK); }
static void initialize(Fixture *f, int prototype)
{
    char error[512];
    size_t i;
    memset(f, 0, sizeof(*f));
    snprintf(f->source, sizeof(f->source), "%s/source-%u.sqlite", directory, ++serial);
    snprintf(f->sidecar, sizeof(f->sidecar), "%s/sidecar-%u.sqlite", directory, serial);
    CHECK(GetFileAttributesA(f->source) == INVALID_FILE_ATTRIBUTES);
    if (prototype) {
        CHECK(experiment_create(f->source, f->sidecar, 1, &f->experiment, error, sizeof(error)));
        if (!f->experiment) { printf("CREATE %s\n", error); exit(2); }
        f->engine = experiment_fixture_core(f->experiment);
    } else {
        CHECK(monitor_open(f->source, 1, &f->engine, error, sizeof(error)));
        if (!f->engine) { printf("OPEN %s\n", error); exit(2); }
    }
    f->anchor = UINT64_C(134058672000000000);
    f->batch.items = f->rows; f->batch.count = 2; f->batch.logical_processor_count = 8;
    for (i = 0; i < 2; ++i) {
        ResourceData *r = &f->rows[i];
        r->pid = 22300 + (uint32_t)i; r->creation_time_utc_100ns = f->anchor - REVIEW_TICKS + i;
        r->process_name_utf8 = i ? "second.exe" : "first.exe";
        r->valid_fields = RESOURCE_VALID_IDENTITY | RESOURCE_VALID_NAME | RESOURCE_VALID_CPU | RESOURCE_VALID_MEMORY |
            RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL;
        r->observation_interval_seconds = r->cpu_interval_seconds = r->io_interval_seconds = 3;
        r->cpu_percent = 10 + 20 * (double)i; r->working_set_bytes = UINT64_C(1000000) * (i + 1);
        r->io_read_bytes_per_second = 100 + (double)i; r->io_write_bytes_per_second = 200 + (double)i;
        f->paths[i] = i ? "SYNTHETIC:\\source\\second.exe" : "SYNTHETIC:\\source\\first.exe";
    }
}
static void set_time(Fixture *f, unsigned step)
{
    size_t i;
    for (i = 0; i < 2; ++i) f->rows[i].measured_at_utc_100ns = f->anchor + step * 3 * REVIEW_TICKS;
    f->batch.started_at_utc_100ns = f->batch.finished_at_utc_100ns = f->rows[0].measured_at_utc_100ns;
}
static MonitorSnapshot *tick(Fixture *f, unsigned step, MonitorCommitRef *ref)
{
    MonitorSnapshot *s = NULL;
    set_time(f, step);
    CHECK(monitor_ingest_with_commit_ref(f->engine, &f->batch, f->paths, &s, ref));
    if (!s) { printf("INGEST %s\n", monitor_error(f->engine)); exit(2); }
    return s;
}
static void close_fixture(Fixture *f)
{
    if (f->experiment) experiment_close(f->experiment); else monitor_close(f->engine);
    memset(f, 0, sizeof(*f));
    CHECK(source_owned_count == 0);
}
static void idle(Fixture *f)
{
    sqlite3 *db = monitor_fixture_db(f->engine);
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_get_autocommit(db));
    while ((s = sqlite3_next_stmt(db, s)) != NULL) CHECK(!sqlite3_stmt_busy(s));
}
static void rejected(MonitorEngine *engine, const MonitorCommitRef *ref, const MonitorSnapshot *snapshot)
{
    MonitorCommittedWindows result = {0};
    CHECK(!monitor_source_read_committed(engine, ref, snapshot, &result));
    CHECK(result.items == NULL && result.count == 0);
    CHECK(source_owned_count == 0);
}
static void accepted(MonitorEngine *engine, const MonitorCommitRef *ref, const MonitorSnapshot *snapshot)
{
    MonitorCommittedWindows result = {0};
    size_t i;
    CHECK(monitor_source_read_committed(engine, ref, snapshot, &result));
    CHECK(result.count == snapshot->completed_windows);
    for (i = 0; i < result.count; ++i) {
        CHECK(result.items[i].window_id > ref->history_cutoff);
        CHECK(result.items[i].run_id == ref->run_id);
        CHECK(result.items[i].row_index < snapshot->count);
        if (result.items[i].row_index < snapshot->count)
            CHECK(result.items[i].row == &snapshot->rows[result.items[i].row_index]);
        CHECK(result.items[i].match_flags == 0);
    }
    if (!result.count) CHECK(result.items == NULL);
    monitor_source_windows_free(&result);
    CHECK(source_owned_count == 0);
}

static void reference_identity(void)
{
    Fixture a, b;
    MonitorCommitRef ar, br, latest;
    MonitorSnapshot *as, *bs, *next;
    MonitorStorageStats stats;
    unsigned i;
    initialize(&a, 0); initialize(&b, 0);
    as = tick(&a, 0, &ar); bs = tick(&b, 0, &br);
    CHECK(ar.engine_cookie != 0 && ar.engine_cookie != br.engine_cookie);
    CHECK(ar.sequence == as->sequence && ar.run_id > 0 && ar.session_id > 0);
    CHECK(as->completed_windows == 0);
    accepted(a.engine, &ar, as);
    CHECK(!monitor_source_read_committed(a.engine, &ar, as, NULL));
    rejected(a.engine, NULL, as); rejected(a.engine, &ar, NULL);
    CHECK(monitor_storage_stats(a.engine, &stats));
    accepted(a.engine, &ar, as); /* Read-only diagnostics do not mutate receipt. */
    rejected(b.engine, &ar, as); rejected(a.engine, &br, bs);
    rejected(a.engine, &ar, bs); rejected(b.engine, &br, as);
    next = tick(&a, 1, &latest);
    rejected(a.engine, &ar, as); rejected(a.engine, &latest, as);
    rejected(a.engine, &ar, next); accepted(a.engine, &latest, next);
    monitor_snapshot_free(as); as = next;
    for (i = 2; i <= 20; ++i) { next = tick(&a, i, &latest); monitor_snapshot_free(as); as = next; }
    CHECK(as->completed_windows == 2); accepted(a.engine, &latest, as);
    idle(&a); monitor_snapshot_free(as); monitor_snapshot_free(bs);
    close_fixture(&a); close_fixture(&b);
    printf("SOURCE_IDENTITY zero_windows=ok cross_engine_snapshot=refused normal_complete=ok\n");
}

static void duplicate_diagnostics(void)
{
    Fixture f;
    MonitorCommitRef ref;
    MonitorSnapshot *s = NULL;
    MonitorCommittedWindows result = {0};
    unsigned step;
    initialize(&f, 0);
    f.rows[1].pid = f.rows[0].pid;
    f.rows[1].creation_time_utc_100ns = f.rows[0].creation_time_utc_100ns;
    f.rows[1].cpu_percent = 80;
    for (step = 0; step <= 20; ++step) {
        monitor_snapshot_free(s); s = NULL;
        set_time(&f, step);
        ++f.rows[1].measured_at_utc_100ns;
        f.batch.finished_at_utc_100ns = f.rows[1].measured_at_utc_100ns;
        CHECK(monitor_ingest_with_commit_ref(f.engine, &f.batch, f.paths, &s, &ref));
        if (!s) { printf("DUPLICATE INGEST %s\n", monitor_error(f.engine)); exit(2); }
    }
    CHECK(s->completed_windows == 2);
    CHECK(monitor_source_read_committed(f.engine, &ref, s, &result)); CHECK(result.count == 2);
    if (result.count == 2) {
        CHECK(result.items[0].row_index == 0 && result.items[1].row_index == 0);
        CHECK(result.items[0].row == &s->rows[0] && result.items[1].row == &s->rows[0]);
        CHECK((result.items[0].match_flags & MONITOR_SOURCE_DUPLICATE_TUPLE) != 0);
        CHECK((result.items[1].match_flags & MONITOR_SOURCE_DUPLICATE_TUPLE) != 0);
        CHECK((result.items[1].match_flags & MONITOR_SOURCE_METADATA_MISMATCH) != 0);
    }
    monitor_source_windows_free(&result); monitor_snapshot_free(s); close_fixture(&f);
    printf("SOURCE_DUPLICATE legacy_row0=preserved metadata_ambiguity=flagged\n");
}

static void epoch_exhaustion(void)
{
    Fixture f;
    MonitorCommitRef last, exhausted, after_reset, zero = {0};
    MonitorSnapshot *first, *second, *third;
    initialize(&f, 0);
    /* This test-only seam is used on a fresh engine, before any receipt exists. */
    monitor_fixture_set_commit_epoch(f.engine, UINT64_MAX - 1);
    first = tick(&f, 0, &last);
    CHECK(last.engine_cookie != 0 && last.mutation_epoch == UINT64_MAX);
    accepted(f.engine, &last, first);
    memset(&exhausted, 0xff, sizeof(exhausted));
    second = tick(&f, 1, &exhausted);
    CHECK(memcmp(&exhausted, &zero, sizeof(zero)) == 0);
    CHECK(second->sequence == first->sequence + 1);
    CHECK(scalar(monitor_fixture_db(f.engine), "SELECT count(*) FROM collection_run") == 2);
    rejected(f.engine, &last, first); rejected(f.engine, &exhausted, second);
    monitor_reset(f.engine);
    memset(&after_reset, 0xff, sizeof(after_reset));
    third = tick(&f, 2, &after_reset);
    CHECK(memcmp(&after_reset, &zero, sizeof(zero)) == 0);
    CHECK(third->sequence == second->sequence + 1 && third->completed_windows == 0);
    CHECK(scalar(monitor_fixture_db(f.engine), "SELECT count(*) FROM collection_run") == 3);
    rejected(f.engine, &after_reset, third); idle(&f);
    monitor_snapshot_free(first); monitor_snapshot_free(second); monitor_snapshot_free(third); close_fixture(&f);
    printf("SOURCE_EPOCH last_max_ref=valid saturated_core_commits=2 zero_refs=2 reset_keeps_disabled=1 wrap=0\n");
}

static void invalidation(unsigned kind)
{
    Fixture f;
    MonitorCommitRef old, next_ref;
    MonitorSnapshot *s, *out = NULL;
    sqlite3 *db;
    const char *const pair = "0123456789abcdef0123456789abcdef";
    initialize(&f, 0);
    if (kind == 8) CHECK(monitor_source_create_pair(f.engine, pair));
    s = tick(&f, 0, &old); db = monitor_fixture_db(f.engine);
    memset(&next_ref, 0xff, sizeof(next_ref)); set_time(&f, 1);
    switch (kind) {
    case 0: CHECK(!monitor_ingest_with_commit_ref(f.engine, NULL, f.paths, &out, &next_ref)); break;
    case 1: CHECK(!monitor_ingest_with_commit_ref(f.engine, &f.batch, f.paths, NULL, &next_ref)); break;
    case 2:
        sql(db, "CREATE TEMP TRIGGER review_fail_raw BEFORE INSERT ON raw BEGIN SELECT RAISE(ABORT,'independent core failure'); END");
        CHECK(!monitor_ingest_with_commit_ref(f.engine, &f.batch, f.paths, &out, &next_ref));
        sql(db, "DROP TRIGGER review_fail_raw"); break;
    case 3: monitor_reset(f.engine); break;
    case 4: CHECK(monitor_feedback(f.engine, s->rows[0].program_id, MONITOR_FEEDBACK_EXPECTED, f.batch.finished_at_utc_100ns)); break;
    case 5: CHECK(monitor_record_failure(f.engine, f.batch.finished_at_utc_100ns, 5)); break;
    case 6: CHECK(monitor_source_create_pair(f.engine, pair)); CHECK(monitor_source_pair_matches(f.source, pair));
        CHECK(!monitor_source_pair_matches(f.source, "fedcba9876543210fedcba9876543210")); break;
    case 7: CHECK(!monitor_feedback(f.engine, -1, MONITOR_FEEDBACK_IGNORE, f.batch.finished_at_utc_100ns)); break;
    case 8: CHECK(!monitor_source_create_pair(f.engine, "fedcba9876543210fedcba9876543210")); break;
    case 9: CHECK(!monitor_record_failure(f.engine, UINT64_MAX, 5)); break;
    case 10: CHECK(monitor_ingest(f.engine, &f.batch, f.paths, &out)); break;
    case 11:
        sql(db, "CREATE TEMP TRIGGER review_fail_feedback BEFORE INSERT ON feedback BEGIN SELECT RAISE(ABORT,'independent feedback failure'); END");
        CHECK(!monitor_feedback(f.engine, s->rows[0].program_id, MONITOR_FEEDBACK_IGNORE, f.batch.finished_at_utc_100ns));
        sql(db, "DROP TRIGGER review_fail_feedback"); break;
    default: CHECK(0); break;
    }
    if (kind <= 2) {
        MonitorCommitRef zero = {0};
        CHECK(memcmp(&next_ref, &zero, sizeof(zero)) == 0); CHECK(out == NULL);
    }
    rejected(f.engine, &old, s); idle(&f);
    monitor_snapshot_free(out); monitor_snapshot_free(s); close_fixture(&f);
    printf("SOURCE_INVALIDATION kind=%u stale=refused\n", kind);
}

static void missing_source(unsigned kind)
{
    Fixture f;
    MonitorCommitRef ref;
    MonitorSnapshot *s = NULL;
    unsigned i;
    initialize(&f, 0);
    for (i = 0; i <= (kind ? 20u : 0u); ++i) {
        monitor_snapshot_free(s); s = tick(&f, i, &ref);
    }
    accepted(f.engine, &ref, s);
    /* Simulate an external deletion on a disposable synthetic DB. The facade
     * does not promise to stop another writer, but must detect missing source. */
    sql(monitor_fixture_db(f.engine), "PRAGMA foreign_keys=OFF");
    if (kind == 2)
        sql(monitor_fixture_db(f.engine), "DELETE FROM aggregate_window WHERE id=(SELECT max(id) FROM aggregate_window)");
    else sql(monitor_fixture_db(f.engine), "DELETE FROM collection_run");
    rejected(f.engine, &ref, s);
    monitor_snapshot_free(s); close_fixture(&f);
    printf("SOURCE_DELETED kind=%u completed=%u refused=1\n", kind, kind ? 2u : 0u);
}

typedef struct QueryFailure { int commit_seen, mode, armed, interrupted; unsigned denied, rows; } QueryFailure;
static int trace(unsigned event, void *context, void *statement, void *detail)
{
    QueryFailure *f = context;
    const char *text = sqlite3_sql((sqlite3_stmt *)statement);
    (void)detail;
    if (!text) return 0;
    if (event == SQLITE_TRACE_STMT && strcmp(text, "COMMIT") == 0) f->commit_seen = 1;
    if (f->mode == 2 && f->commit_seen && event == SQLITE_TRACE_ROW && strstr(text, "aggregate_window") &&
        strstr(text, "ORDER BY")) { ++f->rows; if (!f->interrupted) f->armed = 1; }
    return 0;
}
static int progress(void *context)
{
    QueryFailure *f = context;
    if (f->armed && !f->interrupted) { f->armed = 0; f->interrupted = 1; return 1; }
    return 0;
}
static int authorize(void *context, int action, const char *one, const char *two, const char *db, const char *trigger)
{
    QueryFailure *f = context;
    (void)two; (void)db; (void)trigger;
    if (f->mode == 1 && f->commit_seen && action == SQLITE_READ && one && strcmp(one, "aggregate_window") == 0) {
        ++f->denied; return SQLITE_DENY;
    }
    return SQLITE_OK;
}
static void postcommit_failure(unsigned mode)
{
    Fixture f;
    QueryFailure injection = {0};
    MonitorSnapshot *s = NULL;
    sqlite3 *db, *side;
    int64_t runs, raw, windows, operations, decisions;
    unsigned step, before_hits = source_oom_hits;
    initialize(&f, 1); db = monitor_fixture_db(f.engine); side = experiment_fixture_db(f.experiment);
    for (step = 0; step < 20; ++step) {
        set_time(&f, step); CHECK(experiment_ingest(f.experiment, &f.batch, f.paths, &s));
        monitor_snapshot_free(s); s = NULL;
    }
    runs = scalar(db, "SELECT count(*) FROM collection_run"); raw = scalar(db, "SELECT count(*) FROM raw");
    windows = scalar(db, "SELECT count(*) FROM aggregate_window"); operations = scalar(side, "SELECT count(*) FROM operation");
    decisions = scalar(side, "SELECT count(*) FROM decision");
    injection.mode = (int)mode;
    CHECK(sqlite3_trace_v2(db, SQLITE_TRACE_STMT | SQLITE_TRACE_ROW, trace, &injection) == SQLITE_OK);
    CHECK(sqlite3_set_authorizer(db, authorize, &injection) == SQLITE_OK);
    sqlite3_progress_handler(db, 1, progress, &injection);
    if (!mode) fail_source_allocation = 1;
    set_time(&f, 20); CHECK(!experiment_ingest(f.experiment, &f.batch, f.paths, &s)); CHECK(s == NULL);
    sqlite3_progress_handler(db, 0, NULL, NULL);
    CHECK(sqlite3_set_authorizer(db, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_trace_v2(db, 0, NULL, NULL) == SQLITE_OK);
    CHECK(injection.commit_seen);
    if (!mode) CHECK(source_oom_hits == before_hits + 1 && !fail_source_allocation);
    if (mode == 1) CHECK(injection.denied == 1);
    if (mode == 2) CHECK(injection.rows >= 1 && injection.interrupted);
    CHECK(scalar(db, "SELECT count(*) FROM collection_run") == runs + 1);
    CHECK(scalar(db, "SELECT count(*) FROM raw") == raw + 2);
    CHECK(scalar(db, "SELECT count(*) FROM aggregate_window") == windows + 2);
    CHECK(scalar(side, "SELECT count(*) FROM operation") == operations);
    CHECK(scalar(side, "SELECT count(*) FROM decision") == decisions);
    CHECK(experiment_fixture_fault(f.experiment)); CHECK(source_owned_count == 0); idle(&f);
    CHECK(sqlite3_get_autocommit(side)); CHECK(sqlite3_next_stmt(side, NULL) == NULL);
    set_time(&f, 21); CHECK(!experiment_ingest(f.experiment, &f.batch, f.paths, &s)); CHECK(s == NULL);
    CHECK(scalar(db, "SELECT count(*) FROM collection_run") == runs + 1);
    CHECK(experiment_reset(f.experiment, f.batch.finished_at_utc_100ns, "independent source failure reset"));
    CHECK(experiment_ingest(f.experiment, &f.batch, f.paths, &s));
    CHECK(s && s->completed_windows == 0); monitor_snapshot_free(s);
    CHECK(scalar(db, "SELECT count(*) FROM collection_run") == runs + 2);
    close_fixture(&f);
    printf("SOURCE_POSTCOMMIT mode=%u alloc_hits=%u prepare_denied=%u partial_rows=%u interrupted=%d core_once=1 rollback_fault_reset=1\n",
        mode, source_oom_hits - before_hits, injection.denied, injection.rows, injection.interrupted);
}

int main(int argc, char **argv)
{
    unsigned i;
    if (argc != 2) return 2;
    directory = argv[1];
    reference_identity(); duplicate_diagnostics(); epoch_exhaustion();
    for (i = 0; i < 12; ++i) invalidation(i);
    missing_source(0); missing_source(1); missing_source(2);
    for (i = 0; i < 3; ++i) postcommit_failure(i);
    CHECK(source_owned_count == 0);
    printf("SOURCE_REVIEW checks=%u failures=%u source_oom_hits=%u outstanding=%zu\n", checks, failures, source_oom_hits, source_owned_count);
    return failures ? 1 : 0;
}
