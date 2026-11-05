/* 모듈별 메모리 할당 실패와 공개 ingest의 SQL 실패 경계를 합성 입력으로 검증한다.
 * 실패 시 보존해야 할 상태를 확인하며 비교기 결과의 독립 기대값 계산 시험은 아니다. */
#include "../support/experiment_fixture.h"

enum { COORDINATOR, STORE, HISTORY, REPLAY, ALLOCATORS };
static unsigned checks, failures, serial, hits[ALLOCATORS];
static int fail_module = -1;
static const char *output_dir;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line=%d %s\n", __LINE__, #x); } } while (0)
static void *allocate(unsigned module, size_t count, size_t size, int zero)
{
    if ((int)module == fail_module) { ++hits[module]; fail_module = -1; return NULL; }
    return zero ? calloc(count, size) : malloc(size);
}
#define ALLOCATOR(tag, id) \
    void *prototype_##tag##_malloc(size_t size) { return allocate(id, 1, size, 0); } \
    void *prototype_##tag##_calloc(size_t count, size_t size) { return allocate(id, count, size, 1); }
ALLOCATOR(coordinator, COORDINATOR)
ALLOCATOR(store, STORE)
ALLOCATOR(history, HISTORY)
ALLOCATOR(replay, REPLAY)
static void path(char *out, size_t size, const char *label)
{
    snprintf(out, size, "%s/%lu-%u-%s.sqlite", output_dir, (unsigned long)GetCurrentProcessId(), ++serial, label);
    CHECK(GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES);
}
static int64_t scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL;
    int64_t value = -1;
    CHECK(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
    if (s && sqlite3_step(s) == SQLITE_ROW) value = sqlite3_column_int64(s, 0); else CHECK(0);
    sqlite3_finalize(s);
    return value;
}
typedef struct Fixture {
    Experiment *e;
    ResourceData row;
    ResourceBatch batch;
    char source[512], side[512];
    uint64_t base;
} Fixture;
static void create(Fixture *f)
{
    char error[512];
    memset(f, 0, sizeof(*f));
    path(f->source, sizeof(f->source), "source"); path(f->side, sizeof(f->side), "sidecar");
    CHECK(experiment_create(f->source, f->side, 1, &f->e, error, sizeof(error)));
    if (!f->e) { printf("CREATE %s\n", error); exit(2); }
    f->base = monitor_utc_now() - DAY;
    f->batch.items = &f->row; f->batch.count = 1; f->batch.logical_processor_count = 8;
    f->row.pid = 4242; f->row.creation_time_utc_100ns = f->base - DAY;
    f->row.process_name_utf8 = "synthetic-module-failure.exe";
    f->row.valid_fields = RESOURCE_VALID_IDENTITY | RESOURCE_VALID_NAME | RESOURCE_VALID_CPU | RESOURCE_VALID_MEMORY |
        RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL;
    f->row.cpu_percent = 40; f->row.working_set_bytes = 67108864;
    f->row.observation_interval_seconds = f->row.cpu_interval_seconds = f->row.io_interval_seconds = 3;
}
static int tick(Fixture *f, unsigned tick, MonitorSnapshot **out)
{
    const char *paths[] = {"SYNTHETIC:\\module-failure.exe"};
    f->row.measured_at_utc_100ns = f->base + (uint64_t)tick * 3 * TICKS;
    f->batch.started_at_utc_100ns = f->batch.finished_at_utc_100ns = f->row.measured_at_utc_100ns;
    return experiment_ingest(f->e, &f->batch, paths, out);
}
static void idle(Fixture *f)
{
    sqlite3_stmt *s = NULL;
    sqlite3 *side = experiment_fixture_db(f->e), *core = experiment_fixture_core_db(f->e);
    CHECK(sqlite3_get_autocommit(side)); CHECK(sqlite3_get_autocommit(core));
    CHECK(sqlite3_next_stmt(side, NULL) == NULL);
    while ((s = sqlite3_next_stmt(core, s)) != NULL) CHECK(!sqlite3_stmt_busy(s));
}
static void allocator_failures(void)
{
    Fixture f;
    Experiment *other = NULL;
    ExperimentDecision *rows = NULL;
    MonitorSnapshot *snapshot = NULL;
    size_t count = 0;
    char source[512], side[512], replay[512], error[512];
    path(source, sizeof(source), "store-oom-source"); path(side, sizeof(side), "store-oom-side");
    fail_module = STORE;
    CHECK(!experiment_create(source, side, 1, &other, error, sizeof(error)) && !other);
    CHECK(hits[STORE] == 1); CHECK(GetFileAttributesA(source) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesA(side) == INVALID_FILE_ATTRIBUTES);
    create(&f);
    fail_module = COORDINATOR;
    CHECK(!tick(&f, 0, &snapshot) && !snapshot);
    CHECK(hits[COORDINATOR] == 1); CHECK(!experiment_fixture_fault(f.e));
    CHECK(scalar(experiment_fixture_core_db(f.e), "SELECT count(*) FROM collection_run") == 0);
    CHECK(tick(&f, 0, &snapshot)); monitor_snapshot_free(snapshot); snapshot = NULL;
    fail_module = STORE;
    CHECK(!experiment_open_history(f.side, &other, error, sizeof(error)) && !other);
    CHECK(hits[STORE] == 2);
    fail_module = HISTORY;
    CHECK(!experiment_decisions(f.e, EXPERIMENT_FIXED, 1, 0, &rows, &count));
    CHECK(!rows && count == 0 && hits[HISTORY] == 1);
    CHECK(experiment_decisions(f.e, EXPERIMENT_FIXED, 1, 0, &rows, &count));
    experiment_decisions_free(rows, count);
    path(replay, sizeof(replay), "replay-oom");
    fail_module = REPLAY;
    CHECK(!experiment_replay(f.side, replay, error, sizeof(error)));
    CHECK(hits[REPLAY] == 1 && GetFileAttributesA(replay) == INVALID_FILE_ATTRIBUTES);
    idle(&f); experiment_close(f.e);
    printf("MODULE_ALLOCATOR_HITS coordinator=%u store=%u history=%u replay=%u\n", hits[0], hits[1], hits[2], hits[3]);
}
typedef struct Denial { const char *table; int action; unsigned hits; } Denial;
static int deny_sql(void *context, int action, const char *one, const char *two, const char *db, const char *trigger)
{
    Denial *denial = context;
    (void)two; (void)db; (void)trigger;
    if (action == denial->action && one && !strcmp(one, denial->table)) { ++denial->hits; return SQLITE_DENY; }
    return SQLITE_OK;
}
static void sql_failure(const char *table)
{
    Fixture f;
    MonitorSnapshot *snapshot = NULL;
    ExperimentDecision *rows = NULL;
    Denial denial = {table, SQLITE_INSERT, 0};
    unsigned i;
    size_t count = 0;
    int64_t source_runs, operations, decisions;
    create(&f);
    for (i = 0; i < 60; ++i) { CHECK(tick(&f, i, &snapshot)); monitor_snapshot_free(snapshot); snapshot = NULL; }
    source_runs = scalar(experiment_fixture_core_db(f.e), "SELECT count(*) FROM collection_run");
    operations = scalar(experiment_fixture_db(f.e), "SELECT count(*) FROM operation");
    decisions = scalar(experiment_fixture_db(f.e), "SELECT count(*) FROM decision");
    CHECK(sqlite3_set_authorizer(experiment_fixture_db(f.e), deny_sql, &denial) == SQLITE_OK);
    CHECK(!tick(&f, 60, &snapshot) && !snapshot);
    CHECK(denial.hits == 1); CHECK(experiment_fixture_fault(f.e));
    CHECK(strstr(experiment_error(f.e), "Core committed; evidence capture FAILED") != NULL);
    CHECK(sqlite3_set_authorizer(experiment_fixture_db(f.e), NULL, NULL) == SQLITE_OK);
    CHECK(scalar(experiment_fixture_core_db(f.e), "SELECT count(*) FROM collection_run") == source_runs + 1);
    CHECK(scalar(experiment_fixture_db(f.e), "SELECT count(*) FROM operation") == operations);
    CHECK(scalar(experiment_fixture_db(f.e), "SELECT count(*) FROM decision") == decisions);
    idle(&f);
    CHECK(experiment_reset(f.e, f.batch.finished_at_utc_100ns, "synthetic module failure recovery"));
    CHECK(tick(&f, 61, &snapshot)); monitor_snapshot_free(snapshot);
    denial.table = "decision"; denial.action = SQLITE_READ; denial.hits = 0;
    CHECK(sqlite3_set_authorizer(experiment_fixture_db(f.e), deny_sql, &denial) == SQLITE_OK);
    CHECK(!experiment_decisions(f.e, EXPERIMENT_FIXED, 100, 0, &rows, &count));
    CHECK(denial.hits == 1 && !rows && count == 0);
    CHECK(sqlite3_set_authorizer(experiment_fixture_db(f.e), NULL, NULL) == SQLITE_OK);
    CHECK(experiment_decisions(f.e, EXPERIMENT_FIXED, 100, 0, &rows, &count));
    experiment_decisions_free(rows, count); idle(&f); experiment_close(f.e);
    printf("MODULE_SQL_HITS table=%s insert_denials=1 history_read_denials=%u\n", table, denial.hits);
}
int main(int argc, char **argv)
{
    static const char *const tables[] = {"operation", "window_evidence", "metric_evidence", "decision", "episode", "reduction"};
    unsigned i;
    if (argc != 2) return 2;
    output_dir = argv[1];
    allocator_failures();
    for (i = 0; i < sizeof(tables) / sizeof(tables[0]); ++i) sql_failure(tables[i]);
    printf("MODULE_FAILURE_RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
