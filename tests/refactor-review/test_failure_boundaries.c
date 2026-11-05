#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../support/monitor_fixture.h"
#include "experiment.h"

#define TICKS MONITOR_TICKS_PER_SECOND
#define DAY MONITOR_DAY_TICKS
static unsigned checks, failures, fail_malloc, malloc_hits, serial;
static void *owned[1024];
static unsigned owned_count;
static const char *output_dir;
static int expect_retention_defect;
#define CHECK(v) do { ++checks; if (!(v)) { ++failures; printf("FAIL line=%u %s\n", (unsigned)__LINE__, #v); } } while (0)

/* Only engine/storage are compiled with these replacements. SQLite, the
 * prototype, and this test retain their usual allocation functions. */
void *review_malloc(size_t size)
{
    void *p;
    if (fail_malloc && --fail_malloc == 0) { ++malloc_hits; return NULL; }
    p = malloc(size);
    if (p) { if (owned_count == 1024) exit(3); owned[owned_count++] = p; }
    return p;
}
void review_free(void *p)
{
    unsigned i;
    for (i = 0; i < owned_count; ++i)
        if (owned[i] == p) { owned[i] = owned[--owned_count]; break; }
    free(p);
}
static void path(char *out, size_t size, const char *label)
{
    snprintf(out, size, "%s/%lu-%u-%s.sqlite", output_dir,
        (unsigned long)GetCurrentProcessId(), ++serial, label);
    CHECK(GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES);
}
static int64_t number(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL;
    int64_t result = -1;
    CHECK(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
    if (s && sqlite3_step(s) == SQLITE_ROW) result = sqlite3_column_int64(s, 0);
    else CHECK(0);
    sqlite3_finalize(s);
    return result;
}
static void make_batch(ResourceBatch *b, ResourceData r[2], uint64_t utc)
{
    unsigned i;
    memset(b, 0, sizeof(*b)); memset(r, 0, 2 * sizeof(*r));
    b->items = r; b->count = 2; b->logical_processor_count = 8;
    b->started_at_utc_100ns = b->finished_at_utc_100ns = utc;
    for (i = 0; i < 2; ++i) {
        r[i].pid = 9100 + i; r[i].creation_time_utc_100ns = utc - 3600 * TICKS;
        r[i].measured_at_utc_100ns = utc;
        r[i].process_name_utf8 = i ? "second.exe" : "first.exe";
        r[i].valid_fields = RESOURCE_VALID_IDENTITY | RESOURCE_VALID_NAME | RESOURCE_VALID_MEMORY;
        r[i].working_set_bytes = 123456 + i;
    }
}
static void no_busy_statements(MonitorEngine *e)
{
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_get_autocommit(monitor_fixture_db(e)));
    while ((s = sqlite3_next_stmt(monitor_fixture_db(e), s)) != NULL) CHECK(!sqlite3_stmt_busy(s));
}
static void test_string_allocation_failures(int reuse)
{
    unsigned fail;
    for (fail = 1; fail <= 6; ++fail) {
        MonitorEngine *e = NULL;
        MonitorSnapshot *old = NULL, *out = NULL;
        ResourceData rows[2]; ResourceBatch b;
        const char *paths[2] = { "SYNTHETIC:\\review\\first.exe", "SYNTHETIC:\\review\\second.exe" };
        char file[512], error[256];
        uint64_t utc = monitor_utc_now();
        int64_t runs, raw, programs;
        path(file, sizeof(file), reuse ? "reused-strings" : "fresh-strings");
        CHECK(monitor_open(file, 1, &e, error, sizeof(error)));
        if (!e) exit(2);
        make_batch(&b, rows, utc);
        if (reuse) {
            CHECK(monitor_ingest(e, &b, paths, &old));
            CHECK(old && old->count == 2);
            rows[0].measured_at_utc_100ns += 3 * TICKS;
            rows[1].measured_at_utc_100ns += 3 * TICKS;
            b.started_at_utc_100ns = b.finished_at_utc_100ns = rows[0].measured_at_utc_100ns;
        }
        runs = number(monitor_fixture_db(e), "SELECT count(*) FROM collection_run");
        raw = number(monitor_fixture_db(e), "SELECT count(*) FROM raw");
        programs = number(monitor_fixture_db(e), "SELECT count(*) FROM program");
        malloc_hits = 0; fail_malloc = fail;
        CHECK(!monitor_ingest(e, &b, paths, &out));
        fail_malloc = 0;
        CHECK(malloc_hits == 1); CHECK(out == NULL);
        CHECK(strcmp(monitor_error(e), "Out of memory; window/streak state discarded") == 0);
        CHECK(monitor_fixture_instance_count(e) == 0 && monitor_fixture_instances_empty(e) && monitor_fixture_cache_empty(e));
        CHECK(number(monitor_fixture_db(e), "SELECT count(*) FROM collection_run") == runs);
        CHECK(number(monitor_fixture_db(e), "SELECT count(*) FROM raw") == raw);
        CHECK(number(monitor_fixture_db(e), "SELECT count(*) FROM program") == programs);
        CHECK(number(monitor_fixture_db(e), "SELECT count(*) FROM aggregate_window") == 0);
        no_busy_statements(e);
        if (old) {
            CHECK(strcmp(old->rows[0].name, "first.exe") == 0);
            CHECK(strcmp(old->rows[1].path, paths[1]) == 0);
            CHECK(old->rows[0].raw.process_name_utf8 == old->rows[0].name);
            CHECK(owned_count == 4);
        } else CHECK(owned_count == 0);
        monitor_snapshot_free(old);
        CHECK(owned_count == 0);
        CHECK(monitor_ingest(e, &b, paths, &out));
        CHECK(out && out->completed_windows == 0 && out->count == 2);
        CHECK(out && out->sequence == (uint64_t)(reuse ? 2 : 1));
        CHECK(number(monitor_fixture_db(e), "SELECT count(*) FROM collection_run") == runs + 1);
        CHECK(number(monitor_fixture_db(e), "SELECT count(*) FROM raw") == raw + 2);
        monitor_snapshot_free(out);
        monitor_close(e);
        CHECK(owned_count == 0);
        printf("STRING_FAILURE reuse=%d allocation=%u rollback_reset_resume=ok path=%s\n", reuse, fail, file);
    }
}
static void test_open_path_allocation_failure(void)
{
    MonitorEngine *e = NULL;
    char file[512], error[256];
    path(file, sizeof(file), "open-path-oom");
    malloc_hits = 0; fail_malloc = 1;
    CHECK(!monitor_open(file, 1, &e, error, sizeof(error)));
    fail_malloc = 0;
    CHECK(malloc_hits == 1 && e == NULL && owned_count == 0);
    CHECK(strcmp(error, "A local disk UTF-8 DB path is required") == 0);
    CHECK(GetFileAttributesA(file) == INVALID_FILE_ATTRIBUTES);
}
static void test_episode_only_retention(void)
{
    Experiment *e = NULL;
    ExperimentEpisode *episodes = NULL;
    ExperimentEvaluation evaluation;
    MonitorSnapshot *snapshot = NULL;
    ResourceData rows[2]; ResourceBatch b;
    const char *paths[2] = { "SYNTHETIC:\\review\\first.exe", "SYNTHETIC:\\review\\second.exe" };
    sqlite3 *db = NULL;
    char source[512], sidecar[512], error[512];
    uint64_t now = monitor_utc_now(), utc = now - 29 * DAY;
    unsigned i; size_t count = 0; int64_t episode_id;
    path(source, sizeof(source), "episode-source"); path(sidecar, sizeof(sidecar), "episode-sidecar");
    CHECK(experiment_create(source, sidecar, 1, &e, error, sizeof(error)));
    if (!e) exit(2);
    make_batch(&b, rows, utc); b.count = 1;
    rows[0].cpu_percent = 40;
    rows[0].valid_fields |= RESOURCE_VALID_CPU | RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL;
    rows[0].observation_interval_seconds = rows[0].cpu_interval_seconds = rows[0].io_interval_seconds = 3;
    for (i = 0; i <= 60; ++i) {
        rows[0].measured_at_utc_100ns = utc + (uint64_t)i * 3 * TICKS;
        b.started_at_utc_100ns = b.finished_at_utc_100ns = rows[0].measured_at_utc_100ns;
        CHECK(experiment_ingest(e, &b, paths, &snapshot));
        monitor_snapshot_free(snapshot); snapshot = NULL;
    }
    CHECK(experiment_finish(e, b.finished_at_utc_100ns));
    CHECK(experiment_episodes(e, EXPERIMENT_FIXED, 100, 0, &episodes, &count));
    CHECK(count == 1);
    episode_id = count ? episodes[0].id : 0;
    experiment_episodes_free(episodes, count);
    memset(&evaluation, 0, sizeof(evaluation));
    evaluation.episode_id = episode_id;
    evaluation.trial_id = "retained-latest-episode-only"; evaluation.order_id = "review";
    evaluation.judgment = EXPERIMENT_UNSURE;
    evaluation.understood = evaluation.target_correct = evaluation.time_correct = evaluation.metric_correct = evaluation.next_check = -1;
    evaluation.elapsed_seconds = -1;
    CHECK(experiment_evaluate(e, &evaluation));
    /* A current empty synthetic capture runs normal retention without extending
     * the already ended episode. Evaluation.created uses actual current UTC. */
    b.count = 0; b.started_at_utc_100ns = b.finished_at_utc_100ns = now;
    CHECK(experiment_ingest(e, &b, paths, &snapshot));
    monitor_snapshot_free(snapshot);
    experiment_close(e);
    CHECK(sqlite3_open_v2(sidecar, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
    CHECK(number(db, "SELECT count(*) FROM episode") == 0);
    CHECK(number(db, "SELECT count(*) FROM evaluation") == expect_retention_defect);
    CHECK(number(db, "SELECT count(*) FROM evaluation e WHERE e.decision IS NULL AND NOT EXISTS(SELECT 1 FROM episode p WHERE p.id=e.episode)") == expect_retention_defect);
    CHECK(number(db, "SELECT count(*) FROM pragma_foreign_key_check") == 0);
    sqlite3_close(db);
    CHECK(owned_count == 0);
    printf("RETENTION_REGRESSION expected_episode_only_orphan=%d episode_id=%lld sidecar=%s\n", expect_retention_defect, (long long)episode_id, sidecar);
}
int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) return 2;
    if (argc == 3) {
        if (strcmp(argv[2], "--expect-retention-defect") != 0) return 2;
        expect_retention_defect = 1;
    }
    output_dir = argv[1];
    test_open_path_allocation_failure();
    test_string_allocation_failures(0);
    test_string_allocation_failures(1);
    test_episode_only_retention();
    printf("REVIEW checks=%u failures=%u outstanding_engine_storage_malloc=%u\n", checks, failures, owned_count);
    return failures ? 1 : 0;
}
