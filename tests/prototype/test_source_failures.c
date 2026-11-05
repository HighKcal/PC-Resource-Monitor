/* 실제 source facade와 source 할당 경로를 사용하는 prototype 소비자 시험이다.
 * coordinator의 읽기 호출을 감싸 core commit 뒤 SQL 실패만 설치한다.
 * 모든 DB는 새 합성 시험용이며 앱·사용자 DB를 사용하지 않는다. */
#include "../support/experiment_fixture.h"
#include "monitor_source.h"

enum { NORMAL, ALLOCATION, PREPARE, STEP, SIDECAR_COPY };
static unsigned checks, failures, serial, mode, allocation_failures, source_allocations, source_frees;
static unsigned prepare_hits, step_hits, copy_hits, owned_count;
static void *owned[16];
static Experiment *active_experiment;
static const char *output_dir;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line=%d %s\n", __LINE__, #x); } } while (0)

void *prototype_test_source_calloc(size_t count, size_t size)
{
    void *p;
    if (mode == ALLOCATION) { ++allocation_failures; return NULL; }
    p = calloc(count, size);
    if (p) {
        ++source_allocations;
        if (owned_count == sizeof(owned) / sizeof(owned[0])) exit(3);
        owned[owned_count++] = p;
    }
    return p;
}
void prototype_test_source_free(void *p)
{
    unsigned i;
    for (i = 0; i < owned_count; ++i)
        if (owned[i] == p) { owned[i] = owned[--owned_count]; ++source_frees; break; }
    free(p);
}
static int deny(void *context, int action, const char *one, const char *two, const char *db, const char *trigger)
{
    unsigned *hits = context;
    (void)two; (void)db; (void)trigger;
    if (one && ((mode == PREPARE && action == SQLITE_READ && !strcmp(one, "aggregate_window")) ||
                (mode == SIDECAR_COPY && action == SQLITE_INSERT && !strcmp(one, "metric_evidence")))) {
        ++*hits;
        return SQLITE_DENY;
    }
    return SQLITE_OK;
}
static int interrupt_after_row(unsigned event, void *context, void *statement, void *detail)
{
    sqlite3 *db = context;
    const char *sql = sqlite3_sql(statement);
    (void)detail;
    if (event == SQLITE_TRACE_ROW && sql && strstr(sql, "aggregate_window") && !step_hits) {
        ++step_hits;
        sqlite3_interrupt(db);
    }
    return 0;
}
int prototype_test_source_read(MonitorEngine *engine, const MonitorCommitRef *ref,
                               const MonitorSnapshot *snapshot, MonitorCommittedWindows *out)
{
    sqlite3 *core = experiment_fixture_core_db(active_experiment);
    int ok;
    if (mode == PREPARE) CHECK(sqlite3_set_authorizer(core, deny, &prepare_hits) == SQLITE_OK);
    if (mode == STEP) CHECK(sqlite3_trace_v2(core, SQLITE_TRACE_ROW, interrupt_after_row, core) == SQLITE_OK);
    if (mode == SIDECAR_COPY)
        CHECK(sqlite3_set_authorizer(experiment_fixture_db(active_experiment), deny, &copy_hits) == SQLITE_OK);
    ok = monitor_source_read_committed(engine, ref, snapshot, out);
    if (mode == PREPARE) CHECK(sqlite3_set_authorizer(core, NULL, NULL) == SQLITE_OK);
    if (mode == STEP) CHECK(sqlite3_trace_v2(core, 0, NULL, NULL) == SQLITE_OK);
    return ok;
}
typedef struct Fixture {
    Experiment *e;
    ResourceData rows[2];
    ResourceBatch batch;
    uint64_t base;
    char source[512], side[512];
} Fixture;
static int64_t scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL;
    int64_t value = -1;
    CHECK(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
    if (s && sqlite3_step(s) == SQLITE_ROW) value = sqlite3_column_int64(s, 0); else CHECK(0);
    sqlite3_finalize(s);
    return value;
}
static void create(Fixture *f)
{
    char error[512];
    unsigned i;
    memset(f, 0, sizeof(*f));
    snprintf(f->source, sizeof(f->source), "%s/%lu-%u-source.sqlite", output_dir,
             (unsigned long)GetCurrentProcessId(), ++serial);
    snprintf(f->side, sizeof(f->side), "%s/%lu-%u-side.sqlite", output_dir,
             (unsigned long)GetCurrentProcessId(), ++serial);
    CHECK(experiment_create(f->source, f->side, 1, &f->e, error, sizeof(error)));
    if (!f->e) { printf("CREATE %s\n", error); exit(2); }
    active_experiment = f->e;
    f->base = monitor_utc_now() - DAY;
    f->batch.items = f->rows; f->batch.count = 2; f->batch.logical_processor_count = 8;
    for (i = 0; i < 2; ++i) {
        ResourceData *row = &f->rows[i];
        row->pid = 4242 + i; row->creation_time_utc_100ns = f->base - DAY;
        row->process_name_utf8 = "synthetic-source-failure.exe";
        row->valid_fields = RESOURCE_VALID_IDENTITY | RESOURCE_VALID_NAME | RESOURCE_VALID_CPU | RESOURCE_VALID_MEMORY |
            RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL;
        row->cpu_percent = 40 + i; row->working_set_bytes = 67108864;
        row->observation_interval_seconds = row->cpu_interval_seconds = row->io_interval_seconds = 3;
    }
}
static int tick(Fixture *f, unsigned tick, MonitorSnapshot **out)
{
    const char *paths[] = {"SYNTHETIC:\\source-failure-A.exe", "SYNTHETIC:\\source-failure-B.exe"};
    unsigned i;
    for (i = 0; i < 2; ++i) f->rows[i].measured_at_utc_100ns = f->base + (uint64_t)tick * 3 * TICKS;
    f->batch.started_at_utc_100ns = f->batch.finished_at_utc_100ns = f->rows[0].measured_at_utc_100ns;
    return experiment_ingest(f->e, &f->batch, paths, out);
}
static void idle(Fixture *f)
{
    sqlite3 *core = experiment_fixture_core_db(f->e), *side = experiment_fixture_db(f->e);
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_get_autocommit(core)); CHECK(sqlite3_get_autocommit(side));
    CHECK(sqlite3_next_stmt(side, NULL) == NULL);
    while ((s = sqlite3_next_stmt(core, s)) != NULL) CHECK(!sqlite3_stmt_busy(s));
    CHECK(owned_count == 0);
}
static void source_failure(unsigned failure_mode)
{
    Fixture f;
    MonitorSnapshot *snapshot = NULL;
    int64_t runs, operations;
    unsigned i, allocations_before;
    mode = NORMAL;
    create(&f);
    allocations_before = source_allocations;
    for (i = 0; i < 20; ++i) { CHECK(tick(&f, i, &snapshot)); monitor_snapshot_free(snapshot); snapshot = NULL; }
    CHECK(source_allocations == allocations_before); /* 완료창 0개인 성공 참조는 view 배열을 할당하지 않는다. */
    runs = scalar(experiment_fixture_core_db(f.e), "SELECT count(*) FROM collection_run");
    operations = scalar(experiment_fixture_db(f.e), "SELECT count(*) FROM operation");
    mode = failure_mode;
    CHECK(!tick(&f, 20, &snapshot) && !snapshot);
    mode = NORMAL;
    CHECK(sqlite3_set_authorizer(experiment_fixture_db(f.e), NULL, NULL) == SQLITE_OK);
    CHECK(strstr(experiment_error(f.e), "Core committed; evidence capture FAILED") != NULL);
    CHECK(experiment_fixture_fault(f.e));
    CHECK(scalar(experiment_fixture_core_db(f.e), "SELECT count(*) FROM collection_run") == runs + 1);
    CHECK(scalar(experiment_fixture_core_db(f.e), "SELECT count(*) FROM aggregate_window") == 2);
    CHECK(scalar(experiment_fixture_db(f.e), "SELECT count(*) FROM operation") == operations);
    CHECK(scalar(experiment_fixture_db(f.e), "SELECT count(*) FROM decision") == 0);
    CHECK(!tick(&f, 21, &snapshot) && !snapshot); /* fault 상태에서 이미 core에 commit한 입력의 재시도를 막는다. */
    CHECK(scalar(experiment_fixture_core_db(f.e), "SELECT count(*) FROM collection_run") == runs + 1);
    idle(&f);
    CHECK(experiment_reset(f.e, f.batch.finished_at_utc_100ns, "synthetic source failure reset"));
    for (i = 22; i <= 42; ++i) { CHECK(tick(&f, i, &snapshot)); monitor_snapshot_free(snapshot); snapshot = NULL; }
    CHECK(scalar(experiment_fixture_db(f.e), "SELECT count(*) FROM decision") == 16);
    if (failure_mode == ALLOCATION) {
        /* reset 후 완료창 복사에 성공한 다음 다시 실패시킨다.
           두 번째 rollback에서도 앞서 commit한 실험 결정은 보존되어야 한다. */
        for (i = 43; i < 62; ++i) { CHECK(tick(&f, i, &snapshot)); monitor_snapshot_free(snapshot); snapshot = NULL; }
        mode = ALLOCATION;
        CHECK(!tick(&f, 62, &snapshot) && !snapshot);
        mode = NORMAL;
        CHECK(experiment_fixture_fault(f.e));
        CHECK(scalar(experiment_fixture_db(f.e), "SELECT count(*) FROM decision") == 16);
        CHECK(scalar(experiment_fixture_core_db(f.e), "SELECT count(*) FROM aggregate_window") == 6);
        idle(&f);
        CHECK(experiment_reset(f.e, f.batch.finished_at_utc_100ns, "synthetic repeated source OOM reset"));
        CHECK(tick(&f, 63, &snapshot)); monitor_snapshot_free(snapshot); snapshot = NULL;
    }
    idle(&f); experiment_close(f.e); active_experiment = NULL;
    printf("PROTOTYPE_SOURCE_FAILURE mode=%u core_commit=preserved sidecar_rollback=ok reset_resume=ok view_outstanding=%u\n",
           failure_mode, owned_count);
}
static void partial_diagnostics(void)
{
    Fixture f;
    MonitorSnapshot *snapshot = NULL;
    ExperimentStats before, after;
    mode = NORMAL;
    create(&f); CHECK(tick(&f, 0, &snapshot)); monitor_snapshot_free(snapshot);
    CHECK(experiment_stats(f.e, &before)); CHECK(before.source_available && before.latest_source_available);
    CHECK(sqlite3_exec(experiment_fixture_core_db(f.e), "DROP TABLE raw", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(experiment_stats(f.e, &after));
    CHECK(after.source_available && !after.latest_source_available);
    CHECK(after.latest_source_utc == before.latest_source_utc);
    CHECK(after.latest_source_status == before.latest_source_status);
    CHECK(after.latest_source_rows == before.latest_source_rows);
    CHECK(after.latest_source_raw_rows == 0);
    experiment_close(f.e); active_experiment = NULL;
}
int main(int argc, char **argv)
{
    unsigned m;
    if (argc != 2) return 2;
    output_dir = argv[1];
    for (m = ALLOCATION; m <= SIDECAR_COPY; ++m) source_failure(m);
    partial_diagnostics();
    CHECK(allocation_failures == 2 && prepare_hits == 1 && step_hits == 1 && copy_hits == 1);
    CHECK(owned_count == 0 && source_allocations == source_frees);
    printf("PROTOTYPE_SOURCE_HITS allocation=%u prepare=%u step=%u sidecar_copy=%u allocations=%u frees=%u\n",
           allocation_failures, prepare_hits, step_hits, copy_hits, source_allocations, source_frees);
    printf("PROTOTYPE_SOURCE_RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
